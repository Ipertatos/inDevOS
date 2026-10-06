#!/usr/bin/env bash
# Verify Tab completion now that zeus.c owns the command vocabulary:
#   "ne" + Tab  -> unique match, completes to "neofetch " (then Enter runs it)
#   "cl" + Tab  -> ambiguous, prints the matching names
cd /mnt/c/osproj || exit 1

rm -f serial-tab.log shot-tab-*.png qemu-tab.log

qemu-system-x86_64 \
    -boot d -cdrom os.iso -smp 4 -m 1G -M q35 \
    -netdev user,id=net0 -device rtl8139,netdev=net0 \
    -drive file=sata_disk.img,format=raw,index=0 \
    -drive file=nvme_disk.img,format=raw,index=1,media=disk \
    -device virtio-vga,xres=1920,yres=1080 \
    -display none -no-reboot \
    -serial file:serial-tab.log \
    -qmp tcp:127.0.0.1:4456,server,nowait \
    > qemu-tab.log 2>&1 &
QPID=$!

sleep 16

python3 /mnt/c/osproj/qmp_scenario.py 4456 /mnt/c/osproj/shot-tab \
    "wait:2" "raw:ne" "key:tab" "wait:1" "shot:1-completed" "key:ret" "wait:6" \
    "wait:1" "raw:cl" "key:tab" "wait:2" "shot:2-ambiguous" \
    "wait:1" "key:esc" "key:ret" "wait:3"

sleep 1
kill "$QPID" 2>/dev/null
wait "$QPID" 2>/dev/null

echo "=== completion evidence ==="
grep -nE "zeus> (ne|cl|neofetch)|  (clear|clip|clipboard)|uid=|Page fault|panic" serial-tab.log | tail -25
