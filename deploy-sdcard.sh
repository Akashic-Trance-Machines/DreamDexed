#!/bin/bash
#
# deploy-sdcard.sh - Deploy built SD card content to mounted volume
#
# Usage: yes | ./deploy-sdcard.sh
#
set -e

SDCARD_MOUNT="/Volumes/DREAMDEXED"
SDCARD_DIR="./sdcard"

if [ ! -d "${SDCARD_DIR}" ]; then
  echo "Error: ${SDCARD_DIR} directory not found. Run ./local-ci.sh first."
  exit 1
fi

if [ ! -d "${SDCARD_MOUNT}" ]; then
  echo "Error: SD card not mounted at ${SDCARD_MOUNT}"
  exit 1
fi

echo "WARNING: This will erase all contents of ${SDCARD_MOUNT}"
echo "Continue? (y/N)"
read -r response
if [ "$response" != "y" ] && [ "$response" != "Y" ]; then
  echo "Aborted."
  exit 0
fi

echo "Clearing ${SDCARD_MOUNT}..."
rm -rf "${SDCARD_MOUNT:?}"/*

echo "Copying ${SDCARD_DIR}/ to ${SDCARD_MOUNT}/..."
cp -r "${SDCARD_DIR}"/* "${SDCARD_MOUNT}"/

# Sync filesystem
sync

echo ""
echo "========================================="
echo "Deployment complete!"
echo "========================================="
ls -la "${SDCARD_MOUNT}"/kernel*.img
