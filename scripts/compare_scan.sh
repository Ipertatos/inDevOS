#!/usr/bin/env bash
# Wall-clock A/B: how long does scanning the 4 MB WAD take in this environment?
# The harness prints an elapsed-time stamp for every typed command, so the
# difference between two stamps is real time (not guest HPET time).
#
#   compare_scan.sh [mode]     mode = wsl (default)
cd /mnt/c/osproj || exit 1

rm -f serial-scan.log qemu-scan.log

qemu-system-x86_64 \
    -boot d -cdrom os.iso -smp 4 -m 1G -M q35 \
    -netdev user,id=net0 -device rtl8139,netdev=net0 \
    -drive file=sata_disk.img,format=raw,index=0 \
    -drive file=nvme_disk.img,format=raw,index=1,media=disk \
    -device virtio-vga,xres=1920,yres=1080 \
    -display none -no-reboot \
    -serial file:serial-scan.log \
    -qmp tcp:127.0.0.1:4466,server,nowait \
    > qemu-scan.log 2>&1 &
QPID=$!

sleep 16

python3 /mnt/c/osproj/qmp_scenario.py 4466 /mnt/c/osproj/shot-scan \
    "wait:2" "type:wc /doom1.wad"       "wait:12" \
    "wait:2" "type:grep WAD /doom1.wad" "wait:12" \
    "wait:2" "type:wc /SCORES.TXT"      "wait:6"

sleep 1
kill "$QPID" 2>/dev/null
wait "$QPID" 2>/dev/null

echo "=== results (guest view) ==="
grep -aE "4196020|returned from user mode" serial-scan.log | tail -8
