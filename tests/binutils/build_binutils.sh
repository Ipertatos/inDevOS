#!/usr/bin/env bash
# Static musl GNU binutils (as, ld, ar, ranlib, nm, objdump, strip, readelf,
# objcopy, size, strings) for inDevOS.  Build host: Linux/WSL with musl-gcc
# (Debian/Ubuntu: musl-tools), curl, xz, make.
#
#   tests/binutils/build_binutils.sh [WORKDIR]
#
# Output: tests/binutils/build/bin/<tool> (ignored by git); make_install_iso.sh
# copies them to /bin when present.  The tools are configured with
# --prefix=/usr, so ld searches /usr/lib (musl crt1.o, libc.a from the TinyCC
# sysroot), /lib and /usr/local/lib by default.
set -euo pipefail
V=2.43.1
SHA256=13f74202a3c4c51118b797a39ea4200d3f6cfbe224da6d1d95bb938480132dfd
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="${1:-$HERE/build/work}"
OUT="$HERE/build/bin"
JOBS="${JOBS:-$(nproc)}"
command -v musl-gcc >/dev/null || { echo "musl-gcc not found (install musl-tools)" >&2; exit 1; }
mkdir -p "$WORK" "$OUT"
cd "$WORK"
if [ ! -f binutils-$V.tar.xz ]; then
    curl -fsSLO https://ftp.gnu.org/gnu/binutils/binutils-$V.tar.xz
fi
echo "$SHA256  binutils-$V.tar.xz" | sha256sum -c -
if [ ! -d binutils-$V ]; then
    tar xf binutils-$V.tar.xz
    for p in "$HERE"/patches/*.patch; do
        [ -f "$p" ] || continue
        echo "applying $(basename "$p")"
        patch -d binutils-$V -p1 < "$p"
    done
fi
mkdir -p obj && cd obj
if [ ! -f Makefile ]; then
    # host tools for the musl "cross" build are the build machine's own
    CC="musl-gcc -static" CFLAGS="-O2" LDFLAGS="-static -s" AR=ar RANLIB=ranlib NM=nm \
    ../binutils-$V/configure \
        --build=x86_64-pc-linux-gnu --host=x86_64-linux-musl --target=x86_64-linux-musl \
        --prefix=/usr --disable-nls --disable-werror --disable-gdb --disable-gdbserver \
        --disable-gprof --disable-gprofng --disable-plugins --disable-lto \
        --disable-shared --enable-static --disable-sim --disable-libctf \
        --disable-multilib --without-zstd --without-system-zlib --without-debuginfod \
        --disable-gold --enable-deterministic-archives
fi
# makeinfo is not needed (and usually absent): skip the info manuals.
# libtool drops a plain -static at link time (the result would want
# /lib/ld-musl-x86_64.so.1); -all-static gives fully static programs.
make -j"$JOBS" MAKEINFO=true LDFLAGS="-all-static -s" all-binutils all-gas all-ld
for t in ar ranlib objdump objcopy readelf size strings; do
    install -m 755 binutils/$t "$OUT/$t"
done
install -m 755 binutils/nm-new    "$OUT/nm"
install -m 755 binutils/strip-new "$OUT/strip"
install -m 755 gas/as-new         "$OUT/as"
install -m 755 ld/ld-new          "$OUT/ld"
for t in "$OUT"/*; do
    if readelf -l "$t" | grep -q "program interpreter"; then
        echo "$t is not static" >&2; exit 1
    fi
done
command -v file >/dev/null && file "$OUT"/* | sed "s|$OUT/||"
ls -l "$OUT"
