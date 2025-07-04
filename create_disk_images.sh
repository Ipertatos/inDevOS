#!/bin/bash

# Script to create and format disk images as FAT32

set -e  # Exit on any error

# Configuration
NVME_SIZE="256M"
SATA_SIZE="100M"
NVME_IMG="nvme_disk.img"
SATA_IMG="sata_disk.img"

echo "Creating disk images..."

# Create NVME disk image
echo "Creating ${NVME_IMG} (${NVME_SIZE})..."
dd if=/dev/zero of="${NVME_IMG}" bs=1M count=256 status=progress

# Create SATA disk image  
echo "Creating ${SATA_IMG} (${SATA_SIZE})..."
dd if=/dev/zero of="${SATA_IMG}" bs=1M count=100 status=progress

echo "Formatting disk images as FAT32..."

# Format NVME disk as FAT32
echo "Formatting ${NVME_IMG} as FAT32..."
mkfs.fat -F 32 -n "NVME_DISK" "${NVME_IMG}"

# Format SATA disk as FAT32
echo "Formatting ${SATA_IMG} as FAT32..."
mkfs.fat -F 32 -n "SATA_DISK" "${SATA_IMG}"

echo "Done! Both disk images created and formatted as FAT32:"
echo "  - ${NVME_IMG}: ${NVME_SIZE} (FAT32, label: NVME_DISK)"
echo "  - ${SATA_IMG}: ${SATA_SIZE} (FAT32, label: SATA_DISK)"

# Verify the filesystems
echo ""
echo "Filesystem information:"
file "${NVME_IMG}" "${SATA_IMG}" 