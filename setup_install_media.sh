#!/usr/bin/env bash
# setup_install_media.sh - Prepare sata_disk.img with install files for in-OS installer
#
# Creates /install/ on sata_disk.img containing:
#   boot.bin (kernel), limine.cfg, limine-bios.sys
#
# Run this before booting, then use 'install' or 'install 1' inside inDevOS.
# Usage: ./setup_install_media.sh

set -euo pipefail

IMG="sata_disk.img"
MOUNT_DIR="$(mktemp -d)"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

cd "$SCRIPT_DIR"

echo ">>> inDevOS Install Media Setup"
echo ""

if [[ ! -f "$IMG" ]]; then
    echo ">>> Creating disk image (run create_disk_images.sh or using default)..."
    if [[ -f create_disk_images.sh ]]; then
        ./create_disk_images.sh
    else
        dd if=/dev/zero of="$IMG" bs=100M count=1 status=progress
        LOOPDEV=$(losetup --show -f "$IMG")
        mkfs.fat -F 32 -n "SATA_RAW" "$LOOPDEV"
        losetup -d "$LOOPDEV"
    fi
fi

if [[ ! -f kernel_final.bin ]]; then
    echo ">>> Building kernel..."
    make kernel_final.bin
fi

if [[ ! -f limine.cfg ]]; then
    echo "Error: limine.cfg not found"
    exit 1
fi

if [[ ! -f limine/limine-bios.sys ]]; then
    echo "Error: limine/limine-bios.sys not found (run 'make limine' or ensure limine is set up)"
    exit 1
fi

echo ">>> Mounting $IMG on $MOUNT_DIR"
LOOPDEV=$(sudo losetup --find --show "$IMG")
sudo mount "$LOOPDEV" "$MOUNT_DIR"

echo ">>> Creating /install/ and copying files"
sudo mkdir -p "$MOUNT_DIR/install"
sudo cp -v kernel_final.bin "$MOUNT_DIR/install/boot.bin"
sudo cp -v limine.cfg "$MOUNT_DIR/install/limine.cfg"
sudo cp -v limine/limine-bios.sys "$MOUNT_DIR/install/limine-bios.sys"

echo ">>> Syncing and unmounting"
sudo sync
sudo umount "$MOUNT_DIR"
sudo losetup -d "$LOOPDEV"
rmdir "$MOUNT_DIR"

echo ""
echo ">>> Done! Install media is ready."
echo "    Boot inDevOS from CD, run 'mount 0', then 'install' or 'install 1'"
