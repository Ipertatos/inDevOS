#!/usr/bin/env bash
# Build the DOOM user app and install it into sata_disk.img (FAT32 superfloppy).
#
#   build_doom.sh            build + install
#   build_doom.sh --restore  put the backed-up original binary back
cd /mnt/c/osproj || exit 1
LOG=/mnt/c/osproj/build-doom.log
ORIG=/mnt/c/osproj/.doom-orig.exe

if [ "$1" = "--restore" ]; then
    [ -f "$ORIG" ] || { echo "no backup at $ORIG"; exit 1; }
    mcopy -o -i sata_disk.img "$ORIG" ::/doom.exe && echo "restored original doom.exe"
    exit 0
fi

# Keep the binary that is currently in the repository as a backup.
[ -f "$ORIG" ] || cp -f doom/doom.exe "$ORIG"

: > "$LOG"
{
    echo "=== building DOOM $(date) ==="
    make -C doom clean
    make -C doom CC=x86_64-linux-gnu-gcc LD=x86_64-linux-gnu-ld
    echo "=== make exit=$? ==="
    ls -l doom/doom.exe
} >> "$LOG" 2>&1

if [ ! -f doom/doom.exe ]; then
    echo "BUILD FAILED - see build-doom.log"
    tail -30 "$LOG"
    exit 1
fi

echo "--- installing into sata_disk.img ---" >> "$LOG"
mmd -i sata_disk.img ::/bin 2>/dev/null || true
mcopy -o -i sata_disk.img doom/doom.exe ::/doom.exe >> "$LOG" 2>&1
mcopy -o -i sata_disk.img doom/doom.exe ::/bin/doom.exe >> "$LOG" 2>&1
if [ -f doom1.wad ]; then
    mcopy -o -i sata_disk.img doom1.wad ::/doom1.wad >> "$LOG" 2>&1
    echo "installed /doom1.wad" >> "$LOG"
fi
echo "mcopy exit=$?" >> "$LOG"
mdir -i sata_disk.img ::/doom.exe >> "$LOG" 2>&1

tail -6 "$LOG"
