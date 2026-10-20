#!/usr/bin/env bash
# Profile one `ls /` run to see where its time actually goes (per-symbol).
OSPROJ="${OSPROJ:-$(cd "$(dirname "$0")/.." && pwd)}"; export OSPROJ  # repo root
cd ${OSPROJ} || exit 1

rm -f serial-lsprof.log shot-lsprof-*.png qemu-lsprof.log

qemu-system-x86_64 \
    -boot d -cdrom os.iso -smp 4 -m 1G -M q35 \
    -netdev user,id=net0 -device rtl8139,netdev=net0 \
    -drive file=sata_disk.img,format=raw,index=0 \
    -drive file=nvme_disk.img,format=raw,index=1,media=disk \
    -device virtio-vga,xres=1920,yres=1080 \
    -display none -no-reboot \
    -serial file:serial-lsprof.log \
    -qmp tcp:127.0.0.1:4470,server,nowait \
    > qemu-lsprof.log 2>&1 &
QPID=$!

sleep 16

python3 ${OSPROJ}/qmp_scenario.py 4470 ${OSPROJ}/shot-lsprof \
    "wait:2" "type:prof reset"      "wait:3" \
    "wait:2" "type:prof start 1"    "wait:3" \
    "wait:2" "type:ls /"            "wait:10" \
    "wait:2" "type:prof stop"       "wait:3" \
    "wait:2" "type:prof stats"      "wait:4" \
    "wait:2" "type:prof flame"      "wait:5" \
    "wait:2" "type:prof syscalls"   "wait:4"

sleep 1
kill "$QPID" 2>/dev/null
wait "$QPID" 2>/dev/null

echo "=== profiler output ==="
sed -n '/PROFILER/,$p' serial-lsprof.log | head -60
