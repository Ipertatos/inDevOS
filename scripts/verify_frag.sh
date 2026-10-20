#!/usr/bin/env bash
# Is the launch cost dominated by file fragmentation?
#   * time ls /          -> launches the mcopy-written /ls.exe (possibly fragmented)
#   * cp /ls.exe /LSC.EXE -> an in-OS copy, written sequentially by our allocator
#   * time /LSC.EXE /     -> same program, freshly laid out
cd /mnt/c/osproj || exit 1

rm -f serial-frag.log shot-frag-*.png qemu-frag.log
mdel -i sata_disk.img ::/LSC.EXE 2>/dev/null

qemu-system-x86_64 \
    -boot d -cdrom os.iso -smp 4 -m 1G -M q35 \
    -netdev user,id=net0 -device rtl8139,netdev=net0 \
    -drive file=sata_disk.img,format=raw,index=0 \
    -drive file=nvme_disk.img,format=raw,index=1,media=disk \
    -device virtio-vga,xres=1920,yres=1080 \
    -display none -no-reboot \
    -serial file:serial-frag.log \
    -qmp tcp:127.0.0.1:4469,server,nowait \
    > qemu-frag.log 2>&1 &
QPID=$!

sleep 16

python3 /mnt/c/osproj/qmp_scenario.py 4469 /mnt/c/osproj/shot-frag \
    "wait:2" "type:clear"                  "wait:3" \
    "wait:2" "type:time ls /"              "wait:12" \
    "wait:2" "type:time ls /"              "wait:12" \
    "wait:2" "type:cp /ls.exe /LSC.EXE"    "wait:10" \
    "wait:2" "type:clear"                  "wait:3" \
    "wait:2" "type:time /LSC.EXE /"        "wait:12" \
    "wait:2" "type:time /LSC.EXE /"        "wait:12" \
    "wait:2" "type:rm /LSC.EXE"            "wait:8"

sleep 1
kill "$QPID" 2>/dev/null
wait "$QPID" 2>/dev/null

echo "=== timings (fragmented mcopy file vs in-OS copy) ==="
grep -aE "time: .* took|program file read" serial-frag.log | tail -14
