#!/usr/bin/env bash
# Boot os_install.iso as DVD, remount writable sata_disk.img as /, run musltest.
OSPROJ="${OSPROJ:-$(cd "$(dirname "$0")/.." && pwd)}"; export OSPROJ  # repo root
cd ${OSPROJ} || exit 1
T=/tmp/indev_muslrw; rm -rf $T; mkdir -p $T
ISO=${ISO:-os_install.iso}; cp $ISO $T/boot.iso; cp sata_disk.img $T/
ACC=""; [ -w /dev/kvm ] && ACC="-accel kvm"
echo "accel: ${ACC:-tcg}"
qemu-system-x86_64 $ACC \
    -boot d -drive file=$T/boot.iso,format=raw,index=2,media=cdrom -smp 4 -m 1G -M ${MACH:-pc} \
    -netdev user,id=net0 -device rtl8139,netdev=net0 \
    -drive file=$T/sata_disk.img,format=raw,index=0 \
    -device virtio-vga,xres=1920,yres=1080 \
    -display none -no-reboot \
    -serial file:$T/serial.log \
    -qmp tcp:127.0.0.1:4454,server,nowait \
    > $T/qemu.log 2>&1 &
QPID=$!
sleep ${BOOTWAIT:-30}
python3 ${OSPROJ}/qmp_scenario.py 4454 ${OSPROJ}/shot-muslrw \
    "wait:2" "type:storage" "wait:6" \
    "wait:2" "type:mount 0" "wait:10" "shot:0-mounted" \
    "wait:2" "type:ls /bin" "wait:6" \
    "wait:2" "type:hello_musl a b" "wait:6" "shot:1-hello" \
    "wait:2" "type:musltest" "wait:12" "shot:2-musltest" \
    "wait:2" "type:ls /" "wait:6" \
    "wait:2" "type:neofetch" "wait:6"
sleep 1
kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null
cp $T/serial.log ${OSPROJ}/serial-muslrw.log
echo "--- qemu.log ---"; cat $T/qemu.log
echo "--- linux evidence ---"
grep -naE "storage:|Mount|linux|musl|hello|exit_group|unimplemented|Page fault|exception|PANIC|launching|Command not found|uid=|ALL OK|FAIL|Failed to write|Filesystem" $T/serial.log | tail -100
echo "--- serial tail ---"; tail -50 $T/serial.log