#!/usr/bin/env bash
# Profile the read path: how much time does a large file read cost per syscall,
# and in which syscall does it go?
cd /mnt/c/osproj || exit 1

rm -f serial-readprof.log shot-readprof-*.png qemu-readprof.log

qemu-system-x86_64 \
    -boot d -cdrom os.iso -smp 4 -m 1G -M q35 \
    -netdev user,id=net0 -device rtl8139,netdev=net0 \
    -drive file=sata_disk.img,format=raw,index=0 \
    -drive file=nvme_disk.img,format=raw,index=1,media=disk \
    -device virtio-vga,xres=1920,yres=1080 \
    -display none -no-reboot \
    -serial file:serial-readprof.log \
    -qmp tcp:127.0.0.1:4464,server,nowait \
    > qemu-readprof.log 2>&1 &
QPID=$!

sleep 16

python3 /mnt/c/osproj/qmp_scenario.py 4464 /mnt/c/osproj/shot-readprof \
    "wait:2" "type:prof reset"     "wait:3" \
    "wait:2" "type:prof start 4"   "wait:3" \
    "wait:2" "type:wc /doom1.wad"  "wait:70" \
    "wait:2" "type:prof stop"      "wait:3" \
    "wait:2" "type:prof syscalls"  "wait:5"

sleep 1
kill "$QPID" 2>/dev/null
wait "$QPID" 2>/dev/null

echo "=== read syscall profile ==="
grep -aE "PROFILER SYSCALLS|syscall count|read|print|total_us|profiler:" serial-readprof.log | tail -20
echo "=== wc timing ==="
grep -aE "enter_usermode|returned from user mode|/doom1.wad" serial-readprof.log | tail -8
