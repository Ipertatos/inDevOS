#!/usr/bin/env bash
# Copy the optional programs ostest exercises onto a FAT disk image (e.g. a
# private copy of the dev disk for a full ostest run booted from os.iso), at the
# paths make_install_iso.sh uses in the install image:
#   tests/dynlink/build (build_dynlink.sh)  -> /lib/ld-musl-x86_64.so.1, /lib/libdltest.so,
#       /bin/{hello_dyn,hello_dyn_nopie,dlopen_test,needed_test}.exe      (ostest dyn.*)
#   tests/dynlink/build/glibc               -> /lib64/ld-linux-x86-64.so.2,
#       /lib/x86_64-linux-gnu/{libc.so.6,libdltest_glibc.so}, /bin/*_glibc.exe (dyn.glibc_*)
#   tests/sdl2/build/sdl2demo.exe           -> /bin/sdl2demo.exe            (sdl2.*)
#   tests/tcc/src (build_tcc_src.sh)        -> /usr/src/tcc                  (tcc.selfhost)
# Missing build outputs are reported and left out (ostest then SKIPs those tests).
#   tests/ostest/stage_disk.sh IMG
set -uo pipefail
IMG=$(readlink -f "${1:?usage: stage_disk.sh IMG}")
[[ -f $IMG ]] || { echo "stage_disk: no image $IMG" >&2; exit 1; }
cd "$(dirname "$0")/../.."
nf=0
put() {
    mcopy -o -i "$IMG" "$1" "::$2" </dev/null || { echo "stage_disk: copy $1 -> $2 FAILED" >&2; exit 1; }
    nf=$((nf + 1))
}
mkdirs() { for d in "$@"; do mmd -D s -i "$IMG" "::$d" </dev/null 2>/dev/null || true; done; }
DYN=tests/dynlink/build
if [[ -f $DYN/ld-musl-x86_64.so.1 ]]; then
    mkdirs /lib /bin
    put "$DYN/ld-musl-x86_64.so.1" /lib/ld-musl-x86_64.so.1
    [[ -f $DYN/libdltest.so ]] && put "$DYN/libdltest.so" /lib/libdltest.so
    for t in hello_dyn hello_dyn_nopie dlopen_test needed_test; do
        [[ -f $DYN/$t.exe ]] && put "$DYN/$t.exe" "/bin/$t.exe"
    done
    echo "stage_disk: dynlink (musl) staged"
else
    echo "stage_disk: no $DYN (tests/dynlink/build_dynlink.sh): dyn.* will skip"
fi
if [[ -f $DYN/glibc/ld-linux-x86-64.so.2 && -f $DYN/glibc/libc.so.6 ]]; then
    mkdirs /lib /lib64 /lib/x86_64-linux-gnu /bin
    put "$DYN/glibc/ld-linux-x86-64.so.2" /lib64/ld-linux-x86-64.so.2
    put "$DYN/glibc/libc.so.6" /lib/x86_64-linux-gnu/libc.so.6
    [[ -f $DYN/glibc/libdltest_glibc.so ]] && put "$DYN/glibc/libdltest_glibc.so" /lib/x86_64-linux-gnu/libdltest_glibc.so
    for f in "$DYN"/glibc/*.exe; do [[ -f $f ]] && put "$f" "/bin/$(basename "$f")"; done
    echo "stage_disk: dynlink (glibc) staged"
else
    echo "stage_disk: no glibc runtime in $DYN/glibc: dyn.glibc_* will skip"
fi
if [[ -f tests/sdl2/build/sdl2demo.exe ]]; then
    mkdirs /bin; put tests/sdl2/build/sdl2demo.exe /bin/sdl2demo.exe
    echo "stage_disk: sdl2demo staged"
else
    echo "stage_disk: no tests/sdl2/build/sdl2demo.exe: sdl2.* will skip"
fi
if [[ -f tests/tcc/src/tcc.c ]]; then
    mkdirs /usr /usr/src /usr/src/tcc
    for f in tests/tcc/src/*; do put "$f" "/usr/src/tcc/$(basename "$f")"; done
    echo "stage_disk: TinyCC source staged in /usr/src/tcc"
else
    echo "stage_disk: no tests/tcc/src (tests/tcc/build_tcc_src.sh): tcc.selfhost will skip"
fi
# mtools writes long names as VFAT LFN entries; check a few kept their names.
for f in /lib/ld-musl-x86_64.so.1 /bin/hello_dyn_nopie.exe /lib64/ld-linux-x86-64.so.2 /bin/sdl2demo.exe /usr/src/tcc/tccdefs_.h /usr/src/tcc/x86_64-link.c; do
    case $f in
        /lib/ld-musl*|/bin/hello_dyn*) [[ -f $DYN/ld-musl-x86_64.so.1 ]] || continue ;;
        /lib64/*) [[ -f $DYN/glibc/ld-linux-x86-64.so.2 ]] || continue ;;
        /bin/sdl2demo.exe) [[ -f tests/sdl2/build/sdl2demo.exe ]] || continue ;;
        /usr/src/*) [[ -f tests/tcc/src/tcc.c ]] || continue ;;
    esac
    mdir -i "$IMG" "::$f" >/dev/null 2>&1 </dev/null || { echo "stage_disk: $f missing after copy" >&2; exit 1; }
done
echo "stage_disk: $nf files copied to $IMG"
