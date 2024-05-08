#!/bin/bash

rm -rf archives
xcodebuild archive \
-workspace MMKV.xcworkspace \
-scheme 'MMKV Static' \
-configuration Release \
-destination "generic/platform=iOS" \
-archivePath "archives/MMKV-iOS" \
SKIP_INSTALL=NO \
BUILD_LIBRARY_FOR_DISTRIBUTION=YES

xcodebuild archive \
-workspace MMKV.xcworkspace \
-scheme 'MMKV Static' \
-configuration Release \
-destination "generic/platform=iOS Simulator" \
-archivePath "archives/MMKV-iOS-Simulator" \
SKIP_INSTALL=NO \
BUILD_LIBRARY_FOR_DISTRIBUTION=YES

xcodebuild archive \
-workspace MMKV.xcworkspace \
-scheme 'MMKV Static' \
-configuration Release \
-destination "generic/platform=macOS" \
-archivePath "archives/MMKV-macOS" \
SKIP_INSTALL=NO \
BUILD_LIBRARY_FOR_DISTRIBUTION=YES

xcodebuild \
-create-xcframework \
-archive archives/MMKV-iOS.xcarchive -library libMMKV.a \
-archive archives/MMKV-macOS.xcarchive -library libMMKV.a \
-archive archives/MMKV-iOS-Simulator.xcarchive -library libMMKV.a \
-output archives/MMKV-Static.xcframework

zip -r -X "archives/MMKV-Static.xcframework.zip" "archives/MMKV-Static.xcframework"
