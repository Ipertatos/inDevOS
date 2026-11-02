#!/usr/bin/env bash
# Stage TinyCC's own source for compiling tcc ON inDevOS (ostest tcc.selfhost):
#   tests/tcc/build_tcc_src.sh            # -> tests/tcc/src/ (gitignored)
# make_install_iso.sh installs tests/tcc/src as /usr/src/tcc; so does
# tests/ostest/stage_disk.sh for a private test disk.  On inDevOS:
#   cd /usr/src/tcc && tcc -DONE_SOURCE=1 -o /tmp/tcc2 tcc.c
# (no -DTCC_TARGET_*: config.h only applies its native settings when no
# target is given; with one, the result links glibc-style dynamic programs).
# The tree is the ONE_SOURCE file set of the checkout tests/tcc/build_tcc.sh
# configured (TCC_SRC, default ~/src/tinycc), including its config.h with the
# inDevOS paths (/usr/include, /usr/lib, /usr/lib/tcc, -static) and the
# generated tccdefs_.h, so the tcc built on inDevOS matches /bin/tcc.exe.
set -euo pipefail
cd "$(dirname "$0")/../.."
SRC=${TCC_SRC:-$HOME/src/tinycc}
OUT=tests/tcc/src
die() { echo "build_tcc_src: $*" >&2; exit 1; }
[[ -f $SRC/tcc.c && -f $SRC/config.h && -f $SRC/tccdefs_.h ]] || die "no configured TinyCC tree at $SRC (run tests/tcc/build_tcc.sh first)"
grep -q 'CONFIG_TCC_CRTPREFIX "/usr/lib"' "$SRC/config.h" || die "$SRC/config.h is not the inDevOS configuration (CONFIG_TCC_CRTPREFIX)"
FILES=(tcc.c libtcc.c tccpp.c tccgen.c tccdbg.c tccelf.c tccasm.c tccrun.c tcctools.c
       x86_64-gen.c x86_64-link.c i386-asm.c
       tcc.h libtcc.h tcctok.h elf.h stab.h stab.def dwarf.h i386-asm.h x86_64-asm.h i386-tok.h
       config.h tccdefs_.h tcclib.h VERSION COPYING)
rm -rf "$OUT"; mkdir -p "$OUT"
for f in "${FILES[@]}"; do
    [[ -f $SRC/$f ]] || die "missing $SRC/$f"
    cp "$SRC/$f" "$OUT/$f"
done
rev=$(git -C "$SRC" describe --always --dirty 2>/dev/null || echo unknown)
{
    echo "TinyCC $(cat "$SRC/VERSION") source ($rev) for building tcc on inDevOS:"
    echo "  tcc -DONE_SOURCE=1 -o /tmp/tcc2 tcc.c   (no -DTCC_TARGET_*: config.h applies only without one)"
    echo "Staged by tests/tcc/build_tcc_src.sh from $SRC."
} > "$OUT/README.txt"
# Sanity check on the host: the staged tree alone must compile (the ONE_SOURCE
# include set is complete) with the inDevOS tcc and its sysroot.
TS=""
[[ -f tests/tcc/stage.path ]] && TS=$(cat tests/tcc/stage.path)
if [[ -n $TS && -x $TS/bin/tcc.exe ]]; then
    tmp=$(mktemp -d)
    "$TS/bin/tcc.exe" -nostdinc -I"$TS/usr/include" -I"$TS/usr/lib/tcc/include" \
        -DONE_SOURCE=1 -c "$OUT/tcc.c" -o "$tmp/tcc.o" \
        || { rm -rf "$tmp"; die "staged source does not compile with $TS/bin/tcc.exe"; }
    echo "build_tcc_src: host check ok (tcc.o $(stat -c %s "$tmp/tcc.o") bytes)"
    rm -rf "$tmp"
fi
echo "build_tcc_src: $OUT ($(ls "$OUT" | wc -l) files, $(du -sk "$OUT" | cut -f1) KiB) from $SRC ($rev)"
