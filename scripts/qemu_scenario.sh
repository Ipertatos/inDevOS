#!/usr/bin/env bash
# Boot os.iso headless in QEMU and drive it through a qmp_scenario.py script.
#
# Usage: scripts/qemu_scenario.sh <out-prefix> <step> [<step> ...]
#   Steps are passed to qmp_scenario.py (wait:S  type:TEXT  raw:TEXT  key:K  shot:NAME).
#   Screenshots land at <out-prefix>-<NAME>.png, serial log at <out-prefix>-serial.log.
#
# Env: QMP_PORT (default 4445), SMP (default 4), BOOT_WAIT (default 8 s),
#      CPU (default qemu64,+smep,+smap,+nx so the hardening paths run),
#      SATA_IMG / NVME_IMG (default sata_disk.img / nvme_disk.img).
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

prefix="$1"; shift
port="${QMP_PORT:-4445}"
serial="${prefix}-serial.log"
sata="${SATA_IMG:-sata_disk.img}"
nvme="${NVME_IMG:-nvme_disk.img}"

drives=()
[[ -f "$sata" ]] && drives+=(-drive "file=$sata,format=raw,index=0,snapshot=on")
[[ -f "$nvme" ]] && drives+=(-drive "file=$nvme,format=raw,index=1,media=disk,snapshot=on")

rm -f "$serial"
qemu-system-x86_64 \
    -boot d -cdrom os.iso -smp "${SMP:-4}" -m 1G -M q35 \
    -cpu "${CPU:-qemu64,+smep,+smap,+nx}" \
    -netdev user,id=net0 -device rtl8139,netdev=net0 \
    "${drives[@]}" \
    -device virtio-vga,xres=1920,yres=1080 \
    -usb -device usb-tablet \
    -display none -no-reboot \
    -qmp "tcp:127.0.0.1:${port},server,nowait" \
    -serial "file:${serial}" &
qemu_pid=$!

sleep "${BOOT_WAIT:-8}"
python3 qmp_scenario.py "$port" "$prefix" "$@"
rc=$?

# qmp_scenario.py sends "quit"; make sure QEMU is gone either way.
sleep 1
kill "$qemu_pid" 2>/dev/null
wait "$qemu_pid" 2>/dev/null
exit "$rc"
