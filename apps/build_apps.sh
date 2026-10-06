#!/usr/bin/env bash
# Build every program in apps/ into a real ELF executable and install them into
# the FAT32 image (sata_disk.img), which is what the shell launches them from.
#
#   apps/build_apps.sh              build + install all
#   apps/build_apps.sh ls cat       build + install only those
#   apps/build_apps.sh --no-install build only
#
# Uses mtools (mcopy) so no sudo / loop mount is needed.
set -u
cd "$(dirname "$0")/.." || exit 1

OUT=apps/build
IMG=sata_disk.img
CC=${CC:-x86_64-linux-gnu-gcc}
INSTALL=1

ARGS=()
for a in "$@"; do
    case "$a" in
        --no-install) INSTALL=0 ;;
        *) ARGS+=("$a") ;;
    esac
done

mkdir -p "$OUT"

# crt0 provides _start -> main(argc, argv) -> exit() for apps that define main().
CRT=apps/crt0.c
# appio.c holds the single definition of the shared stdout buffer (the printf
# helpers in syscalls.h are inline, so the buffer cannot live in the header).
APPIO=apps/appio.c

BASE_FLAGS=(-nostdlib -static -fPIE -pie -ffreestanding -O2
            -fno-stack-protector -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0
            -I. -Iinclude -Wl,--entry=_start)

build_one() {
    local src="$1"
    local name
    name=$(basename "$src" .c)
    local out="$OUT/$name.exe"
    local flags=("${BASE_FLAGS[@]}")

    # Per-app extras (kept from build_userapp.sh).
    case "$name" in
        herb_engine_indevos)
            flags+=(-O3 -mstackrealign -fno-builtin-qsort -fno-builtin-sin
                    -fno-builtin-cos -fno-builtin-sqrt -fno-builtin-floor -lgcc)
            ;;
    esac

    # Apps providing their own _start (raw CRT) must not also link crt0.
    local extra=("$APPIO")
    if ! grep -qE 'void[[:space:]]+_start[[:space:]]*\(' "$src"; then
        extra+=("$CRT")
    fi

    if ! $CC "${flags[@]}" -o "$out" "${extra[@]}" "$src" 2> "$OUT/$name.log"; then
        printf '  %-24s FAILED (see %s)\n' "$name" "$OUT/$name.log"
        return 1
    fi
    local size
    size=$(stat -c %s "$out")
    printf '  %-24s ok  %8d bytes\n' "$name" "$size"
    return 0
}

echo "=== building programs ==="
if [ ${#ARGS[@]} -gt 0 ]; then
    SRCS=()
    for a in "${ARGS[@]}"; do SRCS+=("apps/$a.c"); done
else
    SRCS=(apps/*.c)
fi

built=0
failed=0
for src in "${SRCS[@]}"; do
    [ "$(basename "$src")" = "crt0.c" ] && continue
    [ "$(basename "$src")" = "appio.c" ] && continue
    [ -f "$src" ] || { printf '  %-24s MISSING\n' "$src"; failed=$((failed+1)); continue; }
    if build_one "$src"; then built=$((built+1)); else failed=$((failed+1)); fi
done
echo "=== $built built, $failed failed ==="

if [ "$INSTALL" = "1" ] && [ "$built" -gt 0 ]; then
    if [ ! -f "$IMG" ]; then
        echo "!!! $IMG not found - skipping install"
        exit 1
    fi
    echo "=== installing into $IMG ==="
    mmd -i "$IMG" ::/bin 2>/dev/null || true
    for exe in "$OUT"/*.exe; do
        [ -f "$exe" ] || continue
        name=$(basename "$exe")
        if mcopy -o -i "$IMG" "$exe" "::/bin/$name" 2>/dev/null; then
            printf '  installed /bin/%s\n' "$name"
        elif mcopy -o -i "$IMG" "$exe" "::/$name" 2>/dev/null; then
            printf '  installed /%s (no /bin)\n' "$name"
        else
            printf '  FAILED to install %s\n' "$name"
        fi
    done
fi

# Copy the freshly built binaries back into apps/ so the tree keeps the
# executables next to their sources (doom/ does the same with doom.exe).
for exe in "$OUT"/*.exe; do
    [ -f "$exe" ] && cp -f "$exe" "apps/$(basename "$exe")"
done
echo "done"
