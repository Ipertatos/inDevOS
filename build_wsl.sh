#!/usr/bin/env bash
# Build inDevOS with the Linux Makefile (inside WSL) and run a headless boot
# smoke test, writing everything to build-wsl.log / serial-wsl.log.
#
# This is the CI-equivalent path: host gcc + nasm + ld + xorriso + Limine.
# The Makefile's own "run" target needs sudo + a tap device, so the boot test
# below uses QEMU user-mode networking instead (same devices as CI).

cd /mnt/c/osproj || exit 1
LOG=/mnt/c/osproj/build-wsl.log
SERIAL=/mnt/c/osproj/serial-wsl.log
: > "$LOG"

log() { echo "$*" >> "$LOG"; }

run() {
    log "### \$ $*"
    "$@" >> "$LOG" 2>&1
    local rc=$?
    log "### exit=$rc"
    return $rc
}

log "=== inDevOS WSL build $(date) ==="
log "gcc: $(gcc --version | head -1)"
log "nproc: $(nproc)"

log ""
log "=== clean ==="
run make clean

log ""
log "=== limine host tool ==="
run make -C limine

log ""
log "=== kernel ==="
run make kernel_final.bin -j"$(nproc)"

log ""
log "=== iso ==="
run make os.iso

if [ ! -f os.iso ]; then
    log "!!! os.iso missing - skipping boot test"
    exit 1
fi

log ""
log "=== boot smoke test (headless QEMU, user networking) ==="
log "sata_disk.img: $(stat -c '%s bytes' sata_disk.img 2>/dev/null || echo missing)"
rm -f "$SERIAL"
timeout 120 qemu-system-x86_64 \
    -boot d -cdrom os.iso -smp 4 -m 1G -M q35 \
    -netdev user,id=net0 -device rtl8139,netdev=net0 \
    -drive file=sata_disk.img,format=raw,index=0 \
    -drive file=nvme_disk.img,format=raw,index=1,media=disk \
    -device virtio-vga,xres=1920,yres=1080 \
    -display none -no-reboot \
    -serial "file:$SERIAL" >> "$LOG" 2>&1
log "### qemu exit=$? (124 = timeout, expected)"

log ""
log "serial-wsl.log lines: $(wc -l < "$SERIAL" 2>/dev/null || echo 0)"
log "=== done $(date) ==="
