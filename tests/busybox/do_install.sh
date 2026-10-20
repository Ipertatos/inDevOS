#!/usr/bin/env bash
OSPROJ="${OSPROJ:-$(cd "$(dirname "$0")/../.." && pwd)}"; export OSPROJ  # repo root
set -e
cd ${OSPROJ}
IMG=sata_disk.img
BB=tests/busybox/build/busybox.exe
ls -la "$BB" "$IMG"
mdir -i "$IMG" ::/bin >/dev/null 2>&1 || mmd -i "$IMG" ::/bin </dev/null || true
mcopy -o -i "$IMG" "$BB" ::/bin/busybox.exe
mcopy -o -i "$IMG" "$BB" ::/busybox.exe
mdir -i "$IMG" ::/bin | grep -i busy
echo OK
