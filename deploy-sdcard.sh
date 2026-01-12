#!/bin/bash
set -e

# Deploy script for DreamDexed SD card
# Erases and copies all files from ./sdcard to mounted volume DREAMDEXED

SD_VOLUME="/Volumes/DREAMDEXED"
SDCARD_DIR="./sdcard"

echo "=== DreamDexed SD Card Deployment ==="

# Check if SD card is mounted
if [ ! -d "$SD_VOLUME" ]; then
    echo "ERROR: SD card 'DREAMDEXED' not found at $SD_VOLUME"
    echo "Please insert and mount the SD card first."
    exit 1
fi

# Check if sdcard directory exists
if [ ! -d "$SDCARD_DIR" ]; then
    echo "ERROR: Source directory '$SDCARD_DIR' not found."
    echo "Please run the build first (local-ci.sh)."
    exit 1
fi

echo "Found SD card at: $SD_VOLUME"
echo "Source directory: $SDCARD_DIR"
echo ""

# Show what will be deleted
echo "Files currently on SD card:"
ls -la "$SD_VOLUME" | head -20
echo ""

read -p "This will ERASE all files on DREAMDEXED and copy new files. Continue? (y/N) " -n 1 -r
echo ""

if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    echo "Aborted."
    exit 0
fi

echo ""
echo "Erasing SD card..."
rm -rf "${SD_VOLUME:?}"/*

echo "Copying files..."
cp -rv "$SDCARD_DIR"/* "$SD_VOLUME/"

echo ""
echo "Syncing..."
sync

echo ""
echo "=== Deployment Complete ==="
echo "Files on SD card:"
ls -la "$SD_VOLUME"
echo ""
echo "You can now safely eject the SD card."
