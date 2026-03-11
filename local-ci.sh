#!/bin/bash
#
# local-ci.sh - Local build script for DreamDexed (macOS native)
#
# Mirrors the GitHub CI workflow for a single RPi target.
# Usage: RPI=3 ./local-ci.sh
#
set -e
set -x

if [ -z "${RPI}" ]; then
  echo "Error: \$RPI not set. Usage: export RPI=3 && ./local-ci.sh"
  exit 1
fi

# Step 1: Submodules (skip if patches are active)
# Uncomment the next line to sync submodules (will undo local circle patches)
# sh -ex ./submod.sh

# Step 2: Build
bash -ex build.sh

# Step 3: Create SD card layout
mkdir -p ./sdcard/

# Copy kernel image
cp ./src/kernel*.img ./sdcard/

# Build boot files
cd ./circle-stdlib/libs/circle/boot
make
make armstub64
cd -

# Copy boot files
cp -r ./circle-stdlib/libs/circle/boot/* sdcard/
rm -rf sdcard/config*.txt sdcard/README sdcard/Makefile sdcard/armstub sdcard/COPYING.linux

# Copy configs
cp ./src/config.txt ./src/minidexed.ini ./src/performance.ini sdcard/
cp ./getsysex.sh sdcard/
echo "usbspeed=full" > sdcard/cmdline.txt

# Performances
if [ -d "./Soundplantage" ]; then
  cp -r ./Soundplantage/performance ./Soundplantage/*.pdf ./sdcard/ 2>/dev/null || true
fi
if [ -d "./performance" ]; then
  cp -r ./performance/* ./sdcard/performance/ 2>/dev/null || true
fi

# Hardware configuration
if [ -d "./hwconfig" ]; then
  cd hwconfig
  sh -ex ./customize.sh 2>/dev/null || true
  cd -
  mkdir -p ./sdcard/hardware/
  cp -r ./hwconfig/minidexed_* ./sdcard/minidexed.ini ./sdcard/hardware/ 2>/dev/null || true
fi

# WLAN firmware (optional, will skip on macOS without wget)
mkdir -p sdcard/firmware
cp circle-stdlib/libs/circle/addon/wlan/sample/hello_wlan/wpa_supplicant.conf sdcard/ 2>/dev/null || true
cd sdcard/firmware
make -f ../../circle-stdlib/libs/circle/addon/wlan/firmware/Makefile 2>/dev/null || echo "WLAN firmware download skipped (wget not available)"
cd -

echo ""
echo "========================================="
echo "Build complete! SD card content in ./sdcard/"
echo "========================================="
ls -la ./sdcard/kernel*.img
