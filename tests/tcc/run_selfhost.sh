#!/usr/bin/env bash
# Build ws ISO and run self-host scenario in QEMU (unique paths/port).
set -u
WS=/mnt/c/Users/froze/ws_tcc
T=/tmp/indev_tcc_ws_q
PORT=${PORT:-4473}
TAG=${1:-sh1}
cd "$WS" || exit 1
if [ "${NOISO:-0}" != 1 ]; then
  python3 tests/tcc/patch_iso.py 2>/dev/null || true
  bash ./make_install_iso.sh > "$WS/selfhost_iso.log" 2>&1 || { tail -20 "$WS/selfhost_iso.log"; exit 1; }
  grep -E 'make.exe|tcc.exe' "$WS/selfhost_iso.log"
fi
rm -rf "$T"; mkdir -p "$T"
cp os_install.iso "$T/disk.img"
qemu-system-x86_64 -boot c -drive "file=$T/disk.img,format=raw,index=0" \
  -smp 4 -m 1G -M pc -netdev user,id=net0 -device rtl8139,netdev=net0 \
  -device virtio-vga,xres=1920,yres=1080 -display none -no-reboot \
  -serial "file:$T/serial.log" -qmp "tcp:127.0.0.1:$PORT,server,nowait" >"$T/qemu.log" 2>&1 &
QPID=$!
for i in $(seq 1 150); do grep -aq "mouse\] POLL: first" "$T/serial.log" 2>/dev/null && break; sleep 1; done
echo "boot-ready ${i}s"; sleep 8
shift || true
python3 "$WS/qmp_step.py" $PORT "$T/s" "$@" || true
sleep 3
kill $QPID 2>/dev/null; wait $QPID 2>/dev/null
cp -f "$T/serial.log" "$WS/serial-selfhost-$TAG.log"
grep -aE "zeus:/|RC=|OK|tcc version|GNU Make|Page fault|exception|GPF|not an exec|unimpl|ENOSYS|error|Command not" "$T/serial.log" | grep -v kfree | tail -80
