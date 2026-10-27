#!/usr/bin/env bash
# Static musl GNU make for inDevOS (/bin/make.exe).  Build host: Linux/WSL
# with musl-gcc (Debian/Ubuntu: musl-tools), curl, make.
#
#   tests/make/build_make.sh [WORKDIR]
#
# Output: tests/make/build/bin/make (ignored by git); make_install_iso.sh
# copies it to /bin/make.exe when present (otherwise it falls back to the
# make.exe in the TinyCC stage, see tests/tcc/build_selfhost.sh).
# No guile, no load/dlopen, no NLS: the result is a single static binary.
set -euo pipefail
V=4.4.1
SHA256=dd16fb1d67bfab79a72f5e8390735c49e3e8e70b4945a15ab1f81ddb78658fb3
HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="${1:-$HERE/build/work}"
OUT="$HERE/build/bin"
JOBS="${JOBS:-$(nproc)}"
export PATH="${HOME}/.local/musl/bin:$PATH"
command -v musl-gcc >/dev/null || { echo "musl-gcc not found (install musl-tools)" >&2; exit 1; }
mkdir -p "$WORK" "$OUT"
cd "$WORK"
if [ ! -f make-$V.tar.gz ]; then
    curl -fsSLO https://ftp.gnu.org/gnu/make/make-$V.tar.gz
fi
echo "$SHA256  make-$V.tar.gz" | sha256sum -c -
[ -d make-$V ] || tar xzf make-$V.tar.gz
cd make-$V
if [ ! -f Makefile ]; then
    CC=musl-gcc CFLAGS="-O2" LDFLAGS="-static -s" \
        ./configure --disable-nls --without-guile --disable-load -q
fi
make -j"$JOBS" MAKEINFO=true
install -m 755 make "$OUT/make"
if readelf -l "$OUT/make" | grep -q "program interpreter"; then
    echo "$OUT/make is not static" >&2; exit 1
fi
"$OUT/make" --version | head -1
command -v file >/dev/null && file "$OUT/make" | sed "s|$OUT/||"
ls -l "$OUT/make"
