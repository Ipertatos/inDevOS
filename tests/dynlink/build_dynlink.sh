#!/usr/bin/env bash
# Build musl's shared libc (libc.so, which is also musl's dynamic loader) and the
# dynamically linked test programs for inDevOS PT_INTERP support.
#
#   tests/dynlink/build_dynlink.sh
#
# Output (tests/dynlink/build/, not committed; make_install_iso.sh installs it):
#   ld-musl-x86_64.so.1   musl libc.so  -> /lib/ld-musl-x86_64.so.1
#   libdltest.so          test library  -> /lib/libdltest.so
#   hello_dyn.exe         PIE  (ET_DYN + PT_INTERP)      -> /bin/hello_dyn.exe
#   hello_dyn_nopie.exe   non-PIE (ET_EXEC + PT_INTERP)  -> /bin/hello_dyn_nopie.exe
#   dlopen_test.exe       dlopen/dlsym/dlclose of libdltest.so
#   needed_test.exe       DT_NEEDED libdltest.so
#   glibc/                optional (GLIBC=0 disables; default: when the host has
#                         glibc): the host's ld-linux-x86-64.so.2 + libc.so.6,
#                         hello_glibc.exe / dlopen_glibc.exe / io_glibc.exe (host gcc) and copies
#                         of the host's /bin/true, /bin/echo, /bin/cat.
#                         make_install_iso.sh installs them as /lib64/ld-linux-x86-64.so.2,
#                         /lib/x86_64-linux-gnu/{libc.so.6,libdltest_glibc.so}, /bin/*_glibc.exe.
#
# musl is built from source (shared) into $MUSL_DYN_PREFIX (default
# $HOME/.local/musl-dyn) with syslibdir inside the prefix, so nothing is
# written to the host's /lib.  The programs request /lib/ld-musl-x86_64.so.1.
set -euo pipefail
cd "$(dirname "$0")/../.."

MUSL_VERSION=${MUSL_VERSION:-1.2.5}
P=${MUSL_DYN_PREFIX:-$HOME/.local/musl-dyn}
OUT=tests/dynlink/build
SRC=tests/dynlink
LDSO=/lib/ld-musl-x86_64.so.1

if [[ ! -f "$P/lib/libc.so" || ! -x "$P/bin/musl-gcc" ]]; then
    echo ">>> building shared musl $MUSL_VERSION into $P"
    work=$(mktemp -d)
    tgz=${MUSL_SRC_TARBALL:-}
    if [[ -z "$tgz" ]]; then
        tgz=$work/musl.tar.gz
        url="https://musl.libc.org/releases/musl-${MUSL_VERSION}.tar.gz"
        if command -v curl >/dev/null 2>&1; then curl -fsSL "$url" -o "$tgz"; else wget -q "$url" -O "$tgz"; fi
    fi
    tar -xzf "$tgz" -C "$work"
    (cd "$work/musl-${MUSL_VERSION}" &&
        ./configure --prefix="$P" --syslibdir="$P/syslib" --enable-shared >/dev/null &&
        make -j"$(nproc)" >/dev/null &&
        make install >/dev/null)
    rm -rf "$work"
fi
CC="$P/bin/musl-gcc"
[[ -x "$CC" && -f "$P/lib/libc.so" ]] || { echo "shared musl build failed" >&2; exit 1; }
echo ">>> using $CC (libc.so $(stat -c %s "$P/lib/libc.so") bytes)"

mkdir -p "$OUT"
CF=(-O2 -Wall)
DL=(-Wl,-dynamic-linker,"$LDSO")
cp -f "$P/lib/libc.so" "$OUT/ld-musl-x86_64.so.1"
strip --strip-unneeded "$OUT/ld-musl-x86_64.so.1" 2>/dev/null || true
"$CC" "${CF[@]}" -fPIC -shared -Wl,-soname,libdltest.so -o "$OUT/libdltest.so" "$SRC/libdltest.c"
"$CC" "${CF[@]}" -fPIE -pie "${DL[@]}" -o "$OUT/hello_dyn.exe" "$SRC/hello_dyn.c"
"$CC" "${CF[@]}" -fno-pie -no-pie "${DL[@]}" -o "$OUT/hello_dyn_nopie.exe" "$SRC/hello_dyn.c"
"$CC" "${CF[@]}" -fPIE -pie "${DL[@]}" -o "$OUT/dlopen_test.exe" "$SRC/dlopen_test.c"
"$CC" "${CF[@]}" -fPIE -pie "${DL[@]}" -L"$OUT" -o "$OUT/needed_test.exe" "$SRC/needed_test.c" -ldltest
for f in hello_dyn hello_dyn_nopie dlopen_test needed_test; do strip "$OUT/$f.exe"; done
strip --strip-unneeded "$OUT/libdltest.so"

# Every program must ask for /lib/ld-musl-x86_64.so.1.
for f in hello_dyn hello_dyn_nopie dlopen_test needed_test; do
    readelf -l "$OUT/$f.exe" | grep -q "Requesting program interpreter: $LDSO" ||
        { echo "$f.exe: wrong or missing PT_INTERP" >&2; exit 1; }
done
# Host smoke test (x86-64 Linux host): run through the loader as a command.
if [[ "$(uname -m)" == x86_64 ]]; then
    rc=0; "$OUT/ld-musl-x86_64.so.1" "$OUT/hello_dyn.exe" 3 >/dev/null || rc=$?
    echo "    host smoke: hello_dyn exit $rc (want 3)"
    (cd "$OUT" && LD_LIBRARY_PATH=. ./ld-musl-x86_64.so.1 ./dlopen_test.exe ./libdltest.so) | grep -q DLOPEN_OK &&
        echo "    host smoke: DLOPEN_OK" || echo "    host smoke: dlopen test did not pass on the host (not fatal)"
fi
# ---- optional glibc part: run unmodified glibc programs through glibc's ld.so ----
GLD=/lib64/ld-linux-x86-64.so.2; GLC=/lib/x86_64-linux-gnu/libc.so.6
G="$OUT/glibc"
rm -rf "$G"
if [[ "${GLIBC:-1}" != 0 && -e "$GLD" && -e "$GLC" ]] && command -v gcc >/dev/null 2>&1; then
    mkdir -p "$G"
    cp -L "$GLD" "$G/ld-linux-x86-64.so.2"
    cp -L "$GLC" "$G/libc.so.6"
    gcc "${CF[@]}" -o "$G/hello_glibc.exe" "$SRC/hello_dyn.c"
    gcc "${CF[@]}" -fPIC -shared -Wl,-soname,libdltest_glibc.so -o "$G/libdltest_glibc.so" "$SRC/libdltest.c"
    gcc "${CF[@]}" -o "$G/dlopen_glibc.exe" "$SRC/dlopen_test.c" -ldl
    gcc "${CF[@]}" -o "$G/io_glibc.exe" "$SRC/io_test.c"
    for b in true echo cat; do cp -L "/bin/$b" "$G/${b}_glibc.exe"; done
    strip "$G/hello_glibc.exe" "$G/dlopen_glibc.exe" "$G/io_glibc.exe"; strip --strip-unneeded "$G/libdltest_glibc.so"
    for b in true echo cat; do
        n=$(readelf -d "$G/${b}_glibc.exe" | grep -c NEEDED)
        [[ $n == 1 ]] || { echo "  /bin/$b needs more than libc.so.6: dropped"; rm -f "$G/${b}_glibc.exe"; }
    done
    echo "  glibc: $(ldd --version 2>/dev/null | head -1)"
    for f in "$G"/*; do printf '  glibc/%-16s %8d bytes  %s\n' "$(basename "$f")" "$(stat -c %s "$f")" "$(md5sum < "$f" | cut -c1-8)"; done
fi
for f in "$OUT"/*; do [[ -f "$f" ]] || continue; printf '  %-22s %8d bytes  %s\n' "$(basename "$f")" "$(stat -c %s "$f")" "$(md5sum < "$f" | cut -c1-8)"; done
echo "done"
