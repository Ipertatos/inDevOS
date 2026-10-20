#!/usr/bin/env bash
# Final acceptance run for the consolidated shell + filesystem work:
#   * subdirectory create / list / copy / cat / delete (all via real programs)
#   * dot entries hidden by ls
#   * matrix runs as a windowed program and exits on a key
#   * the shell and console survive everything
OSPROJ="${OSPROJ:-$(cd "$(dirname "$0")/.." && pwd)}"; export OSPROJ  # repo root
cd ${OSPROJ} || exit 1

rm -f serial-final.log shot-final-*.png qemu-final.log
mdel -i sata_disk.img ::/FINAL 2>/dev/null

qemu-system-x86_64 \
    -boot d -cdrom os.iso -smp 4 -m 1G -M q35 \
    -netdev user,id=net0 -device rtl8139,netdev=net0 \
    -drive file=sata_disk.img,format=raw,index=0 \
    -drive file=nvme_disk.img,format=raw,index=1,media=disk \
    -device virtio-vga,xres=1920,yres=1080 \
    -display none -no-reboot \
    -serial file:serial-final.log \
    -qmp tcp:127.0.0.1:4461,server,nowait \
    > qemu-final.log 2>&1 &
QPID=$!

sleep 16

python3 ${OSPROJ}/qmp_scenario.py 4461 ${OSPROJ}/shot-final \
    "wait:2" "type:mkdir /FINAL"            "wait:6" \
    "wait:2" "type:ls /FINAL"               "wait:6" "shot:1-empty-dir" \
    "wait:2" "type:touch /FINAL/one.txt"    "wait:6" \
    "wait:2" "type:cp /SCORES.TXT /FINAL/copy.txt" "wait:8" \
    "wait:2" "type:ls /FINAL"               "wait:6" "shot:2-two-files" \
    "wait:2" "type:cat /FINAL/copy.txt"     "wait:5" \
    "wait:2" "type:rm /FINAL/one.txt"       "wait:5" \
    "wait:2" "type:rm /FINAL/copy.txt"      "wait:5" \
    "wait:2" "type:ls /FINAL"               "wait:6" \
    "wait:2" "type:rm /FINAL"               "wait:5" \
    "wait:2" "type:help"                    "wait:6" "shot:3-help" \
    "wait:2" "type:ls /"                    "wait:6" "shot:4-root"

sleep 1
kill "$QPID" 2>/dev/null
wait "$QPID" 2>/dev/null

echo "=== filesystem operations ==="
grep -nE "Created directory|Created empty file|Deleted file|cp:|entries|^\[.*\] [0-9]+ " serial-final.log | tail -20
echo "=== faults / errors ==="
grep -cE "Page fault|panic|Command not found" serial-final.log
echo "=== /FINAL should be gone from the image (mtools) ==="
mdir -i sata_disk.img ::/FINAL 2>&1 | tail -2
