#!/usr/bin/env bash
cd /mnt/c/osproj || exit 1
T=/tmp/indev_bb3; rm -rf "$T"; mkdir -p "$T"
cp os_install.iso "$T/boot.iso"; cp sata_disk.img "$T/"
qemu-system-x86_64 -boot d -drive file=$T/boot.iso,format=raw,index=2,media=cdrom -smp 4 -m 1G -M pc \
    -netdev user,id=net0 -device rtl8139,netdev=net0 \
    -drive file=$T/sata_disk.img,format=raw,index=0 \
    -device virtio-vga,xres=1920,yres=1080 -display none -no-reboot \
    -serial file:$T/serial.log -qmp tcp:127.0.0.1:4457,server,nowait >$T/qemu.log 2>&1 &
QPID=$!; sleep ${BOOTWAIT:-35}
python3 /mnt/c/osproj/qmp_scenario.py 4457 /mnt/c/osproj/shot-bb3 \
    "wait:2" "type:mount 0" "wait:12" \
    "wait:2" "type:busybox mkdir /tmpbb" "wait:5" \
    "wait:2" "type:busybox cp /bin/hello_musl.exe /tmpbb/h.exe" "wait:8" \
    "wait:2" "type:busybox ls /tmpbb" "wait:5" "shot:1-ls" \
    "wait:2" "type:busybox wc /tmpbb/h.exe" "wait:5" \
    "wait:2" "type:busybox head -c 8 /tmpbb/h.exe" "wait:5" \
    "wait:2" "type:busybox mv /tmpbb/h.exe /tmpbb/x.exe" "wait:5" \
    "wait:2" "type:busybox rm /tmpbb/x.exe" "wait:5" \
    "wait:2" "type:busybox ls /tmpbb" "wait:5" \
    "wait:2" "type:busybox sh -c printf hiash" "wait:6" "shot:2-ash" \
    "wait:2" "type:neofetch" "wait:6"
kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null
cp "$T/serial.log" /mnt/c/osproj/serial-busybox3.log
grep -naE "Unmount|VFS: FAT32|linux:|tmpbb|hiash|unimplemented|Page fault|exit_group|uid=|wc:|mkdir|cp:|mv:|rm:|Command not found|BusyBox|hiash|ELF|printf" "$T/serial.log" | tail -80
