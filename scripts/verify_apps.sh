#!/usr/bin/env bash
# Verify that the shell dispatches utility commands to the programs installed
# on the filesystem (apps/*.c -> /<name>.exe) and that the kernel survives.
OSPROJ="${OSPROJ:-$(cd "$(dirname "$0")/.." && pwd)}"; export OSPROJ  # repo root
cd ${OSPROJ} || exit 1

rm -f serial-apps.log shot-apps-*.png qemu-apps.log

qemu-system-x86_64 \
    -boot d -cdrom os.iso -smp 4 -m 1G -M q35 \
    -netdev user,id=net0 -device rtl8139,netdev=net0 \
    -drive file=sata_disk.img,format=raw,index=0 \
    -drive file=nvme_disk.img,format=raw,index=1,media=disk \
    -device virtio-vga,xres=1920,yres=1080 \
    -display none -no-reboot \
    -serial file:serial-apps.log \
    -qmp tcp:127.0.0.1:4452,server,nowait \
    > qemu-apps.log 2>&1 &
QPID=$!

sleep 16

python3 ${OSPROJ}/qmp_scenario.py 4452 ${OSPROJ}/shot-apps \
    "wait:2" "type:pwd"                  "wait:5" \
    "wait:2" "type:neofetch"             "wait:5" "shot:1-neofetch" \
    "wait:2" "type:help"                 "wait:5" "shot:2-help" \
    "wait:2" "type:echo hello world"     "wait:5" \
    "wait:2" "type:mkdir /TESTDIR"       "wait:5" \
    "wait:2" "type:touch /TESTDIR/A.TXT" "wait:5" \
    "wait:2" "type:ls /TESTDIR"          "wait:5" "shot:3-ls-dir" \
    "wait:2" "type:wc /SCORES.TXT"       "wait:5" \
    "wait:2" "type:head -3 /SCORES.TXT"  "wait:5" \
    "wait:2" "type:rm /TESTDIR/A.TXT"    "wait:5" \
    "wait:2" "type:ls"                   "wait:5" "shot:4-ls-root"

sleep 1
kill "$QPID" 2>/dev/null
wait "$QPID" 2>/dev/null

echo "--- dispatch + program/exec evidence ---"
grep -nE "launching '|exec_spawned_exit|returned from user mode|Page fault|Command not found|exec: process" serial-apps.log | tail -40
echo "--- program output ---"
grep -nE "uid=|video:|root :|program\(s\) on disk|entr|from /echo|Total:|usage:" serial-apps.log | tail -25
