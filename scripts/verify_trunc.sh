#!/usr/bin/env bash
# 1. O_TRUNC: copy a small file over a large one and confirm with mtools
#    (an independent FAT32 implementation) that the file really shrank.
# 2. matrix: the ported program's window must be visible on the desktop and it
#    must exit when a key is pressed.
OSPROJ="${OSPROJ:-$(cd "$(dirname "$0")/.." && pwd)}"; export OSPROJ  # repo root
cd ${OSPROJ} || exit 1

rm -f serial-trunc.log shot-trunc-*.png qemu-trunc.log
mdel -i sata_disk.img ::/BIG.TXT 2>/dev/null   # clean any earlier run

qemu-system-x86_64 \
    -boot d -cdrom os.iso -smp 4 -m 1G -M q35 \
    -netdev user,id=net0 -device rtl8139,netdev=net0 \
    -drive file=sata_disk.img,format=raw,index=0 \
    -drive file=nvme_disk.img,format=raw,index=1,media=disk \
    -device virtio-vga,xres=1920,yres=1080 \
    -display none -no-reboot \
    -serial file:serial-trunc.log \
    -qmp tcp:127.0.0.1:4459,server,nowait \
    > qemu-trunc.log 2>&1 &
QPID=$!

sleep 16

python3 ${OSPROJ}/qmp_scenario.py 4459 ${OSPROJ}/shot-trunc \
    "wait:2" "type:cp /test.png /BIG.TXT"    "wait:12" \
    "wait:2" "type:cp /SCORES.TXT /BIG.TXT"  "wait:8"  "shot:1-truncated" \
    "wait:2" "type:wc /BIG.TXT"              "wait:5" \
    "wait:2" "type:matrix"                   "wait:8" \
    "key:ctrl,q" "wait:3" "shot:2-matrix-desktop" \
    "key:spc"    "wait:4" "shot:3-after-exit"

sleep 1
kill "$QPID" 2>/dev/null
wait "$QPID" 2>/dev/null

echo "=== truncate + matrix evidence ==="
grep -nE "BIG.TXT|cp:|matrix:|matrix\.exe|Page fault|panic|exited \(status" serial-trunc.log | tail -25

echo "=== independent check: BIG.TXT size as seen by mtools ==="
mdir -i sata_disk.img ::/BIG.TXT 2>&1 | grep -iE "BIG|bytes" | tail -3

# leave the disk image as we found it
mdel -i sata_disk.img ::/BIG.TXT 2>/dev/null && echo "cleaned up BIG.TXT"
