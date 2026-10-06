#!/usr/bin/env bash
# Separate exec cost from console-output cost:
#   * mkdir /X9   - prints nothing (pure exec)
#   * ls /        - 54 lines: the first run has to scroll the screen, the second
#                   is measured again after `clear` when the screen is empty.
cd /mnt/c/osproj || exit 1

rm -f serial-t2.log shot-t2-*.png qemu-t2.log

qemu-system-x86_64 \
    -boot d -cdrom os.iso -smp 4 -m 1G -M q35 \
    -netdev user,id=net0 -device rtl8139,netdev=net0 \
    -drive file=sata_disk.img,format=raw,index=0 \
    -drive file=nvme_disk.img,format=raw,index=1,media=disk \
    -device virtio-vga,xres=1920,yres=1080 \
    -display none -no-reboot \
    -serial file:serial-t2.log \
    -qmp tcp:127.0.0.1:4468,server,nowait \
    > qemu-t2.log 2>&1 &
QPID=$!

sleep 16

python3 /mnt/c/osproj/qmp_scenario.py 4468 /mnt/c/osproj/shot-t2 \
    "wait:2" "type:time mkdir /X9"    "wait:8" \
    "wait:2" "type:clear"             "wait:3" \
    "wait:2" "type:time ls /"         "wait:12" "shot:1-cleared" \
    "wait:2" "type:clear"             "wait:3" \
    "wait:2" "type:time echo one"     "wait:8" \
    "wait:2" "type:rm /X9"            "wait:8"

sleep 1
kill "$QPID" 2>/dev/null
wait "$QPID" 2>/dev/null

echo "=== timings ==="
grep -aE "time: (mkdir|ls|echo|rm) took|program file read" serial-t2.log | tail -12
