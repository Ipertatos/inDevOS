#!/usr/bin/env bash
cd /mnt/c/osproj || exit 1
cp /mnt/c/Users/froze/qmp_scenario.py /mnt/c/osproj/qmp_scenario.py
T=/tmp/indev_gaps2; rm -rf "$T"; mkdir -p "$T"
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
    -serial "file:$serial" -qmp "tcp:127.0.0.1:4462,server,nowait" \
    >"$T/qemu-$tag.log" 2>&1 &
  local QPID=$!
  sleep "${BOOTWAIT:-35}"
  python3 /mnt/c/osproj/qmp_scenario.py 4462 "$T/shot-$tag" "$@"
  kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null
  cp "$serial" "/mnt/c/osproj/serial-gaps2-$tag.log"
  cp "$serial" "/mnt/c/Users/froze/serial-gaps2-$tag.log" 2>/dev/null || true
  echo "==== $tag ===="
  grep -naE "zeus:/|Created|Renamed|exit_group|can't |unimplemented|hiash|PIPE|Function|T\.TXT|A\.TXT|X\.EXE|H\.EXE|neofetch|wc:|Page fault|Directory grew" "$serial" | tail -160
}

echo "=== PASS 1 ==="
run_qemu p1 \
  "wait:2" "type:mount 0" "wait:12" \
  "wait:2" "type:busybox mkdir /tmpbb" "wait:4" \
  "wait:2" "type:busybox touch /tmpbb/t.txt" "wait:4" \
  "wait:2" "type:busybox ls /tmpbb" "wait:4" \
  "wait:2" "type:busybox sh -c 'printf hiash>/tmpbb/a.txt'" "wait:8" \
  "wait:2" "type:busybox cat /tmpbb/a.txt" "wait:5" \
  "wait:2" "type:busybox sh -c 'printf PIPEOK | busybox wc -c'" "wait:10" \
  "wait:2" "type:busybox cp /bin/hello_musl.exe /tmpbb/h.exe" "wait:8" \
  "wait:2" "type:busybox mv /tmpbb/h.exe /tmpbb/x.exe" "wait:5" \
  "wait:2" "type:busybox ls /tmpbb" "wait:4" \
  "wait:2" "type:busybox mkdir /tmpbb/d" "wait:4" \
  "wait:2" "type:busybox mv /tmpbb/d /tmpbb/e" "wait:5" \
  "wait:2" "type:busybox ls /tmpbb" "wait:4" \
  "wait:2" "type:busybox rm /tmpbb/t.txt" "wait:4" \
  "wait:2" "type:busybox rmdir /tmpbb/e" "wait:4" \
  "wait:2" "type:busybox ls /tmpbb" "wait:4" \
  "wait:2" "type:mkdir /tmpz" "wait:3" \
  "wait:2" "type:neofetch" "wait:6"

echo "=== PASS 2 persistence ==="
run_qemu p2 \
  "wait:2" "type:mount 0" "wait:12" \
  "wait:2" "type:busybox ls /tmpbb" "wait:5" \
  "wait:2" "type:busybox cat /tmpbb/a.txt" "wait:5" \
  "wait:2" "type:ls /tmpz" "wait:4"

echo DONE
