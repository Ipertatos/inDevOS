#!/usr/bin/env bash
# End-to-end check of the file utilities that now live on disk:
#   * cp / mv  (real data write through the syscall API)
#   * tab completion (registry owned by zeus.c)
# Afterwards it uses mtools - an independent FAT32 implementation - to prove
# the kernel's writes are laid out correctly for other systems to read.
cd /mnt/c/osproj || exit 1

rm -f serial-cpmv.log shot-cpmv-*.png qemu-cpmv.log
rm -f /tmp/copy-check.txt

qemu-system-x86_64 \
    -boot d -cdrom os.iso -smp 4 -m 1G -M q35 \
    -netdev user,id=net0 -device rtl8139,netdev=net0 \
    -drive file=sata_disk.img,format=raw,index=0 \
    -drive file=nvme_disk.img,format=raw,index=1,media=disk \
    -device virtio-vga,xres=1920,yres=1080 \
    -display none -no-reboot \
    -serial file:serial-cpmv.log \
    -qmp tcp:127.0.0.1:4455,server,nowait \
    > qemu-cpmv.log 2>&1 &
QPID=$!

sleep 16

python3 /mnt/c/osproj/qmp_scenario.py 4455 /mnt/c/osproj/shot-cpmv \
    "wait:2" "type:cp /SCORES.TXT /COPY.TXT"  "wait:6" \
    "wait:2" "type:wc /COPY.TXT"              "wait:5" \
    "wait:2" "type:head -3 /COPY.TXT"         "wait:5" "shot:1-copy" \
    "wait:2" "type:mv /COPY.TXT /MOVED.TXT"   "wait:6" \
    "wait:2" "type:ls /"                      "wait:5" "shot:2-after-mv" \
    "wait:2" "type:wc /COPY.TXT"              "wait:5" \
    "type:neo" "key:tab"                      "wait:2" "shot:3-tabcomplete"

sleep 1
kill "$QPID" 2>/dev/null
wait "$QPID" 2>/dev/null

echo "=== shell/program evidence ==="
grep -nE "cp:|mv:|cannot|not found|Page fault|panic|exited \(status" serial-cpmv.log | tail -30

echo "=== independent check with mtools ==="
mdir -i sata_disk.img ::/COPY.TXT 2>&1 | tail -3
mdir -i sata_disk.img ::/MOVED.TXT 2>&1 | tail -3
mtype -i sata_disk.img ::/MOVED.TXT > /tmp/copy-check.txt 2>/dev/null
if [ -s /tmp/copy-check.txt ]; then
    echo "--- first lines of MOVED.TXT as read by mtools ---"
    head -3 /tmp/copy-check.txt
    echo "--- byte counts ---"
    wc -c /tmp/copy-check.txt
    mtype -i sata_disk.img ::/SCORES.TXT > /tmp/scores-orig.txt 2>/dev/null
    wc -c /tmp/scores-orig.txt
    if cmp -s /tmp/copy-check.txt /tmp/scores-orig.txt; then
        echo "RESULT: copy is byte-identical to the source"
    else
        echo "RESULT: copy DIFFERS from the source"
    fi
else
    echo "RESULT: mtools could not read MOVED.TXT"
fi
