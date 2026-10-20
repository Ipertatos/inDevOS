#!/usr/bin/env bash
# Private UEFI install test for ws_uefi. QMP ports 4551/4552. No global kills.
WINHOME="${WINHOME:-$(wslpath "$(cmd.exe /c 'echo %USERPROFILE%' 2>/dev/null | tr -d '\r')" 2>/dev/null || echo "$HOME")}"; export WINHOME  # Windows user profile (logs, ws_* scratch)
set -u
W=${WINHOME}/ws_uefi
T=/tmp/indev_uefi_t
MODE=${1:-gpt}
cd "$W"
rm -rf "$T"; mkdir -p "$T"
OVMF=""
for f in /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_CODE.fd /usr/share/ovmf/OVMF.fd /usr/share/qemu/OVMF.fd; do [ -f "$f" ] && { OVMF=$f; break; }; done
[ -n "$OVMF" ] || { echo "NO_OVMF"; exit 2; }
VARS=""
for f in /usr/share/OVMF/OVMF_VARS_4M.fd /usr/share/OVMF/OVMF_VARS.fd; do [ -f "$f" ] && { VARS=$f; break; }; done
echo "OVMF=$OVMF VARS=$VARS MODE=$MODE"
cp -f os_install.iso "$T/live.iso"
truncate -s 512M "$T/nvme.img"
FW=(-drive "if=pflash,format=raw,readonly=on,file=$OVMF")
if [ -n "$VARS" ]; then cp -f "$VARS" "$T/vars.fd"; FW+=(-drive "if=pflash,format=raw,file=$T/vars.fd"); fi
[ "$MODE" = bios ] && FW=()
# Phase 1: boot ISO on UEFI, install to NVMe
qemu-system-x86_64 "${FW[@]}" -M pc -m 1G -smp 2 \
  -drive "file=$T/live.iso,format=raw,if=none,id=cd,media=cdrom" -device ide-cd,drive=cd,bus=ide.1,bootindex=0 \
  -drive "file=$T/nvme.img,format=raw,if=none,id=nv" -device nvme,serial=uefi1,drive=nv \
  -vga std -display none -no-reboot -serial "file:$T/s1.log" \
  -qmp "tcp:127.0.0.1:4551,server,nowait" >"$T/q1.log" 2>&1 &
P=$!
for i in $(seq 1 180); do grep -aq "mouse\] POLL: first\|zeus" "$T/s1.log" 2>/dev/null && break; sleep 1; done
echo "phase1 boot-ready ${i}s"; sleep 8
grep -a "storage: \|Limine boot file\|device [0-9]*:" "$T/s1.log" | head -20
python3 qmp_step.py 4551 "$T/i" "wait:2" "type:install 0 yes $MODE" "wait:8" || true
if grep -aq "refusing device 0" "$T/s1.log"; then
  DEV=$(grep -a "install: device [0-9]*: .*MB)$" "$T/s1.log" | grep -v "not a target" | head -1 | sed -E 's/.*device ([0-9]+):.*/\1/')
  echo "retry with device $DEV"
  python3 qmp_step.py 4551 "$T/j" "wait:2" "type:install $DEV yes $MODE" "wait:5" || true
fi
for i in $(seq 1 420); do grep -aq "install: complete\|install: .*failed\|aborted\|refusing" "$T/s1.log" && break; sleep 1; done
echo "install wait ${i}s"; sleep 3
kill $P 2>/dev/null; wait $P 2>/dev/null
grep -a "install: \|GPT\|fat32: Formatting\|EFI" "$T/s1.log" | grep -v "install: read \|install: write \|install: mkdir " | tail -25
# Phase 2: boot NVMe only
if [ -n "$VARS" ]; then cp -f "$VARS" "$T/vars2.fd"; FW=(-drive "if=pflash,format=raw,readonly=on,file=$OVMF" -drive "if=pflash,format=raw,file=$T/vars2.fd"); fi
[ "$MODE" = bios ] && FW=()
qemu-system-x86_64 "${FW[@]}" -M pc -m 1G -smp 2 \
  -drive "file=$T/nvme.img,format=raw,if=none,id=nv" -device nvme,serial=uefi1,drive=nv,bootindex=0 \
  -vga std -display none -no-reboot -serial "file:$T/s2.log" \
  -qmp "tcp:127.0.0.1:4552,server,nowait" >"$T/q2.log" 2>&1 &
P=$!
for i in $(seq 1 120); do grep -aq "mouse\] POLL: first" "$T/s2.log" 2>/dev/null && break; sleep 1; done
echo "phase2 boot-ready ${i}s"; sleep 6
python3 qmp_step.py 4552 "$T/k" "wait:2" "type:ls /EFI/BOOT" "wait:3" "type:ls /bin" "wait:3" "type:echo UEFI_NVME_BOOT_OK" "wait:3" || true
sleep 2; kill $P 2>/dev/null; wait $P 2>/dev/null
grep -a "Limine boot file\|storage: Chosen\|storage: .*boot\|Mounting FAT32\|BOOTX64\|UEFI_NVME_BOOT_OK\|Page fault\|panic" "$T/s2.log" | head -20
echo "s2 lines: $(wc -l < $T/s2.log)"
cp -f "$T/s1.log" "$W/uefi_${MODE}_install_serial.log"; cp -f "$T/s2.log" "$W/uefi_${MODE}_boot_serial.log"
echo UEFI_TEST_DONE
