#!/usr/bin/env bash
# Rebuild os.iso only (limine.cfg / ISO layout changed; kernel untouched) and
# capture the Limine boot menu so the config change can be verified.
cd /mnt/c/osproj || exit 1

echo "=== rebuilding os.iso ==="
# The os.iso rule does not list limine.cfg as a prerequisite, so make thinks the
# ISO is up to date after a cfg-only change: force it.
rm -f os.iso
make os.iso 2>&1 | tail -8
echo "--- staged cfg ---"
grep -E 'TERM_FONT_SCALE|INTERFACE_RESOLUTION' iso/boot/limine/limine.cfg

echo "=== capturing the boot menu ==="
rm -f shot-menu.png qemu-menu.log serial-menu.log
qemu-system-x86_64 \
    -M q35 -m 1G -smp 4 -boot d -cdrom os.iso \
    -device virtio-vga,xres=1920,yres=1080 \
    -display none -no-reboot \
    -serial file:serial-menu.log \
    -qmp tcp:127.0.0.1:4450,server,nowait \
    > qemu-menu.log 2>&1 &
QPID=$!

sleep 3
# Screenshot before the 5 s TIMEOUT auto-boots, then press a key to hold the
# menu and take a second shot.
python3 /mnt/c/osproj/qmp_scenario.py 4450 /mnt/c/osproj/shot-menu "wait:2" "shot:menu-early" \
    "key:down" "wait:1" "shot:menu"
sleep 1
kill "$QPID" 2>/dev/null
wait "$QPID" 2>/dev/null

ls -l shot-menu*.png 2>&1
