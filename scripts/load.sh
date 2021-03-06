#!/bin/bash
DIR=$(echo $PWD | rev | cut -d '/' -f1 | rev)
BUILD="../build"
PROJECT="../AppleIntelWifiAdapter.xcodeproj"

if [ "$DIR" != "scripts" ]; then
BUILD="./build"
PROJECT="./AppleIntelWifiAdapter.xcodeproj"
fi

if [ ! -d "$BUILD" ]; then
echo "Build the kext with ./scripts/build.sh before running this script."
exit 123
fi

KEXT="$BUILD/Build/Products/Debug/AppleIntelWifiAdapterV2.kext"
(./unload.sh | true)
sudo chown -R $USER $KEXT
read # Wait for user input here
sudo chown -R root:wheel $KEXT && sudo kextload -v 6 $KEXT
