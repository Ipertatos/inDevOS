#!/usr/bin/env bash
cd /mnt/c/osproj || exit 1
T=/tmp/indev_bb2; rm -rf "$T"; mkdir -p "$T"
cp os_install.iso "$T/boot.iso"
cp sata_disk.img "$T/"
ACC=""; [ -w /dev/kvm ] && ACC="-accel kvm"
qemu-system-x86_64 $ACC \
    -boot d -drive file=$T/boot.iso,format=raw,index=2,media=cdrom -smp 4 -m 1G -M ${MACH:-pc} \
    -netdev user,id=net0 -device rtl8139,netdev=net0 \
    -drive file=$T/sata_disk.img,format=raw,index=0 \
    -device virtio-vga,xres=1920,yres=1080 \
    -display none -no-reboot \
    -serial file:$T/serial.log \
    -qmp tcp:127.0.0.1:4456,server,nowait \
    > $T/qemu.log 2>&1 &
QPID=$!
sleep ${BOOTWAIT:-35}
python3 /mnt/c/osproj/qmp_scenario.py 4456 /mnt/c/osproj/shot-bb2 \
    "wait:2" "type:mount 0" "wait:10" \
    "wait:2" "type:busybox sh -c echo hi_from_ash" "wait:6" \
    "wait:2" "type:busybox sh -c printf hiash" "wait:6" "shot:1-ashc" \
    "wait:2" "type:busybox mkdir /tmpbb" "wait:5" \
    "wait:2" "type:busybox sh -c echo hello>/tmpbb/a.txt" "wait:6" \
    "wait:2" "type:busybox cat /tmpbb/a.txt" "wait:5" \
    "wait:2" "type:busybox wc /tmpbb/a.txt" "wait:5" \
    "wait:2" "type:busybox head -1 /tmpbb/a.txt" "wait:5" \
    "wait:2" "type:busybox cp /tmpbb/a.txt /tmpbb/b.txt" "wait:5" \
    "wait:2" "type:busybox mv /tmpbb/b.txt /tmpbb/c.txt" "wait:5" \
    "wait:2" "type:busybox ls /tmpbb" "wait:5" \
    "wait:2" "type:busybox env" "wait:5" \
    "wait:2" "type:busybox date" "wait:5" \
    "wait:2" "type:busybox sh -c echo a|busybox wc -c" "wait:8" "shot:2-pipe" \
    "wait:2" "type:busybox rm /tmpbb/a.txt" "wait:5" \
    "wait:2" "type:ls" "wait:6" \
    "wait:2" "type:neofetch" "wait:6"
sleep 1
kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null
cp "$T/serial.log" /mnt/c/osproj/serial-busybox2.log
grep -naE "linux:|hiash|hi_from|hello|tmpbb|unimplemented|Page fault|PANIC|exit_group|uid=|Command not found|BusyBox|wc:|PATH=|from_ash" "$T/serial.log" | tail -100
echo "--- tail ---"; tail -50 "$T/serial.log"
