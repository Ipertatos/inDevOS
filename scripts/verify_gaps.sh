#!/usr/bin/env bash
# Verify rename/touch/rm/rmdir, Zeus quotes+ash redir/pipe, sendfile, persistence.
OSPROJ="${OSPROJ:-$(cd "$(dirname "$0")/.." && pwd)}"; export OSPROJ  # repo root
WINHOME="${WINHOME:-$(wslpath "$(cmd.exe /c 'echo %USERPROFILE%' 2>/dev/null | tr -d '\r')" 2>/dev/null || echo "$HOME")}"; export WINHOME  # Windows user profile (logs, ws_* scratch)
cd ${OSPROJ} || exit 1
T=/tmp/indev_gaps; rm -rf "$T"; mkdir -p "$T"
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
    -serial "file:$serial" -qmp "tcp:127.0.0.1:4461,server,nowait" \
    >"$T/qemu-$tag.log" 2>&1 &
  local QPID=$!
  sleep "${BOOTWAIT:-35}"
  python3 ${OSPROJ}/qmp_scenario.py 4461 "$T/shot-$tag" "$@"
  kill "$QPID" 2>/dev/null
  wait "$QPID" 2>/dev/null
  cp "$serial" "${OSPROJ}/serial-gaps-$tag.log"
  cp "$serial" "${WINHOME}/serial-gaps-$tag.log" 2>/dev/null || true
  echo "==== serial-$tag ===="
  grep -naE "Unmount|VFS: FAT32|Created|Renamed|Directory grew|tmpbb|tmpz|tmpd|exit_group|can't |unimplemented|hiash|Function not|ENOTEMPTY|neofetch|Page fault|A\.TXT|T\.TXT|H\.EXE|X\.EXE|PIPEOK|sendfile" "$serial" | tail -140
}

echo "=== PASS 1: ops ==="
run_qemu p1 \
  "wait:2" "type:mount 0" "wait:12" \
  "wait:2" "type:busybox mkdir /tmpbb" "wait:4" \
  "wait:2" "type:busybox touch /tmpbb/t.txt" "wait:4" \
  "wait:2" "type:busybox ls /tmpbb" "wait:4" \
  "wait:2" "type:busybox sh -c 'printf hiash>/tmpbb/a.txt'" "wait:6" \
  "wait:2" "type:busybox cat /tmpbb/a.txt" "wait:4" \
  "wait:2" "type:busybox sh -c 'printf PIPEOK | busybox wc -c'" "wait:6" \
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

echo "=== PASS 2: persistence ==="
run_qemu p2 \
  "wait:2" "type:mount 0" "wait:12" \
  "wait:2" "type:busybox ls /tmpbb" "wait:5" \
  "wait:2" "type:busybox cat /tmpbb/a.txt" "wait:5" \
  "wait:2" "type:ls /tmpz" "wait:4"

echo "DONE gaps harness"
