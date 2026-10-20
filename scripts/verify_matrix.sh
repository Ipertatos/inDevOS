#!/usr/bin/env bash
# Verify the ported matrix program (SDL window + single CopyPixels per frame)
# and re-check the rest of the shell/file utilities after the src/usoft removal.
OSPROJ="${OSPROJ:-$(cd "$(dirname "$0")/.." && pwd)}"; export OSPROJ  # repo root
cd ${OSPROJ} || exit 1

rm -f serial-matrix.log shot-matrix-*.png qemu-matrix.log

qemu-system-x86_64 \
    -boot d -cdrom os.iso -smp 4 -m 1G -M q35 \
    -netdev user,id=net0 -device rtl8139,netdev=net0 \
    -drive file=sata_disk.img,format=raw,index=0 \
    -drive file=nvme_disk.img,format=raw,index=1,media=disk \
    -device virtio-vga,xres=1920,yres=1080 \
    -display none -no-reboot \
    -serial file:serial-matrix.log \
    -qmp tcp:127.0.0.1:4458,server,nowait \
    > qemu-matrix.log 2>&1 &
QPID=$!

sleep 16

python3 ${OSPROJ}/qmp_scenario.py 4458 ${OSPROJ}/shot-matrix \
    "wait:2" "type:matrix"                  "wait:12" "shot:1-matrix" \
    "key:spc" "wait:4" "shot:2-after-matrix" \
    "wait:2" "type:mkdir /REGTEST"          "wait:6" \
    "wait:2" "type:touch /REGTEST/A.TXT"    "wait:6" \
    "wait:2" "type:cp /SCORES.TXT /REG.TXT" "wait:6" \
    "wait:2" "type:wc /REG.TXT"             "wait:5" \
    "wait:2" "type:rm /REG.TXT"             "wait:5" \
    "wait:2" "type:rm /REGTEST/A.TXT"       "wait:5" \
    "wait:2" "type:rm /REGTEST"             "wait:5" \
    "wait:2" "type:ls /"                    "wait:6" "shot:3-ls"

sleep 1
kill "$QPID" 2>/dev/null
wait "$QPID" 2>/dev/null

echo "=== matrix + regression evidence ==="
grep -nE "matrix|SDL|Created (empty file|directory)|Deleted file|cp:|cannot|Page fault|panic|exited \(status" serial-matrix.log | tail -35
