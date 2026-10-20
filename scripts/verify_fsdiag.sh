#!/usr/bin/env bash
# Short probe: one failed file create and one failed mkdir, to capture the
# fat32 directory-state diagnostics on the serial log.
OSPROJ="${OSPROJ:-$(cd "$(dirname "$0")/.." && pwd)}"; export OSPROJ  # repo root
cd ${OSPROJ} || exit 1

rm -f serial-fsdiag.log shot-fsdiag-*.png qemu-fsdiag.log

qemu-system-x86_64 \
    -boot d -cdrom os.iso -smp 4 -m 1G -M q35 \
    -netdev user,id=net0 -device rtl8139,netdev=net0 \
    -drive file=sata_disk.img,format=raw,index=0 \
    -drive file=nvme_disk.img,format=raw,index=1,media=disk \
    -device virtio-vga,xres=1920,yres=1080 \
    -display none -no-reboot \
    -serial file:serial-fsdiag.log \
    -qmp tcp:127.0.0.1:4454,server,nowait \
    > qemu-fsdiag.log 2>&1 &
QPID=$!

sleep 16

python3 ${OSPROJ}/qmp_scenario.py 4454 ${OSPROJ}/shot-fsdiag \
    "wait:2" "type:touch /A.TXT" "wait:6" \
    "wait:2" "type:mkdir /B"     "wait:6"

sleep 1
kill "$QPID" 2>/dev/null
wait "$QPID" 2>/dev/null

echo "=== fat32 diagnostics ==="
grep -nE "fat32\[|Directory is full|table full|Cannot resolve" serial-fsdiag.log | tail -40
