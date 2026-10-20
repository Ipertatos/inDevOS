#!/usr/bin/env bash
# setup_apps_directory.sh - Set up /os/apps/ directory structure in sata.img

set -euo pipefail

IMG="sata_disk.img"
MOUNT_DIR="$(mktemp -d)"
APPS_DIR="${APPS_DIR:-$(cd "$(dirname "$0")" && pwd)/apps}"

if [[ ! -f "$IMG" ]]; then
    echo "Error: $IMG not found. Run create_disk_images.sh first."
    exit 1
fi

echo ">>> Setting up /os/apps directory structure..."

# Mount the image
LOOPDEV=$(sudo losetup --find --show "$IMG")
sudo mount "$LOOPDEV" "$MOUNT_DIR"

# Create the apps directory structure
sudo mkdir -p "$MOUNT_DIR/os/apps"

# Copy command executables if they exist
if [[ -d "$APPS_DIR" ]]; then
    echo ">>> Copying command executables..."

    # Find all .exe files in apps directory
    find "$APPS_DIR" -name "*.exe" -type f | while read -r exe_file; do
        base_name=$(basename "$exe_file" .exe)
        echo ">>> Copying $base_name -> $MOUNT_DIR/os/apps/"
        sudo cp "$exe_file" "$MOUNT_DIR/os/apps/$base_name"
        sudo chmod +x "$MOUNT_DIR/os/apps/$base_name"
    done
fi

# Create a README file in the apps directory
sudo tee "$MOUNT_DIR/os/apps/README.txt" > /dev/null << 'EOF'
inDevOS Applications Directory
=============================

This directory contains external command executables that can be run from Zeus Shell.

Available commands:
- ls      - List directory contents
- cat     - Display file contents
- echo    - Display text
- clear   - Clear the screen
- help    - Show available commands
- pwd     - Print working directory
- touch   - Create empty file
- mkdir   - Create directory

To run a command from Zeus Shell:
zeus> ls
zeus> cat filename.txt
zeus> echo Hello World

The Zeus Shell will automatically look for executables in this directory.
EOF

echo ">>> Apps directory setup complete"

# Clean up
sudo sync
sudo umount "$MOUNT_DIR"
sudo losetup -d "$LOOPDEV"
rmdir "$MOUNT_DIR"

echo ">>> Done. /os/apps directory is now set up in $IMG"

