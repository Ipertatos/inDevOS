#!/usr/bin/env bash
# Build the Linux-ABI test programs in tests/musl/ as STATIC musl x86-64
# binaries and (optionally) install them into the inDevOS disk images.
#
#   tests/musl/build_musl_tests.sh               build + install into sata_disk.img
#   tests/musl/build_musl_tests.sh --no-install  build only
#
# Output: tests/musl/build/hello_musl.exe, tests/musl/build/musltest.exe
# (make_install_iso.sh picks these up for os_install.iso /bin as well).
#
# Toolchain lookup order:
#   $MUSL_GCC, musl-gcc on PATH (apt install musl-tools),
#   $HOME/.local/musl/bin/musl-gcc.
# If none is found, musl is built from source into $HOME/.local/musl
# (no root needed; needs gcc, make and curl or wget).
set -euo pipefail
cd "$(dirname "$0")/../.."

MUSL_VERSION=${MUSL_VERSION:-1.2.5}
MUSL_PREFIX=${MUSL_PREFIX:-$HOME/.local/musl}
OUT=tests/musl/build
IMG=sata_disk.img
INSTALL=1
for a in "$@"; do
    case "$a" in
        --no-install) INSTALL=0 ;;
        *) echo "usage: $0 [--no-install]" >&2; exit 2 ;;
    esac
done

find_musl_gcc() {
    if [[ -n "${MUSL_GCC:-}" && -x "${MUSL_GCC}" ]]; then echo "$MUSL_GCC"; return 0; fi
    if command -v musl-gcc >/dev/null 2>&1; then command -v musl-gcc; return 0; fi
    if [[ -x "$MUSL_PREFIX/bin/musl-gcc" ]]; then echo "$MUSL_PREFIX/bin/musl-gcc"; return 0; fi
    return 1
}

build_musl_from_source() {
    echo ">>> musl-gcc not found; building musl $MUSL_VERSION into $MUSL_PREFIX"
    local work
    work=$(mktemp -d)
    local url="https://musl.libc.org/releases/musl-${MUSL_VERSION}.tar.gz"
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL "$url" -o "$work/musl.tar.gz"
    else
        wget -q "$url" -O "$work/musl.tar.gz"
    fi
    tar -xzf "$work/musl.tar.gz" -C "$work"
    (cd "$work/musl-${MUSL_VERSION}" &&
        ./configure --prefix="$MUSL_PREFIX" --disable-shared >/dev/null &&
        make -j"$(nproc)" >/dev/null &&
        make install >/dev/null)
    rm -rf "$work"
}

CC=$(find_musl_gcc || true)
if [[ -z "$CC" ]]; then
    build_musl_from_source
    CC=$(find_musl_gcc) || { echo "musl build failed" >&2; exit 1; }
fi
echo ">>> using $CC"

mkdir -p "$OUT"
# name-in-image : source
TARGETS=(
    "hello_musl:tests/musl/hello.c"
    "musltest:tests/musl/musltest.c"
)
for t in "${TARGETS[@]}"; do
    name=${t%%:*}; src=${t#*:}
    "$CC" -static -O2 -Wall -o "$OUT/$name.exe" "$src"
    strip "$OUT/$name.exe" 2>/dev/null || true
    printf '  %-16s %8d bytes\n' "$name.exe" "$(stat -c %s "$OUT/$name.exe")"
done

if [[ "$INSTALL" == "1" ]]; then
    if [[ -f "$IMG" ]] && command -v mcopy >/dev/null 2>&1; then
        # mmd prompts on stdin when the directory exists: check first.
        mdir -i "$IMG" ::/bin </dev/null >/dev/null 2>&1 || mmd -i "$IMG" ::/bin </dev/null
        for t in "${TARGETS[@]}"; do
            name=${t%%:*}
            mcopy -o -i "$IMG" "$OUT/$name.exe" "::/bin/$name.exe" </dev/null && echo "  installed /bin/$name.exe into $IMG"
        done
    else
        echo "  ($IMG or mtools missing: skipped install; make_install_iso.sh will still pick them up)"
    fi
fi
echo "done"
