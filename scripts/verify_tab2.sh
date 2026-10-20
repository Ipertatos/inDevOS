#!/usr/bin/env bash
# Unique-prefix Tab completion: "neo" + Tab must complete to "neofetch " and
# Enter must then run /neofetch.exe (which prints "uid=").
cd /mnt/c/osproj || exit 1

rm -f serial-tab2.log shot-tab2-*.png qemu-tab2.log

qemu-system-x86_64 \
    -boot d -cdrom os.iso -smp 4 -m 1G -M q35 \
    -netdev user,id=net0 -device rtl8139,netdev=net0 \
    -drive file=sata_disk.img,format=raw,index=0 \
    -drive file=nvme_disk.img,format=raw,index=1,media=disk \
    -device virtio-vga,xres=1920,yres=1080 \
    -display none -no-reboot \
    -serial file:serial-tab2.log \
    -qmp tcp:127.0.0.1:4457,server,nowait \
    > qemu-tab2.log 2>&1 &
QPID=$!

sleep 16

python3 /mnt/c/osproj/qmp_scenario.py 4457 /mnt/c/osproj/shot-tab2 \
    "wait:2" "raw:neo" "key:tab" "wait:1" "shot:1-completed" "key:ret" "wait:8" "shot:2-ran"

sleep 1
kill "$QPID" 2>/dev/null
wait "$QPID" 2>/dev/null

echo "=== unique-prefix completion ==="
grep -nE "zeus> neo|neofetch|uid=|Command not found" serial-tab2.log | tail -12
