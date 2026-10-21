#!/usr/bin/env bash
# Build the OS test suite (static musl, links userlib/libwm for the /dev/wm test).
#   tests/ostest/build_ostest.sh  -> tests/ostest/build/ostest.exe
# make_install_iso.sh installs it as /bin/ostest.exe.
set -euo pipefail
cd "$(dirname "$0")/../.."
CC=${MUSL_GCC:-$(command -v musl-gcc || echo "$HOME/.local/musl/bin/musl-gcc")}
mkdir -p tests/ostest/build
[[ -f userlib/libwm/build/libwm.a ]] || bash tests/wm/build_wm.sh
"$CC" -static -O2 -Wall -Wextra -Wno-unused-parameter -Wno-unused-result -Wno-format-truncation \
    -Iuserlib/libwm tests/ostest/ostest.c -Luserlib/libwm/build -lwm -o tests/ostest/build/ostest.exe
echo "built tests/ostest/build/ostest.exe ($(stat -c %s tests/ostest/build/ostest.exe) bytes)"
