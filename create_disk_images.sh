#!/bin/bash

# Simplified: create raw FAT-32 images (no partition table)
# Useful when the OS mounts the block device itself rather than partitions.

set -e

NVME_SIZE="256M"
SATA_SIZE="100M"
NVME_IMG="nvme_disk.img"
SATA_IMG="sata_disk.img"

create_raw_fat32() {
    local img=$1
    local size=$2
    local label=$3

    echo "Creating $img ($size, raw FAT32)..."
    dd if=/dev/zero of="$img" bs=$size count=1 status=progress

    echo "Formatting $img as FAT32 ..."
    # Associate a loop-device then mkfs on the whole disk (no partitions)
    LOOPDEV=$(losetup --show -f "$img")
    mkfs.fat -F 32 -n "$label" "$LOOPDEV"
    losetup -d "$LOOPDEV"
}

create_raw_fat32 "$NVME_IMG" "$NVME_SIZE" "NVME_RAW"
create_raw_fat32 "$SATA_IMG" "$SATA_SIZE" "SATA_RAW"

echo "Completed. Images are raw FAT32 with labels NVME_RAW and SATA_RAW." 