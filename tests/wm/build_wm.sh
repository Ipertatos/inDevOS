#!/usr/bin/env bash
# Build libwm (static, musl) and the /dev/wm demo client.
#   tests/wm/build_wm.sh   -> userlib/libwm/build/libwm.a, tests/wm/build/wmdemo.exe
# make_install_iso.sh installs wmdemo.exe as /bin/wmdemo.exe when present.
set -euo pipefail
cd "$(dirname "$0")/../.."
CC=${MUSL_GCC:-$(command -v musl-gcc || echo "$HOME/.local/musl/bin/musl-gcc")}
mkdir -p userlib/libwm/build tests/wm/build
CFLAGS="-static -O2 -Wall -Wextra -Wno-unused-parameter -Iuserlib/libwm"
"$CC" $CFLAGS -c userlib/libwm/wm.c -o userlib/libwm/build/wm.o
ar rcs userlib/libwm/build/libwm.a userlib/libwm/build/wm.o
"$CC" $CFLAGS tests/wm/wmdemo.c -Luserlib/libwm/build -lwm -o tests/wm/build/wmdemo.exe
echo "built tests/wm/build/wmdemo.exe ($(stat -c %s tests/wm/build/wmdemo.exe) bytes)"
