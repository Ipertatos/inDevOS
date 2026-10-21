#!/usr/bin/env bash
# Build the standalone kernel repro programs (static musl).
#   tests/ostest/build_repro.sh -> tests/ostest/build/repro{alarm,grandchild,nanosleep,nonblock}.exe
# (exe names have no underscore: the QEMU type helper drops '_').
set -euo pipefail
cd "$(dirname "$0")/../.."
CC=${MUSL_GCC:-$(command -v musl-gcc || echo "$HOME/.local/musl/bin/musl-gcc")}
mkdir -p tests/ostest/build
for n in alarm grandchild nanosleep nonblock orphans mprotect vfork sigsuspend fdpoll execenv pipeio bigwrite ftrunc bbread dupcloexec; do
    "$CC" -static -O2 -Wall -Wno-unused-function -Wno-unused-result \
        tests/ostest/repro_$n.c -o tests/ostest/build/repro$n.exe
    echo "built tests/ostest/build/repro$n.exe"
done
