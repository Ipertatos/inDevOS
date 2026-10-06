#!/usr/bin/env bash
# Compare the time the read path takes for the same 4 MB file through different
# programs, to tell an I/O cost apart from a program's own CPU cost.
cd /mnt/c/osproj || exit 1

rm -f serial-speed.log shot-speed-*.png qemu-speed.log

qemu-system-x86_64 \
    -boot d -cdrom os.iso -smp 4 -m 1G -M q35 \
    -netdev user,id=net0 -device rtl8139,netdev=net0 \
    -drive file=sata_disk.img,format=raw,index=0 \
    -drive file=nvme_disk.img,format=raw,index=1,media=disk \
    -device virtio-vga,xres=1920,yres=1080 \
    -display none -no-reboot \
    -serial file:serial-speed.log \
    -qmp tcp:127.0.0.1:4463,server,nowait \
    > qemu-speed.log 2>&1 &
QPID=$!

sleep 16

python3 /mnt/c/osproj/qmp_scenario.py 4463 /mnt/c/osproj/shot-speed \
    "wait:2" "type:wc /doom1.wad"          "wait:25" \
    "wait:2" "type:grep WAD /doom1.wad"    "wait:25" \
    "wait:2" "type:head -1 /doom1.wad"     "wait:12"

sleep 1
kill "$QPID" 2>/dev/null
wait "$QPID" 2>/dev/null

echo "=== timings: exec enter -> return ==="
grep -nE "launching '/(wc|grep|head)\.exe|enter_usermode|returned from user mode|Total|^\[.*\] *[0-9]+ *[0-9]+ *[0-9]+" serial-speed.log | tail -30
