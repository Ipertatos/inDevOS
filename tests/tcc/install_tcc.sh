#!/usr/bin/env bash
# Copy staged TCC + musl sysroot onto sata_disk.img
# Work on a Linux-local copy of the image — mtools against /mnt/c is prone to hangs.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
OSPROJ="$(cd "$ROOT/../.." && pwd)"
cd "$OSPROJ"
IMG_WIN="${IMG:-sata_disk.img}"
STAGE="$ROOT/stage"
if [ -f "$ROOT/stage.path" ]; then STAGE=$(cat "$ROOT/stage.path"); fi
if [ -L "$ROOT/stage" ]; then STAGE=$(readlink -f "$ROOT/stage"); fi
if [ ! -x "$STAGE/bin/tcc.exe" ]; then
  echo "missing $STAGE/bin/tcc.exe — run build_tcc.sh first (STAGE=$STAGE)" >&2
  exit 1
fi
command -v mcopy >/dev/null
command -v mmd >/dev/null

WORK=/tmp/indev_tcc_img
mkdir -p "$WORK"
echo "Copying $IMG_WIN -> $WORK/sata_disk.img (local)..."
cp -f "$IMG_WIN" "$WORK/sata_disk.img"
IMG="$WORK/sata_disk.img"
echo "installing from STAGE=$STAGE into local $IMG"

ensure_dir() {
  local d="$1"
  if mdir -i "$IMG" "::/$d" >/dev/null 2>&1; then return 0; fi
  local cur="" part
  local oldifs=$IFS
  IFS=/
  for part in $d; do
    if [ -z "$cur" ]; then cur="$part"; else cur="$cur/$part"; fi
    if ! mdir -i "$IMG" "::/$cur" >/dev/null 2>&1; then
      mmd -i "$IMG" "::/$cur" 2>/dev/null || true
    fi
  done
  IFS=$oldifs
}

for d in bin usr usr/lib usr/lib/tcc usr/lib/tcc/include usr/include tmp home home/user; do
  ensure_dir "$d"
done

echo "copying tcc.exe..."
mcopy -o -i "$IMG" "$STAGE/bin/tcc.exe" ::/bin/tcc.exe

echo "copying /usr/lib..."
for f in "$STAGE"/usr/lib/*; do
  [ -f "$f" ] || continue
  mcopy -o -i "$IMG" "$f" "::/usr/lib/$(basename "$f")"
done

echo "copying /usr/lib/tcc..."
mcopy -o -i "$IMG" "$STAGE/usr/lib/tcc/libtcc1.a" ::/usr/lib/tcc/libtcc1.a
if [ -d "$STAGE/usr/lib/tcc/include" ]; then
  find "$STAGE/usr/lib/tcc/include" -type d -printf '%P\n' | while read -r rel; do
    [ -z "$rel" ] && continue
    ensure_dir "usr/lib/tcc/include/$rel"
  done
  find "$STAGE/usr/lib/tcc/include" -type f -printf '%P\n' | while read -r rel; do
    mcopy -o -i "$IMG" "$STAGE/usr/lib/tcc/include/$rel" "::/usr/lib/tcc/include/$rel"
  done
fi

echo "copying /usr/include..."
find "$STAGE/usr/include" -type d -printf '%P\n' | while read -r rel; do
  [ -z "$rel" ] && continue
  ensure_dir "usr/include/$rel"
done
count=0
while IFS= read -r rel; do
  mcopy -o -i "$IMG" "$STAGE/usr/include/$rel" "::/usr/include/$rel"
  count=$((count+1))
  if (( count % 50 == 0 )); then echo "  ... $count headers"; fi
done < <(find "$STAGE/usr/include" -type f -printf '%P\n')
echo "  ... $count headers done"

if ls "$STAGE"/home/user/*.c >/dev/null 2>&1; then
  for f in "$STAGE"/home/user/*.c; do
    mcopy -o -i "$IMG" "$f" "::/home/user/$(basename "$f")"
  done
fi

echo "=== verify local image ==="
mdir -i "$IMG" ::/bin | grep -i tcc || true
mdir -i "$IMG" ::/usr/lib | head -20
mdir -i "$IMG" ::/ 2>&1 | tail -5

echo "Writing back $WORK/sata_disk.img -> $OSPROJ/$IMG_WIN ..."
cp -f "$WORK/sata_disk.img" "$OSPROJ/$IMG_WIN"
echo "install done"
du -sh "$STAGE"
