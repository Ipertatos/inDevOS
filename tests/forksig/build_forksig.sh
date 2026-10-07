#!/usr/bin/env bash
set -euo pipefail
export PATH="/home/ipertatos/.local/musl/bin:/usr/bin:/bin:$PATH"
ROOT="$(cd "$(dirname "$0")" && pwd)"
OUT="$ROOT/build"
mkdir -p "$OUT" "$ROOT/src"
CC="${CC:-musl-gcc}"
CFLAGS="${CFLAGS:--static -O2 -Wall}"
shopt -s nullglob
for src in "$ROOT"/src/*.c; do
  base=$(basename "$src" .c)
  echo "CC $base"
  $CC $CFLAGS -o "$OUT/${base}.exe" "$src"
done
ls -la "$OUT"
echo "build_forksig done"
