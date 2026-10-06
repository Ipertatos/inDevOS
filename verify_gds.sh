#!/usr/bin/env bash
# Verify the last three ported programs (grep / date / stopwatch) plus the new
# SYS_RTC_TIME syscall, and confirm the fork-free shell still works.
cd /mnt/c/osproj || exit 1

rm -f serial-gds.log shot-gds-*.png qemu-gds.log

qemu-system-x86_64 \
    -boot d -cdrom os.iso -smp 4 -m 1G -M q35 \
    -netdev user,id=net0 -device rtl8139,netdev=net0 \
    -drive file=sata_disk.img,format=raw,index=0 \
    -drive file=nvme_disk.img,format=raw,index=1,media=disk \
    -device virtio-vga,xres=1920,yres=1080 \
    -display none -no-reboot \
    -serial file:serial-gds.log \
    -qmp tcp:127.0.0.1:4462,server,nowait \
    > qemu-gds.log 2>&1 &
QPID=$!

sleep 16

python3 /mnt/c/osproj/qmp_scenario.py 4462 /mnt/c/osproj/shot-gds \
    "wait:2" "type:date"                             "wait:6"  "shot:1-date" \
    "wait:2" "type:grep 10 /SCORES.TXT"              "wait:6" \
    "wait:2" "type:grep zzz /SCORES.TXT"             "wait:6" \
    "wait:2" "type:grep doom /doom1.wad"             "wait:8" \
    "wait:2" "type:stopwatch" "wait:4" "key:spc"     "wait:3" "shot:2-stopwatch" \
    "wait:2" "type:help"                             "wait:6" "shot:3-help"

sleep 1
kill "$QPID" 2>/dev/null
wait "$QPID" 2>/dev/null

echo "=== program output ==="
grep -nE "grep:|stopwatch|^\[.*\] [0-9]{4}-[0-9]{2}-[0-9]{2}|program\(s\) on disk|Command not found|Page fault|panic" serial-gds.log | tail -25
