#!/usr/bin/env bash
# Boot os.iso + copies of the disk images headless and run the musl tests.
OSPROJ="${OSPROJ:-$(cd "$(dirname "$0")/.." && pwd)}"; export OSPROJ  # repo root
cd ${OSPROJ} || exit 1
T=/tmp/indev_musl; rm -rf $T; mkdir -p $T
ISO=${ISO:-os_install.iso}; cp $ISO $T/boot.iso; cp sata_disk.img $T/ 
NV=""
ACC=""; [ -w /dev/kvm ] && ACC="-accel kvm"
echo "accel: ${ACC:-tcg}"
qemu-system-x86_64 $ACC \
    -boot d -drive file=$T/boot.iso,format=raw,index=2,media=cdrom -smp 4 -m 1G -M ${MACH:-pc} \
    -netdev user,id=net0 -device rtl8139,netdev=net0 \
    -drive file=$T/sata_disk.img,format=raw,index=0 $NV \
    -device virtio-vga,xres=1920,yres=1080 \
    -display none -no-reboot \
    -serial file:$T/serial.log \
    -qmp tcp:127.0.0.1:4453,server,nowait \
    > $T/qemu.log 2>&1 &
QPID=$!
sleep ${BOOTWAIT:-20}
python3 ${OSPROJ}/qmp_scenario.py 4453 ${OSPROJ}/shot-musl \
    "wait:2" "type:ls /bin" "wait:6" "shot:0-ls" "wait:2" "type:hello_musl a b"      "wait:6" "shot:1-hello" \
    "wait:2" "type:musltest"            "wait:10" "shot:2-musltest" \
    "wait:2" "type:neofetch"            "wait:6" "shot:3-neofetch" \
    "wait:2" "type:/bin/hello_musl.exe" "wait:6" "shot:4-hello-abs"
sleep 1
kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null
cp $T/serial.log ${OSPROJ}/serial-musl.log
echo "--- qemu.log ---"; cat $T/qemu.log
echo "--- linux evidence ---"
grep -naE "storage:|Mount|linux|musl|hello|exit_group|unimplemented|Page fault|exception|PANIC|launching|Command not found|uid=|ALL OK|FAIL" $T/serial.log | tail -80
echo "--- serial tail ---"; tail -40 $T/serial.log
