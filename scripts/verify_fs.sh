#!/usr/bin/env bash
# Isolate which filesystem write operations actually work:
#   file create (open O_CREAT + write), unlink, mkdir, rmdir.
# Reads (cat/wc/head) are already known-good, so every failure here is a
# write-path bug.
OSPROJ="${OSPROJ:-$(cd "$(dirname "$0")/.." && pwd)}"; export OSPROJ  # repo root
cd ${OSPROJ} || exit 1

rm -f serial-fs.log shot-fs-*.png qemu-fs.log

qemu-system-x86_64 \
    -boot d -cdrom os.iso -smp 4 -m 1G -M q35 \
    -netdev user,id=net0 -device rtl8139,netdev=net0 \
    -drive file=sata_disk.img,format=raw,index=0 \
    -drive file=nvme_disk.img,format=raw,index=1,media=disk \
    -device virtio-vga,xres=1920,yres=1080 \
    -display none -no-reboot \
    -serial file:serial-fs.log \
    -qmp tcp:127.0.0.1:4453,server,nowait \
    > qemu-fs.log 2>&1 &
QPID=$!

sleep 16

python3 ${OSPROJ}/qmp_scenario.py 4453 ${OSPROJ}/shot-fs \
    "wait:2" "type:ls /"                  "wait:5" "shot:1-before" \
    "wait:2" "type:touch /NEWFILE.TXT"    "wait:5" \
    "wait:2" "type:wc /NEWFILE.TXT"       "wait:5" \
    "wait:2" "type:mkdir /NEWDIR"         "wait:5" \
    "wait:2" "type:ls /"                  "wait:5" "shot:2-after-create" \
    "wait:2" "type:touch /NEWDIR/F.TXT"   "wait:5" \
    "wait:2" "type:ls /NEWDIR"            "wait:5" \
    "wait:2" "type:rm /NEWFILE.TXT"       "wait:5" \
    "wait:2" "type:rm /NEWDIR/F.TXT"      "wait:5" \
    "wait:2" "type:rm /NEWDIR"            "wait:5" \
    "wait:2" "type:ls /"                  "wait:5" "shot:3-after-rm"

sleep 1
kill "$QPID" 2>/dev/null
wait "$QPID" 2>/dev/null

echo "=== shell transcript (program output lines) ==="
grep -nE "fat32|vfs:|cannot|Command not found|Page fault|no such|denied" serial-fs.log | tail -40
