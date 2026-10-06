#!/usr/bin/env bash
# Headless QEMU smoke for TCC on inDevOS.
set -euo pipefail
cd /mnt/c/osproj || exit 1
cp /mnt/c/Users/froze/qmp_scenario.py /mnt/c/osproj/qmp_scenario.py 2>/dev/null || true
T=/tmp/indev_tcc
rm -rf "$T"; mkdir -p "$T"
cp os_install.iso "$T/boot.iso"
cp sata_disk.img "$T/sata_disk.img"

run_qemu() {
  local tag="$1"; shift
  local serial="$T/serial-$tag.log"
  qemu-system-x86_64 -boot d \
    -drive "file=$T/boot.iso,format=raw,index=2,media=cdrom" \
    -smp 4 -m 1G -M pc \
    -netdev user,id=net0 -device rtl8139,netdev=net0 \
    -drive "file=$T/sata_disk.img,format=raw,index=0" \
    -device virtio-vga,xres=1920,yres=1080 -display none -no-reboot \
    -serial "file:$serial" -qmp "tcp:127.0.0.1:4471,server,nowait" \
    >"$T/qemu-$tag.log" 2>&1 &
  local QPID=$!
  sleep "${BOOTWAIT:-40}"
  python3 /mnt/c/osproj/qmp_scenario.py 4471 "$T/shot-$tag" "$@" || true
  sleep 2
  kill "$QPID" 2>/dev/null || true
  wait "$QPID" 2>/dev/null || true
  cp "$serial" "/mnt/c/osproj/serial-tcc-$tag.log"
  cp "$serial" "/mnt/c/Users/froze/serial-tcc-$tag.log" 2>/dev/null || true
  echo "==== $tag ===="
  grep -naE "zeus:/|tcc|hello|work|multi|unimplemented|Page fault|Stack smash|exit_group|Created|error|Error|ENOENT|PIPE|neofetch|musl|mprotect|mmap|ENOSYS|criterion|ok" "$serial" | tail -150
}

echo "=== TCC PASS 1 ==="
run_qemu p1 \
  "wait:2" "type:mount 0" "wait:14" \
  "wait:2" "type:busybox ls /bin/tcc.exe" "wait:5" \
  "wait:2" "type:busybox ls /usr/lib" "wait:5" \
  "wait:2" "type:tcc -v" "wait:8" \
  "wait:2" "type:busybox cat /home/user/hello.c" "wait:5" \
  "wait:2" "type:tcc -o /tmp/hello /home/user/hello.c" "wait:25" \
  "wait:2" "type:busybox ls -l /tmp/hello" "wait:5" \
  "wait:2" "type:/tmp/hello" "wait:8" \
  "wait:2" "type:tcc -run /home/user/hello.c" "wait:15" \
  "wait:2" "type:tcc -o /tmp/work /home/user/work.c" "wait:30" \
  "wait:2" "type:/tmp/work" "wait:10" \
  "wait:2" "type:tcc -o /tmp/multi /home/user/a.c /home/user/b.c" "wait:30" \
  "wait:2" "type:/tmp/multi" "wait:8" \
  "wait:2" "type:busybox sh -c 'echo int main(){puts(\"ashok\");} > /tmp/t.c; tcc /tmp/t.c -o /tmp/t; /tmp/t'" "wait:25" \
  "wait:2" "type:busybox sh -c 'printf PIPEOK | busybox wc -c'" "wait:12" \
  "wait:2" "type:neofetch" "wait:8"

echo DONE
