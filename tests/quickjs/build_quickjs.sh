#!/usr/bin/env bash
# Static musl x86-64 QuickJS (quickjs-ng v0.17.0) for inDevOS.  No cmake needed: plain musl-gcc, -static.
# Box/WSL prerequisites (Debian): sudo apt-get install -y musl-tools musl-dev curl
# Usage: tests/quickjs/build_quickjs.sh [WORKDIR]     (default WORKDIR=$PWD/qjs_work)
#   TEST262=1 also fetches the pinned test262 checkout (for run-test262).
# Output: WORKDIR/dist/{lib/libquickjs.a, lib/libquickjs-libc.a, include/*.h, bin/qjs, bin/qjsc, bin/run-test262, bin/api-test}
#   libquickjs.a      = engine only (quickjs.c libregexp.c libunicode.c dtoa.c) -> what NetSurf links
#   libquickjs-libc.a = quickjs-libc.c (std/os modules, used by qjs/run-test262; optional for embedders)
# Binaries are NOT committed; only this script + patches/.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
W=${1:-$PWD/qjs_work}; J=${J:-8}
VER=0.17.0
URL=https://github.com/quickjs-ng/quickjs/archive/refs/tags/v$VER.tar.gz
SHA=559bc4c420475e55c7ab4510adbc562f55d7524d75e8e89d79ce4bb02f5687d9
T262=5ef1e5723be95296f36afb0386676fed0205869c   # submodule pin of v0.17.0
T262SHA=d67ff29d7bcee3493f38b3ef54db068ea5ad6d5a67b67b3f505dfee581dd3879
mkdir -p "$W"; cd "$W"
[ -f quickjs-ng-$VER.tar.gz ] || curl -fsSL -o quickjs-ng-$VER.tar.gz $URL
echo "$SHA  quickjs-ng-$VER.tar.gz" | sha256sum -c -
S=$W/quickjs-$VER
if [ ! -f $S/.patched ]; then
  rm -rf $S; tar xzf quickjs-ng-$VER.tar.gz
  for p in "$HERE"/patches/*.patch; do [ -e "$p" ] || continue; echo "patch $(basename $p)"; patch -d $S -p1 < "$p"; done
  touch $S/.patched
fi
if [ "${TEST262:-0}" = 1 ] && [ ! -f $S/test262/harness/assert.js ]; then
  [ -f test262-$T262.tar.gz ] || curl -fsSL -o test262-$T262.tar.gz https://github.com/tc39/test262/archive/$T262.tar.gz
  echo "$T262SHA  test262-$T262.tar.gz" | sha256sum -c -
  rm -rf $S/test262; mkdir -p $S/test262; tar xzf test262-$T262.tar.gz -C $S/test262 --strip-components=1
fi
CC=${CC:-musl-gcc}
CFLAGS="-O2 -g -std=gnu11 -D_GNU_SOURCE -DQUICKJS_NG_BUILD -fno-strict-aliasing -Wno-unused-parameter -Wno-sign-compare -Wno-implicit-fallthrough -I$S"
B=$W/build; rm -rf $B; mkdir -p $B
cd $S
for f in quickjs.c libregexp.c libunicode.c dtoa.c; do $CC $CFLAGS -c $f -o $B/${f%.c}.o & done; wait
$CC $CFLAGS -DQJS_BUILD_LIBC -c quickjs-libc.c -o $B/quickjs-libc.o
ar rcs $B/libquickjs.a $B/quickjs.o $B/libregexp.o $B/libunicode.o $B/dtoa.o
ar rcs $B/libquickjs-libc.a $B/quickjs-libc.o
L="$B/libquickjs-libc.a $B/libquickjs.a -lm -pthread"
$CC $CFLAGS -static -o $B/qjs qjs.c gen/repl.c gen/standalone.c $L
$CC $CFLAGS -static -o $B/qjsc qjsc.c $L
$CC $CFLAGS -static -o $B/run-test262 run-test262.c $L
$CC $CFLAGS -static -o $B/api-test api-test.c $L
D=$W/dist; rm -rf $D; mkdir -p $D/lib $D/include $D/bin
cp $B/libquickjs.a $B/libquickjs-libc.a $D/lib/
cp quickjs.h quickjs-libc.h $D/include/
for x in qjs qjsc run-test262 api-test; do cp $B/$x $D/bin/$x.unstripped; strip -o $D/bin/$x $B/$x; done
ls -la $D/lib $D/bin
$D/bin/qjs -e 'console.log("qjs", typeof Promise, [1,2,3].at(-1), Object.groupBy([1,2],x=>x%2?"o":"e").o)'
