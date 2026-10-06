#!/usr/bin/env bash
# Boot os_install.iso DVD + sata, mount writable disk, exercise busybox.
cd /mnt/c/osproj || exit 1
T=/tmp/indev_bb; rm -rf "$T"; mkdir -p "$T"
ISO=${ISO:-os_install.iso}
cp "$ISO" "$T/boot.iso"
cp sata_disk.img "$T/"
ACC=""; [ -w /dev/kvm ] && ACC="-accel kvm"
echo "accel: ${ACC:-tcg}"
qemu-system-x86_64 $ACC \
    -boot d -drive file=$T/boot.iso,format=raw,index=2,media=cdrom -smp 4 -m 1G -M ${MACH:-pc} \
    -netdev user,id=net0 -device rtl8139,netdev=net0 \
    -drive file=$T/sata_disk.img,format=raw,index=0 \
    -device virtio-vga,xres=1920,yres=1080 \
    -display none -no-reboot \
    -serial file:$T/serial.log \
    -qmp tcp:127.0.0.1:4455,server,nowait \
    > $T/qemu.log 2>&1 &
QPID=$!
sleep ${BOOTWAIT:-35}
python3 /mnt/c/osproj/qmp_scenario.py 4455 /mnt/c/osproj/shot-bb \
    "wait:2" "type:storage" "wait:5" \
    "wait:2" "type:mount 0" "wait:10" \
    "wait:2" "type:ls /bin" "wait:6" "shot:0-ls" \
    "wait:2" "type:busybox" "wait:8" "shot:1-busybox" \
    "wait:2" "type:busybox echo hello_bb" "wait:5" \
    "wait:2" "type:busybox uname -a" "wait:5" \
    "wait:2" "type:busybox ls /bin" "wait:6" \
    "wait:2" "type:busybox pwd" "wait:4" \
    "wait:2" "type:busybox true" "wait:3" \
    "wait:2" "type:busybox false" "wait:3" \
    "wait:2" "type:busybox sh -c echo from_ash" "wait:6" "shot:2-ash" \
    "wait:2" "type:neofetch" "wait:6" "shot:3-neofetch" \
    "wait:2" "type:hello_musl" "wait:6"
sleep 1
kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null
cp "$T/serial.log" /mnt/c/osproj/serial-busybox.log
echo "--- evidence ---"
grep -naE "storage:|Mount|linux:|busybox|hello|ash|unimplemented|Page fault|PANIC|uid=|ALL OK|Command not found|exit_group|from_ash|BusyBox" "$T/serial.log" | tail -120
echo "--- tail ---"; tail -60 "$T/serial.log"
