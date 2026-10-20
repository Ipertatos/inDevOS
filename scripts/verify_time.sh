#!/usr/bin/env bash
# Exercise the new `time` built-in and `uptime`, and use them to locate where
# program start-up time actually goes.
OSPROJ="${OSPROJ:-$(cd "$(dirname "$0")/.." && pwd)}"; export OSPROJ  # repo root
cd ${OSPROJ} || exit 1

rm -f serial-time.log shot-time-*.png qemu-time.log

qemu-system-x86_64 \
    -boot d -cdrom os.iso -smp 4 -m 1G -M q35 \
    -netdev user,id=net0 -device rtl8139,netdev=net0 \
    -drive file=sata_disk.img,format=raw,index=0 \
    -drive file=nvme_disk.img,format=raw,index=1,media=disk \
    -device virtio-vga,xres=1920,yres=1080 \
    -display none -no-reboot \
    -serial file:serial-time.log \
    -qmp tcp:127.0.0.1:4467,server,nowait \
    > qemu-time.log 2>&1 &
QPID=$!

sleep 16

python3 ${OSPROJ}/qmp_scenario.py 4467 ${OSPROJ}/shot-time \
    "wait:2" "type:time"                    "wait:5"  "shot:1-usage" \
    "wait:2" "type:uptime"                  "wait:5" \
    "wait:2" "type:time echo hi"            "wait:6" \
    "wait:2" "type:time ls /"               "wait:8" \
    "wait:2" "type:time ls /"               "wait:8"  "shot:2-time" \
    "wait:2" "type:time wc /doom1.wad"      "wait:15" \
    "wait:2" "type:time date"               "wait:6" \
    "wait:2" "type:time help"               "wait:8"

sleep 1
kill "$QPID" 2>/dev/null
wait "$QPID" 2>/dev/null

echo "=== time / uptime output ==="
grep -aE "time:|^\[.*\] up |usage: time|program file read" serial-time.log | tail -25
