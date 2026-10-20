#!/usr/bin/env bash
# Boot inDevOS, start the desktop, spawn a windowed app, and capture the stages
# (desktop -> app window -> after the app exits -> shell still alive).
#
#   desktop_app_probe.sh [boot_wait] [app]
cd /mnt/c/osproj || exit 1

BOOT_WAIT="${1:-16}"
APP="${2:-minimal_sdl_test.exe}"

rm -f shot-app-*.png serial-app.log qemu-app.log

qemu-system-x86_64 \
    -boot d -cdrom os.iso -smp 4 -m 1G -M q35 \
    -netdev user,id=net0 -device rtl8139,netdev=net0 \
    -drive file=sata_disk.img,format=raw,index=0 \
    -drive file=nvme_disk.img,format=raw,index=1,media=disk \
    -device virtio-vga,xres=1920,yres=1080 \
    -display none -no-reboot \
    -serial file:serial-app.log \
    -qmp tcp:127.0.0.1:4447,server,nowait \
    > qemu-app.log 2>&1 &
QPID=$!

sleep "$BOOT_WAIT"

python3 /mnt/c/osproj/qmp_scenario.py 4447 /mnt/c/osproj/shot-app \
    "type:desktop" "wait:13" "shot:1-desktop" \
    "type:spawn $APP" "wait:2" "shot:2-app" \
    "wait:4" "shot:3-after" \
    "type:ls" "wait:3" "shot:4-shell"

sleep 1
kill "$QPID" 2>/dev/null
wait "$QPID" 2>/dev/null

ls -l shot-app-*.png 2>&1
echo "--- serial tail ---"
tail -30 serial-app.log 2>/dev/null
