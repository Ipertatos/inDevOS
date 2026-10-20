#!/usr/bin/env bash
# DOOM frame-rate benchmark: profile a DOOM run and report frames + syscall time.
#
#   doom_bench.sh [mode] [boot_wait] [run_seconds]
#     mode = exec   -> 'exec doom.exe'  (standalone, full screen; shell blocked)
#     mode = spawn  -> 'spawn doom.exe' (preemptively scheduled thread)
#     mode = desk   -> start the desktop, then spawn doom (dock-style launch)
cd /mnt/c/osproj || exit 1

MODE="${1:-exec}"
BOOT_WAIT="${2:-16}"
RUN_SECS="${3:-30}"

rm -f shot-doom-*.png serial-doom.log qemu-doom.log

qemu-system-x86_64 \
    -boot d -cdrom os.iso -smp 4 -m 1G -M q35 \
    -netdev user,id=net0 -device rtl8139,netdev=net0 \
    -drive file=sata_disk.img,format=raw,index=0 \
    -drive file=nvme_disk.img,format=raw,index=1,media=disk \
    -device virtio-vga,xres=1920,yres=1080 \
    -display none -no-reboot \
    -serial file:serial-doom.log \
    -qmp tcp:127.0.0.1:4448,server,nowait \
    > qemu-doom.log 2>&1 &
QPID=$!

sleep "$BOOT_WAIT"

case "$MODE" in
  desk)
    STEPS=("wait:2" "type:desktop" "wait:14"
           "wait:2" "type:prof reset" "wait:2" "type:prof start 8" "wait:2"
           "type:spawn doom.exe" "wait:$RUN_SECS" "shot:1-run"
           "wait:2" "type:prof stop" "wait:2" "type:prof syscalls" "wait:3"
           "type:prof stats" "wait:2")
    ;;
  spawn)
    STEPS=("wait:2" "type:prof reset" "wait:2" "type:prof start 8" "wait:2"
           "type:spawn doom.exe" "wait:$RUN_SECS" "shot:1-run"
           "wait:2" "type:prof stop" "wait:2" "type:prof syscalls" "wait:3"
           "type:prof stats" "wait:2")
    ;;
  *)
    STEPS=("wait:2" "type:prof reset" "wait:2" "type:prof start 64" "wait:2"
           "type:exec doom.exe" "wait:$RUN_SECS" "shot:1-run"
           "key:ctrl,c" "wait:3" "type:prof stop" "wait:2"
           "type:prof syscalls" "wait:3" "type:prof stats" "wait:2")
    ;;
esac

python3 /mnt/c/osproj/qmp_scenario.py 4448 /mnt/c/osproj/shot-doom "${STEPS[@]}"

sleep 1
kill "$QPID" 2>/dev/null
wait "$QPID" 2>/dev/null

echo "--- prof / doom lines ---"
grep -E 'profiler:|frames=|exec \+|returned from user mode|doom\.exe|Timed|gametic|prof ' serial-doom.log | tail -30
