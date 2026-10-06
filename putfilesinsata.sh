#!/usr/bin/env bash
# put_files_sata.sh  – copy arbitrary files into the sata_disk.img FAT32 image

set -euo pipefail

IMG="sata_disk.img"              # change if you renamed the image
MOUNT_DIR="$(mktemp -d)"         # we’ll mount the image here

if [[ $# -lt 1 ]]; then
    echo "Usage:  $0  <file1> [file2 …]"
    exit 1
fi

echo ">>> Using image: $IMG"
[[ -f "$IMG" ]] || { echo "Image not found"; exit 1; }

# ---------------------------------------------------------------------------
# 1. Create a temporary loop device and mount it
# ---------------------------------------------------------------------------
LOOPDEV=$(sudo losetup --find --show "$IMG")   # e.g. /dev/loop5
echo ">>> Loop device: $LOOPDEV"

sudo mount "$LOOPDEV" "$MOUNT_DIR"
echo ">>> Mounted on $MOUNT_DIR"

# ---------------------------------------------------------------------------
# 2. Copy each supplied file into the image
# ---------------------------------------------------------------------------
for f in "$@"; do
    [[ -f "$f" ]] || { echo "File '$f' does not exist, skipping"; continue; }
    echo ">>> Copying $f -> $MOUNT_DIR/"
    sudo cp -v "$f" "$MOUNT_DIR/"
done

# ---------------------------------------------------------------------------
# 3. Clean up
# ---------------------------------------------------------------------------
sudo sync
sudo umount "$MOUNT_DIR"
sudo losetup -d "$LOOPDEV"
rmdir "$MOUNT_DIR"

echo ">>> Done.  Files are now inside $IMG."
