#!/usr/bin/env bash
# Install busybox.exe into sata_disk.img /bin (and optionally root).
set -e
cd "$(dirname "$0")/../.."
IMG=${IMG:-sata_disk.img}
BB=tests/busybox/build/busybox.exe
if [[ ! -f "$BB" ]]; then
  echo "missing $BB — build with tests/busybox/build_busybox.sh first" >&2
  exit 1
fi
if ! command -v mcopy >/dev/null; then
  echo "mtools (mcopy) required" >&2
  exit 1
fi
if ! mdir -i "$IMG" ::/bin >/dev/null 2>&1; then
  mmd -i "$IMG" ::/bin </dev/null || true
fi
mcopy -o -i "$IMG" "$BB" ::/bin/busybox.exe
mcopy -o -i "$IMG" "$BB" ::/busybox.exe
echo "installed $(stat -c%s "$BB") bytes -> $IMG:/bin/busybox.exe"
mdir -i "$IMG" ::/bin | grep -i busy || true
