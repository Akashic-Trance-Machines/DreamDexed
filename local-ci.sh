#!/bin/bash
set -e

# Default RPI version to 4 if not set
RPI=${RPI:-4}

echo "Starting Local Build for Raspberry Pi $RPI..."

# 1. Update Submodules
echo "Updating submodules..."
./submod.sh

# 2. Prepare SD Card Directory
echo "Preparing sdcard directory..."
mkdir -p ./sdcard/

# 3. Build Kernel
echo "Building main project..."

# 3. Build Kernel
echo "Building main project..."

# Export RPI variable for build.sh
export RPI=$RPI
bash -ex build.sh

# Copy kernel image
echo "Copying kernel image..."
cp ./src/kernel*.img ./sdcard/

# 4. Build Boot Files (Circle)
echo "Building boot files..."
cd ./circle-stdlib/libs/circle/boot
make
# Build 64-bit stub if RPI >= 3
if [ "$RPI" -ge 3 ]; then
    make armstub64
fi
cd -

# Copy boot files
echo "Copying boot files to sdcard..."
cp -r ./circle-stdlib/libs/circle/boot/* sdcard
# Clean up unnecessary Circle boot files
rm -rf sdcard/config*.txt sdcard/README sdcard/Makefile sdcard/armstub sdcard/COPYING.linux

# 5. Configuration Files
echo "Copying configuration files..."
cp ./src/config.txt ./src/minidexed.ini ./src/performance.ini sdcard/
if [ -f "./getsysex.sh" ]; then
    cp ./getsysex.sh sdcard/
fi
echo "usbspeed=full" > sdcard/cmdline.txt

# 6. Performances (Optional - check if directories exist/git clone needed?)
# For local dev, maybe don't re-clone Soundplantage every time if it exists?
# GitHub workflow unconditionally clones. Let's replicate but be gentle.
if [ ! -d "Soundplantage" ]; then
    echo "Cloning Soundplantage..."
    git clone https://github.com/Banana71/Soundplantage --depth 1
fi

echo "Copying performances..."
if [ -d "Soundplantage/performance" ]; then
    mkdir -p ./sdcard/performance/
    cp -r ./Soundplantage/performance/* ./sdcard/performance/
fi
if [ -d "performance/004_Mirage" ]; then
    cp -r ./performance/004_Mirage ./sdcard/performance/
fi
# Copy pdfs if they exist
cp ./Soundplantage/*.pdf ./sdcard/ 2>/dev/null || true
cp ./performance/*.pdf ./sdcard/ 2>/dev/null || true

# 7. Hardware Configuration
echo "Generating hardware configs..."
cd hwconfig
sh -ex ./customize.sh
cd -
mkdir -p ./sdcard/hardware/
cp -r ./hwconfig/minidexed_* ./sdcard/minidexed.ini ./sdcard/hardware/

# 8. WLAN Firmware
echo "Building WLAN firmware..."
mkdir -p sdcard/firmware
cp circle-stdlib/libs/circle/addon/wlan/sample/hello_wlan/wpa_supplicant.conf sdcard/
cd sdcard/firmware
make -f ../../circle-stdlib/libs/circle/addon/wlan/firmware/Makefile
cd -

echo "Build Success! Output is in ./sdcard/"
