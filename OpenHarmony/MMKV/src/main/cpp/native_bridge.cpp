/*
 * Tencent is pleased to support the open source community by making
 * MMKV available.
 *
 * Copyright (C) 2024 THL A29 Limited, a Tencent company.
 * All rights reserved.
 *
 * Licensed under the BSD 3-Clause License (the "License"); you may not use
 * this file except in compliance with the License. You may obtain a copy of
 * the License at
 *
 *       https://opensource.org/licenses/BSD-3-Clause
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "MMKVPredef.h"

#include "MMBuffer.h"
#include "MMKV.h"
#include "MMKVLog.h"
#include "MemoryFile.h"
#include "napi/native_api.h"
#include <cstdint>
#include <string>
#include <sys/stat.h>
#include <system_error>
#include <hilog/log.h>

#define LOG_LEVEL(level, ...) OH_LOG_Print(LOG_APP, level, 0, "mmkv", __VA_ARGS__)
#define MMKV_LOG_DEBUG(...) LOG_LEVEL(LOG_DEBUG, __VA_ARGS__)
#define MMKV_LOG_INFO(...) LOG_LEVEL(LOG_INFO, __VA_ARGS__)
#define MMKV_LOG_WARN(...) LOG_LEVEL(LOG_WARN, __VA_ARGS__)
#define MMKV_LOG_ERROR(...) LOG_LEVEL(LOG_ERROR, __VA_ARGS__)

using namespace std;
using namespace mmkv;

// assuming env is defined
#define NAPI_CALL_RET(call, return_value)                                                                              \
    do {                                                                                                               \
        napi_status status = (call);                                                                                   \
        if (status != napi_ok) {                                                                                       \
            const napi_extended_error_info *error_info = nullptr;                                                      \
            napi_get_last_error_info(env, &error_info);                                                                \
            MMKV_LOG_INFO("NAPI Error: code %{public}d, msg %{public}s\n",                                             \
                error_info->error_code, error_info->error_message);                                                    \
            bool is_pending;                                                                                           \
            napi_is_exception_pending(env, &is_pending);                                                               \
            if (!is_pending) {                                                                                         \
                auto message = error_info->error_message ? error_info->error_message : "null";                         \
                napi_throw_error(env, nullptr, message);                                                               \
                return return_value;                                                                                        \
            }                                                                                                          \
        }                                                                                                              \
    } while (0)

#define NAPI_CALL(call) NAPI_CALL_RET(call, nullptr)

bool IsNValueUndefined(napi_env env, napi_value value) {
    napi_valuetype type;
    if (napi_typeof(env, value, &type) == napi_ok && type == napi_undefined) {
        return true;
    }
    return false;
}

static string NValueToString(napi_env env, napi_value value, bool maybeUndefined = false) {
    if (maybeUndefined && IsNValueUndefined(env, value)) {
        return "";
    }

    string result;
    size_t size;
    NAPI_CALL_RET(napi_get_value_string_utf8(env, value, nullptr, 0, &size), "");
    result.resize(size);
    // MMKV_LOG_INFO("size: %{public}zu, capacity: %{public}zu\n", result.size(), result.capacity());
    NAPI_CALL_RET(napi_get_value_string_utf8(env, value, result.data(), result.capacity(), nullptr), "");
    return result;
}

static napi_value StringToNValue(napi_env env, const string &value) {
    napi_value result;
    napi_create_string_utf8(env, value.data(), value.size(), &result);
    return result;
}

static vector<string> NValueToStringArray(napi_env env, napi_value value, bool maybeUndefined = false) {
    vector<string> keys;
    if (maybeUndefined && IsNValueUndefined(env, value)) {
        return keys;
    }

    uint32_t length = 0;
    if (napi_get_array_length(env, value, &length) != napi_ok || length == 0) {
        return keys;
    }
    keys.reserve(length);

    for (uint32_t index = 0; index < length; index++) {
        napi_value jsKey = nullptr;
        if (napi_get_element(env, value, index, &jsKey) != napi_ok) {
            continue;
        }
        keys.push_back(NValueToString(env, jsKey));
    }
    return keys;
}

static napi_value StringArrayToNValue(napi_env env, const vector<string> &value) {
    napi_value jsArr = nullptr;
    napi_create_array_with_length(env, value.size(), &jsArr);
    for (size_t index = 0; index < value.size(); index++) {
        auto jsKey = StringToNValue(env, value[index]);
        napi_set_element(env, jsArr, index, jsKey);
    }
    return jsArr;
}

static void my_finalizer(napi_env env, void *finalize_data, void *finalize_hint) {
    // MMKVInfo("free %p", finalize_data);
    free(finalize_data);
}

static MMBuffer NValueToMMBuffer(napi_env env, napi_value value, bool maybeUndefined = false) {
    if (maybeUndefined && IsNValueUndefined(env, value)) {
        return MMBuffer();
    }

    void *data = nullptr;
    size_t length = 0;
    if (napi_get_arraybuffer_info(env, value, &data, &length) == napi_ok) {
        return MMBuffer(data, length, mmkv::MMBufferNoCopy);
    }
    return MMBuffer();
}

static napi_value MMBufferToNValue(napi_env env, const MMBuffer &value) {
    napi_value result = nullptr;
    void *data = nullptr;
    if (napi_create_arraybuffer(env, value.length(), &data, &result) == napi_ok) {
        memcpy(data, value.getPtr(), value.length());
    }
    return result;
}

static napi_value MMBufferToNValue(napi_env env, MMBuffer &&value) {
    if (!value.isStoredOnStack()) {
        napi_value result = nullptr;
        auto ret = napi_create_external_arraybuffer(env, value.getPtr(), value.length(), my_finalizer, nullptr, &result);
        if (ret == napi_ok) {
            // MMKVInfo("using napi_create_external_arraybuffer %p", value.getPtr());
            value.detach();
            return result;
        }
    }
    return MMBufferToNValue(env, (const MMBuffer &) value);
}

static napi_value NAPIUndefined(napi_env env) {
    napi_value result = nullptr;
    napi_get_undefined(env, &result);
    return result;
}

static napi_value NAPINull(napi_env env) {
    napi_value result;
    napi_get_null(env, &result);
    return result;
}

static napi_value BoolToNValue(napi_env env, bool value) {
    napi_value result;
    napi_value resultBool;
    napi_create_double(env, value, &result);
    napi_coerce_to_bool(env, result, &resultBool);
    return resultBool;
}

static bool NValueToBool(napi_env env, napi_value value) {
    bool result;
    if (napi_get_value_bool(env, value, &result) == napi_ok) {
        return result;
    }
    return false;
}

static napi_value Int32ToNValue(napi_env env, int32_t value) {
    napi_value result;
    napi_create_int32(env, value, &result);
    return result;
}

static int32_t NValueToInt32(napi_env env, napi_value value) {
    int32_t result;
    napi_get_value_int32(env, value, &result);
    return result;
}

static napi_value UInt32ToNValue(napi_env env, uint32_t value) {
    napi_value result;
    napi_create_uint32(env, value, &result);
    return result;
}

static uint32_t NValueToUInt32(napi_env env, napi_value value) {
    uint32_t result;
    napi_get_value_uint32(env, value, &result);
    return result;
}

static napi_value DoubleToNValue(napi_env env, double value) {
    napi_value result;
    napi_create_double(env, value, &result);
    return result;
}

static double NValueToDouble(napi_env env, napi_value value) {
    double result;
    napi_get_value_double(env, value, &result);
    return result;
}

static napi_value Int64ToNValue(napi_env env, int64_t value) {
    napi_value result;
    napi_create_bigint_int64(env, value, &result);
    return result;
}

static int64_t NValueToInt64(napi_env env, napi_value value) {
    int64_t result;
    bool lossless;
    napi_get_value_bigint_int64(env, value, &result, &lossless);
    return result;
}

static napi_value UInt64ToNValue(napi_env env, uint64_t value) {
    napi_value result;
    napi_create_bigint_uint64(env, value, &result);
    return result;
}

static uint64_t NValueToUInt64(napi_env env, napi_value value) {
    uint64_t result;
    bool lossless;
    napi_get_value_bigint_uint64(env, value, &result, &lossless);
    return result;
}

static napi_value initialize(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3] = {nullptr};
    NAPI_CALL(napi_get_cb_info(env, info, &argc, args , nullptr, nullptr));

    auto rootDir = NValueToString(env, args[0]);
    auto cacheDir = NValueToString(env, args[1]);

    int32_t logLevel;
    NAPI_CALL(napi_get_value_int32(env, args[2], &logLevel));

    MMKV_LOG_INFO("rootDir: %{public}s, cacheDir: %{public}s, log level:%{public}d\n",
        rootDir.c_str(), cacheDir.c_str(), logLevel);

    MMKV::initializeMMKV(rootDir, (MMKVLogLevel) logLevel);
    g_android_tmpDir = cacheDir;

    return StringToNValue(env, MMKV::getRootDir());
}

static napi_value version(napi_env env, napi_callback_info info) {
    napi_value sum;
    NAPI_CALL(napi_create_string_latin1(env, MMKV_VERSION, strlen(MMKV_VERSION), &sum));
    return sum;
}

static napi_value pageSize(napi_env env, napi_callback_info info) {
    napi_value value;
    NAPI_CALL(napi_create_uint32(env, DEFAULT_MMAP_SIZE, &value));
    return value;
}

static napi_value getDefaultMMKV(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = {nullptr};
    NAPI_CALL(napi_get_cb_info(env, info, &argc, args, nullptr, nullptr));

    int32_t mode;
    NAPI_CALL(napi_get_value_int32(env, args[0], &mode));
    auto crypt = NValueToString(env, args[1], true);

    MMKV *kv = nullptr;
    if (crypt.length() > 0) {
        kv = MMKV::defaultMMKV((MMKVMode)mode, &crypt);
    }

    if (!kv) {
        kv = MMKV::defaultMMKV((MMKVMode)mode, nullptr);
    }

    return UInt64ToNValue(env,(uint64_t)kv);
}

static napi_value encodeBool(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value args[4] = {nullptr};
    NAPI_CALL(napi_get_cb_info(env, info, &argc, args, nullptr, nullptr));

    auto handle = NValueToUInt64(env, args[0]);
    auto key = NValueToString(env, args[1]);
    MMKV *kv = reinterpret_cast<MMKV *>(handle);
    if (kv && key.length() > 0) {
        auto value = NValueToBool(env,args[2]);
        if (IsNValueUndefined(env, args[3])) {
            auto ret = kv->set(value, key);
            return BoolToNValue(env, ret);
        }
        uint32_t expiration = NValueToUInt32(env, args[3]);
        auto ret = kv->set(value, key, expiration);
        return BoolToNValue(env, ret);
    }
    return BoolToNValue(env,false);
}

static napi_value decodeBool(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3] = {nullptr};
    NAPI_CALL(napi_get_cb_info(env, info, &argc, args, nullptr, nullptr));

    auto handle = NValueToUInt64(env, args[0]);
    auto key = NValueToString(env, args[1]);
    auto defaultValue = NValueToBool(env, args[2]);
    MMKV *kv = reinterpret_cast<MMKV *>(handle);
    if (kv && key.length() > 0) {
        return BoolToNValue(env, kv->getBool(key, defaultValue));
    }
    return BoolToNValue(env, defaultValue);
}

static napi_value encodeInt32(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value args[4] = {nullptr};
    NAPI_CALL(napi_get_cb_info(env, info, &argc, args, nullptr, nullptr));

    auto handle = NValueToUInt64(env, args[0]);
    auto key = NValueToString(env, args[1]);
    MMKV *kv = reinterpret_cast<MMKV *>(handle);
    if (kv && key.length() > 0) {
        auto value = NValueToInt32(env, args[2]);
        if (IsNValueUndefined(env, args[3])) {
            auto ret = kv->set(value, key);
            return BoolToNValue(env, ret);
        }
        uint32_t expiration = NValueToUInt32(env, args[3]);
        auto ret = kv->set(value, key, expiration);
        return BoolToNValue(env, ret);
    }
    return BoolToNValue(env, false);
}

static napi_value decodeInt32(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3] = {nullptr};
    NAPI_CALL(napi_get_cb_info(env, info, &argc, args, nullptr, nullptr));

    auto handle = NValueToUInt64(env, args[0]);
    auto key = NValueToString(env, args[1]);
    auto defaultValue = NValueToInt32(env, args[2]);
    MMKV *kv = reinterpret_cast<MMKV *>(handle);
    if (kv && key.length() > 0) {
        return Int32ToNValue(env, kv->getInt32(key, defaultValue));
    }
    return Int32ToNValue(env, defaultValue);
}

static napi_value encodeUInt32(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value args[4] = {nullptr};
    NAPI_CALL(napi_get_cb_info(env, info, &argc, args, nullptr, nullptr));

    auto handle = NValueToUInt64(env, args[0]);
    auto key = NValueToString(env, args[1]);
    MMKV *kv = reinterpret_cast<MMKV *>(handle);
    if (kv && key.length() > 0) {
        auto value = NValueToUInt32(env, args[2]);
        if (IsNValueUndefined(env, args[3])) {
            auto ret = kv->set(value, key);
            return BoolToNValue(env, ret);
        }
        uint32_t expiration = NValueToUInt32(env, args[3]);
        auto ret = kv->set(value, key, expiration);
        return BoolToNValue(env, ret);
    }
    return BoolToNValue(env, false);
}

static napi_value decodeUInt32(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3] = {nullptr};
    NAPI_CALL(napi_get_cb_info(env, info, &argc, args, nullptr, nullptr));

    auto handle = NValueToUInt64(env, args[0]);
    auto key = NValueToString(env, args[1]);
    auto defaultValue = NValueToUInt32(env, args[2]);
    MMKV *kv = reinterpret_cast<MMKV *>(handle);
    if (kv && key.length() > 0) {
        return UInt32ToNValue(env, kv->getUInt32(key, defaultValue));
    }
    return UInt32ToNValue(env, defaultValue);
}

static napi_value encodeInt64(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value args[4] = {nullptr};
    NAPI_CALL(napi_get_cb_info(env, info, &argc, args, nullptr, nullptr));

    auto handle = NValueToUInt64(env, args[0]);
    auto key = NValueToString(env, args[1]);
    MMKV *kv = reinterpret_cast<MMKV *>(handle);
    if (kv && key.length() > 0) {
        auto value = NValueToInt64(env, args[2]);
        if (IsNValueUndefined(env, args[3])) {
            auto ret = kv->set(value, key);
            return BoolToNValue(env, ret);
        }
        uint32_t expiration = NValueToUInt32(env, args[3]);
        auto ret = kv->set(value, key, expiration);
        return BoolToNValue(env, ret);
    }
    return BoolToNValue(env, false);
}

static napi_value decodeInt64(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3] = {nullptr};
    NAPI_CALL(napi_get_cb_info(env, info, &argc, args, nullptr, nullptr));

    auto handle = NValueToUInt64(env, args[0]);
    auto key = NValueToString(env, args[1]);
    auto defaultValue = NValueToInt64(env, args[2]);
    MMKV *kv = reinterpret_cast<MMKV *>(handle);
    if (kv && key.length() > 0) {
        return Int64ToNValue(env, kv->getInt64(key, defaultValue));
    }
    return Int64ToNValue(env, defaultValue);
}

static napi_value encodeUInt64(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value args[4] = {nullptr};
    NAPI_CALL(napi_get_cb_info(env, info, &argc, args, nullptr, nullptr));

    auto handle = NValueToUInt64(env, args[0]);
    auto key = NValueToString(env, args[1]);
    MMKV *kv = reinterpret_cast<MMKV *>(handle);
    if (kv && key.length() > 0) {
        auto value = NValueToUInt64(env, args[2]);
        if (IsNValueUndefined(env, args[3])) {
            auto ret = kv->set(value, key);
            return BoolToNValue(env, ret);
        }
        uint32_t expiration = NValueToUInt32(env, args[3]);
        auto ret = kv->set(value, key, expiration);
        return BoolToNValue(env, ret);
    }
    return BoolToNValue(env, false);
}

static napi_value decodeUInt64(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3] = {nullptr};
    NAPI_CALL(napi_get_cb_info(env, info, &argc, args, nullptr, nullptr));

    auto handle = NValueToUInt64(env, args[0]);
    auto key = NValueToString(env, args[1]);
    auto defaultValue = NValueToUInt64(env, args[2]);
    MMKV *kv = reinterpret_cast<MMKV *>(handle);
    if (kv && key.length() > 0) {
        return UInt64ToNValue(env, kv->getUInt64(key, defaultValue));
    }
    return UInt64ToNValue(env, defaultValue);
}

static napi_value encodeDouble(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value args[4] = {nullptr};
    NAPI_CALL(napi_get_cb_info(env, info, &argc, args, nullptr, nullptr));

    auto handle = NValueToUInt64(env, args[0]);
    auto key = NValueToString(env, args[1]);
    MMKV *kv = reinterpret_cast<MMKV *>(handle);
    if (kv && key.length() > 0) {
        auto value = NValueToDouble(env, args[2]);
        if (IsNValueUndefined(env, args[3])) {
            auto ret = kv->set(value, key);
            return BoolToNValue(env, ret);
        }
        uint32_t expiration = NValueToUInt32(env, args[3]);
        auto ret = kv->set(value, key, expiration);
        return BoolToNValue(env, ret);
    }
    return BoolToNValue(env, false);
}

static napi_value decodeDouble(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3] = {nullptr};
    NAPI_CALL(napi_get_cb_info(env, info, &argc, args, nullptr, nullptr));

    auto handle = NValueToUInt64(env, args[0]);
    auto key = NValueToString(env, args[1]);
    auto defaultValue = NValueToDouble(env, args[2]);
    MMKV *kv = reinterpret_cast<MMKV *>(handle);
    if (kv && key.length() > 0) {
        return DoubleToNValue(env, kv->getDouble(key, defaultValue));
    }
    return DoubleToNValue(env, defaultValue);
}

static napi_value encodeString(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value args[4] = {nullptr};
    NAPI_CALL(napi_get_cb_info(env, info, &argc, args, nullptr, nullptr));

    auto handle = NValueToUInt64(env, args[0]);
    auto key = NValueToString(env, args[1]);
    MMKV *kv = reinterpret_cast<MMKV *>(handle);
    if (kv && key.length() > 0) {
        auto value = NValueToString(env, args[2]);
        if (IsNValueUndefined(env, args[3])) {
            auto ret = kv->set(value, key);
            return BoolToNValue(env, ret);
        }
        uint32_t expiration = NValueToUInt32(env, args[3]);
        auto ret = kv->set(value, key, expiration);
        return BoolToNValue(env, ret);
    }
    return BoolToNValue(env, false);
}

static napi_value decodeString(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3] = {nullptr};
    NAPI_CALL(napi_get_cb_info(env, info, &argc, args, nullptr, nullptr));

    auto handle = NValueToUInt64(env, args[0]);
    auto key = NValueToString(env, args[1]);
    MMKV *kv = reinterpret_cast<MMKV *>(handle);
    if (kv && key.length() > 0) {
        string result;
        if (kv->getString(key, result)) {
            return StringToNValue(env, result);
        }
    }
    return args[2];
}

static napi_value encodeStringSet(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value args[4] = {nullptr};
    NAPI_CALL(napi_get_cb_info(env, info, &argc, args, nullptr, nullptr));

    auto handle = NValueToUInt64(env, args[0]);
    auto key = NValueToString(env, args[1]);
    MMKV *kv = reinterpret_cast<MMKV *>(handle);
    if (kv && key.length() > 0) {
        auto value = NValueToStringArray(env, args[2]);
        if (IsNValueUndefined(env, args[3])) {
            auto ret = kv->set(value, key);
            return BoolToNValue(env, ret);
        }
        uint32_t expiration = NValueToUInt32(env, args[3]);
        auto ret = kv->set(value, key, expiration);
        return BoolToNValue(env, ret);
    }
    return BoolToNValue(env, false);
}

static napi_value decodeStringSet(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3] = {nullptr};
    NAPI_CALL(napi_get_cb_info(env, info, &argc, args, nullptr, nullptr));

    auto handle = NValueToUInt64(env, args[0]);
    auto key = NValueToString(env, args[1]);
    MMKV *kv = reinterpret_cast<MMKV *>(handle);
    if (kv && key.length() > 0) {
        vector<string> result;
        if (kv->getVector(key, result)) {
            return StringArrayToNValue(env, result);
        }
    }
    return args[2];
}

static napi_value encodeBytes(napi_env env, napi_callback_info info) {
    size_t argc = 4;
    napi_value args[4] = {nullptr};
    NAPI_CALL(napi_get_cb_info(env, info, &argc, args, nullptr, nullptr));

    auto handle = NValueToUInt64(env, args[0]);
    auto key = NValueToString(env, args[1]);
    MMKV *kv = reinterpret_cast<MMKV *>(handle);
    if (kv && key.length() > 0) {
        auto value = NValueToMMBuffer(env, args[2]);
        if (IsNValueUndefined(env, args[3])) {
            auto ret = kv->set(value, key);
            return BoolToNValue(env, ret);
        }
        uint32_t expiration = NValueToUInt32(env, args[3]);
        auto ret = kv->set(value, key, expiration);
        return BoolToNValue(env, ret);
    }
    return BoolToNValue(env, false);
}

static napi_value decodeBytes(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3] = {nullptr};
    NAPI_CALL(napi_get_cb_info(env, info, &argc, args, nullptr, nullptr));

    auto handle = NValueToUInt64(env, args[0]);
    auto key = NValueToString(env, args[1]);
    MMKV *kv = reinterpret_cast<MMKV *>(handle);
    if (kv && key.length() > 0) {
        MMBuffer result;
        if (kv->getBytes(key, result)) {
            return MMBufferToNValue(env, std::move(result));
        }
    }
    return args[2];
}

static napi_value containsKey(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = {nullptr};
    NAPI_CALL(napi_get_cb_info(env, info, &argc, args, nullptr, nullptr));

    auto handle = NValueToUInt64(env, args[0]);
    auto key = NValueToString(env, args[1]);
    MMKV *kv = reinterpret_cast<MMKV *>(handle);
    if (kv && key.length() > 0) {
       return BoolToNValue(env, kv->containsKey(key));
    }
    return BoolToNValue(env, false);
}

static napi_value removeValueForKey(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = {nullptr};
    NAPI_CALL(napi_get_cb_info(env, info, &argc, args, nullptr, nullptr));

    auto handle = NValueToUInt64(env, args[0]);
    auto key = NValueToString(env, args[1]);
    MMKV *kv = reinterpret_cast<MMKV *>(handle);
    if (kv && key.length() > 0) {
       kv->removeValueForKey(key);
    }
    return NAPIUndefined(env);
}

static napi_value removeValuesForKeys(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = {nullptr};
    NAPI_CALL(napi_get_cb_info(env, info, &argc, args, nullptr, nullptr));

    auto handle = NValueToUInt64(env, args[0]);
    MMKV *kv = reinterpret_cast<MMKV *>(handle);
    if (!kv) {
       return NAPIUndefined(env);
    }

    vector<string> keys = NValueToStringArray(env, args[1]);
    if (keys.size() > 0) {
       kv->removeValuesForKeys(keys);
    }
    return NAPIUndefined(env);
}

static napi_value count(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = {nullptr};
    NAPI_CALL(napi_get_cb_info(env, info, &argc, args, nullptr, nullptr));

    auto handle = NValueToUInt64(env, args[0]);
    auto filterExpire = NValueToBool(env, args[1]);
    MMKV *kv = reinterpret_cast<MMKV *>(handle);
    if (kv) {
       return UInt64ToNValue(env, kv->count(filterExpire));
    }
    return NAPIUndefined(env);
}

static napi_value allKeys(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = {nullptr};
    NAPI_CALL(napi_get_cb_info(env, info, &argc, args, nullptr, nullptr));

    auto handle = NValueToUInt64(env, args[0]);
    auto filterExpire = NValueToBool(env, args[1]);
    MMKV *kv = reinterpret_cast<MMKV *>(handle);
    if (kv) {
        auto keys = kv->allKeys(filterExpire);
        return StringArrayToNValue(env, keys);
    }
    return NAPIUndefined(env);
}

static napi_value clearAll(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = {nullptr};
    NAPI_CALL(napi_get_cb_info(env, info, &argc, args, nullptr, nullptr));

    auto handle = NValueToUInt64(env, args[0]);
    auto keepSpace = NValueToBool(env, args[1]);
    MMKV *kv = reinterpret_cast<MMKV *>(handle);
    if (kv) {
        kv->clearAll(keepSpace);
    }
    return NAPIUndefined(env);
}

static napi_value sync(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value args[2] = {nullptr};
    NAPI_CALL(napi_get_cb_info(env, info, &argc, args, nullptr, nullptr));

    auto handle = NValueToUInt64(env, args[0]);
    auto needSync = NValueToBool(env, args[1]);
    MMKV *kv = reinterpret_cast<MMKV *>(handle);
    if (kv) {
        kv->sync(needSync ? MMKV_SYNC : MMKV_ASYNC);
    }
    return NAPIUndefined(env);
}

static napi_value clearMemoryCache(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    NAPI_CALL(napi_get_cb_info(env, info, &argc, args, nullptr, nullptr));

    auto handle = NValueToUInt64(env, args[0]);
    MMKV *kv = reinterpret_cast<MMKV *>(handle);
    if (kv) {
        kv->clearMemoryCache();
    }
    return NAPIUndefined(env);
}

static napi_value totalSize(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    NAPI_CALL(napi_get_cb_info(env, info, &argc, args, nullptr, nullptr));

    auto handle = NValueToUInt64(env, args[0]);
    MMKV *kv = reinterpret_cast<MMKV *>(handle);
    if (kv) {
        return UInt64ToNValue(env, kv->totalSize());
    }
    return NAPIUndefined(env);
}

static napi_value actualSize(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    NAPI_CALL(napi_get_cb_info(env, info, &argc, args, nullptr, nullptr));

    auto handle = NValueToUInt64(env, args[0]);
    MMKV *kv = reinterpret_cast<MMKV *>(handle);
    if (kv) {
        return UInt64ToNValue(env, kv->actualSize());
    }
    return NAPIUndefined(env);
}

static napi_value lock(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    NAPI_CALL(napi_get_cb_info(env, info, &argc, args, nullptr, nullptr));

    auto handle = NValueToUInt64(env, args[0]);
    MMKV *kv = reinterpret_cast<MMKV *>(handle);
    if (kv) {
        kv->lock();
    }
    return NAPIUndefined(env);
}

static napi_value unlock(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    NAPI_CALL(napi_get_cb_info(env, info, &argc, args, nullptr, nullptr));

    auto handle = NValueToUInt64(env, args[0]);
    MMKV *kv = reinterpret_cast<MMKV *>(handle);
    if (kv) {
        kv->unlock();
    }
    return NAPIUndefined(env);
}

static napi_value tryLock(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1] = {nullptr};
    NAPI_CALL(napi_get_cb_info(env, info, &argc, args, nullptr, nullptr));

    auto handle = NValueToUInt64(env, args[0]);
    MMKV *kv = reinterpret_cast<MMKV *>(handle);
    if (kv) {
        return BoolToNValue(env, kv->try_lock());
    }
    return NAPIUndefined(env);
}

EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports) {
    napi_property_descriptor desc[] = {
        { "initialize", nullptr, initialize, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "version", nullptr, version, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "pageSize", nullptr, pageSize, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "getDefaultMMKV", nullptr, getDefaultMMKV, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "encodeBool", nullptr, encodeBool, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "decodeBool", nullptr, decodeBool, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "encodeInt32", nullptr, encodeInt32, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "decodeInt32", nullptr, decodeInt32, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "encodeUInt32", nullptr, encodeUInt32, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "decodeUInt32", nullptr, decodeUInt32, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "encodeInt64", nullptr, encodeInt64, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "decodeInt64", nullptr, decodeInt64, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "encodeUInt64", nullptr, encodeUInt64, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "decodeUInt64", nullptr, decodeUInt64, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "encodeDouble", nullptr, encodeDouble, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "decodeDouble", nullptr, decodeDouble, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "encodeString", nullptr, encodeString, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "decodeString", nullptr, decodeString, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "encodeStringSet", nullptr, encodeStringSet, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "decodeStringSet", nullptr, decodeStringSet, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "encodeBytes", nullptr, encodeBytes, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "decodeBytes", nullptr, decodeBytes, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "containsKey", nullptr, containsKey, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "count", nullptr, count, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "allKeys", nullptr, allKeys, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "removeValueForKey", nullptr, removeValueForKey, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "removeValuesForKeys", nullptr, removeValuesForKeys, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "clearAll", nullptr, clearAll, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "clearMemoryCache", nullptr, clearMemoryCache, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "actualSize", nullptr, actualSize, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "totalSize", nullptr, totalSize, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "sync", nullptr, sync, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "lock", nullptr, lock, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "unlock", nullptr, unlock, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "tryLock", nullptr, tryLock, nullptr, nullptr, nullptr, napi_default, nullptr },
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    return exports;
}
EXTERN_C_END

static napi_module mmkvModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "mmkv",
    .nm_priv = ((void*)0),
    .reserved = { 0 },
};

extern "C" __attribute__((constructor)) void RegisterMMKVModule(void) {
    napi_module_register(&mmkvModule);
}
