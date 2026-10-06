#!/usr/bin/env bash
# Measure how fast the matrix program actually animates, and grab a desktop
# screenshot while it is running.
cd /mnt/c/osproj || exit 1

rm -f serial-matrixfps.log shot-matrixfps-*.png qemu-matrixfps.log

qemu-system-x86_64 \
    -boot d -cdrom os.iso -smp 4 -m 1G -M q35 \
    -netdev user,id=net0 -device rtl8139,netdev=net0 \
    -drive file=sata_disk.img,format=raw,index=0 \
    -drive file=nvme_disk.img,format=raw,index=1,media=disk \
    -device virtio-vga,xres=1920,yres=1080 \
    -display none -no-reboot \
    -serial file:serial-matrixfps.log \
    -qmp tcp:127.0.0.1:4460,server,nowait \
    > qemu-matrixfps.log 2>&1 &
QPID=$!

sleep 16

python3 /mnt/c/osproj/qmp_scenario.py 4460 /mnt/c/osproj/shot-matrixfps \
    "wait:2" "type:matrix" "wait:3" \
    "key:ctrl,q" "wait:10" "shot:1-matrix" \
    "key:spc" "wait:4"

sleep 1
kill "$QPID" 2>/dev/null
wait "$QPID" 2>/dev/null

echo "=== matrix frame log ==="
grep -nE "matrix: [0-9]+ frames|matrix: stopped" serial-matrixfps.log | tail -12
