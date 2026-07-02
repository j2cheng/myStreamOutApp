#!/bin/bash

cd /home/builduser/collab_stream_in/streamOutApp

# Step 1: Sign the APK
echo "==> Signing APK with platform key..."
./sign_with_platform_key.sh ./X80/ app/build/outputs/apk/debug/app-debug.apk
 
# Step 2: Connect to the device using the first argument ($1)
echo "==> Connecting to device at $1..."
adb connect "$1"

adb root
adb remount


# Step 3: Install the test APK
echo "==> Pushing signed APK to vendor/priv-app..."
adb push app/build/outputs/apk/debug/signed-app-debug.apk /vendor/priv-app/cres-streamout/cres-streamout.apk

echo "==> Process complete!"

