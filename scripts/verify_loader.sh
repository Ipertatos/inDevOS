#!/usr/bin/env bash
# Final check of the loader work:
#   * execsym off/on        - ELF symtab loading is now opt-in
#   * time <program>        - launch cost comparison
#   * plain utility sweep   - nothing regressed, no faults
OSPROJ="${OSPROJ:-$(cd "$(dirname "$0")/.." && pwd)}"; export OSPROJ  # repo root
cd ${OSPROJ} || exit 1

rm -f serial-loader.log shot-loader-*.png qemu-loader.log

qemu-system-x86_64 \
    -boot d -cdrom os.iso -smp 4 -m 1G -M q35 \
    -netdev user,id=net0 -device rtl8139,netdev=net0 \
    -drive file=sata_disk.img,format=raw,index=0 \
    -drive file=nvme_disk.img,format=raw,index=1,media=disk \
    -device virtio-vga,xres=1920,yres=1080 \
    -display none -no-reboot \
    -serial file:serial-loader.log \
    -qmp tcp:127.0.0.1:4471,server,nowait \
    > qemu-loader.log 2>&1 &
QPID=$!

sleep 16

python3 ${OSPROJ}/qmp_scenario.py 4471 ${OSPROJ}/shot-loader \
    "wait:2" "type:execsym"                "wait:5" \
    "wait:2" "type:time ls /"              "wait:12" \
    "wait:2" "type:time ls /"              "wait:12" "shot:1-time" \
    "wait:2" "type:time help"              "wait:12" \
    "wait:2" "type:execsym on"             "wait:5" \
    "wait:2" "type:ls /TESTDIR"            "wait:8" \
    "wait:2" "type:execsym off"            "wait:5" \
    "wait:2" "type:ls /"                   "wait:10" "shot:2-ls"

sleep 1
kill "$QPID" 2>/dev/null
wait "$QPID" 2>/dev/null

echo "=== execsym + timings ==="
grep -aE "execsym:|ELF symbol|time: .* took|shell lookup" serial-loader.log | tail -14
echo "=== old/absent markers ==="
grep -aE "loaded [0-9]+ ELF symbol" serial-loader.log | tail -4
echo "=== faults: $(grep -ac 'PAGE FAULT' serial-loader.log) ==="
