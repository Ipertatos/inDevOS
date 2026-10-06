#!/usr/bin/env bash
# Build a static musl-linked TCC for inDevOS (x86_64 Linux ABI).
set -euo pipefail
export PATH="/home/ipertatos/.local/musl/bin:/usr/bin:/bin:$PATH"
ROOT="$(cd "$(dirname "$0")" && pwd)"
OSPROJ="$(cd "$ROOT/../.." && pwd)"
SRC_DIR="${TCC_SRC:-/home/ipertatos/src/tinycc}"
# Stage on Linux FS — /mnt/c is NTFS and collides on musl linux/*.h case variants.
BUILD="${TCC_BUILD:-/home/ipertatos/indev_tcc/build}"
STAGE="${TCC_STAGE:-/home/ipertatos/indev_tcc/stage}"
# Keep a convenience symlink/copy pointer under tests/tcc for install scripts
LINK_STAGE="$ROOT/stage"
LOG="$OSPROJ/tcc_build.log"
: > "$LOG"
log() { echo "$*" | tee -a "$LOG"; }

log "=== TCC build $(date) ==="
log "musl-gcc=$(command -v musl-gcc) gcc=$(command -v gcc)"

if [ ! -d "$SRC_DIR/.git" ] && [ ! -f "$SRC_DIR/configure" ]; then
  log "Cloning TinyCC mob..."
  mkdir -p "$(dirname "$SRC_DIR")"
  git clone --depth 1 https://github.com/TinyCC/tinycc.git "$SRC_DIR" >>"$LOG" 2>&1 \
    || git clone --depth 1 https://repo.or.cz/tinycc.git "$SRC_DIR" >>"$LOG" 2>&1
fi
cd "$SRC_DIR"
git fetch --depth 1 origin mob >>"$LOG" 2>&1 || true
git checkout mob >>"$LOG" 2>&1 || git checkout master >>"$LOG" 2>&1 || true
log "tcc rev: $(git rev-parse --short HEAD 2>/dev/null || echo unknown)"

make distclean >/dev/null 2>&1 || true
# Runtime paths on inDevOS. Link tcc with -static via musl-gcc LDFLAGS.
# Host helper c2str.exe MUST be built with system gcc (musl dynamic loader
# is not installed on the WSL host).
./configure \
  --cc=musl-gcc \
  --extra-cflags="-O2" \
  --extra-ldflags="-static" \
  --config-musl \
  --config-backtrace=no \
  --tcc-switches=-static \
  --crtprefix=/usr/lib \
  --libpaths=/usr/lib:/usr/lib/tcc \
  --tccdir=/usr/lib/tcc \
  --sysincludepaths=/usr/include:/usr/lib/tcc/include \
  --prefix=/usr \
  >>"$LOG" 2>&1 || true
log "Configured (see log for conftest noise)."

# Prebuild c2str with glibc gcc so make can generate tccdefs_.h
gcc -DC2STR conftest.c -o c2str.exe
./c2str.exe include/tccdefs.h tccdefs_.h
log "c2str/tccdefs ready"

make -j"$(nproc)" >>"$LOG" 2>&1 || log "WARN: make returned non-zero (often tcov/host-include); continuing"
[ -x ./tcc ] || { log "ERROR: tcc binary missing after make"; exit 1; }
log "Built. tcc size=$(stat -c%s tcc 2>/dev/null || echo 0)"
file tcc | tee -a "$LOG"
(ldd tcc 2>&1 || true) | tee -a "$LOG"
./tcc -v 2>&1 | tee -a "$LOG" || true

# Ensure libtcc1 exists (runtime helpers). Host /usr/include is glibc;
# point tcc at musl + its own include when finishing libtcc1.
if [ ! -f libtcc1.a ]; then
  make libtcc1.a >>"$LOG" 2>&1 || true
fi
if [ ! -f libtcc1.a ]; then
  log "Building libtcc1.a with explicit musl includes..."
  MUSL_INC=/home/ipertatos/.local/musl/include
  (
    cd lib
    for src in libtcc1.c stdatomic.c builtin.c dsohandle.c va_list.c runmain.c tcov.c; do
      [ -f "$src" ] || continue
      ../tcc -c "$src" -o "${src%.c}.o" -B.. -I.. -I../include -I"$MUSL_INC" >>"$LOG" 2>&1 || true
    done
    for src in atomic.S alloca.S alloca-bt.S; do
      [ -f "$src" ] || continue
      ../tcc -c "$src" -o "${src%.S}.o" -B.. -I.. >>"$LOG" 2>&1 || true
    done
    objs=$(ls libtcc1.o stdatomic.o atomic.o builtin.o alloca.o alloca-bt.o va_list.o dsohandle.o tcov.o 2>/dev/null || true)
    ../tcc -ar rcs ../libtcc1.a $objs >>"$LOG" 2>&1
  )
fi
[ -f libtcc1.a ] || { log "ERROR: no libtcc1.a"; exit 1; }
find . -name 'libtcc1.a' -print | tee -a "$LOG"

rm -rf "$BUILD" "$STAGE"
mkdir -p "$BUILD" "$STAGE"
cp -f tcc "$BUILD/tcc.exe"
if [ -f libtcc1.a ]; then
  cp -f libtcc1.a "$BUILD/libtcc1.a"
else
  find . -name 'libtcc1.a' -exec cp -f {} "$BUILD/libtcc1.a" \; -quit
fi
[ -f "$BUILD/libtcc1.a" ] || { log "ERROR: no libtcc1.a"; exit 1; }
mkdir -p "$BUILD/tcc_include"
cp -a include/. "$BUILD/tcc_include/" 2>/dev/null || true

# Stage on-OS sysroot
mkdir -p "$STAGE/bin" \
         "$STAGE/usr/lib/tcc/include" \
         "$STAGE/usr/include" \
         "$STAGE/usr/lib" \
         "$STAGE/tmp" \
         "$STAGE/home/user"
cp -f "$BUILD/tcc.exe" "$STAGE/bin/tcc.exe"
cp -f "$BUILD/libtcc1.a" "$STAGE/usr/lib/tcc/libtcc1.a"
cp -a "$BUILD/tcc_include/." "$STAGE/usr/lib/tcc/include/" 2>/dev/null || true

MUSL=/home/ipertatos/.local/musl
# Skip linux/ UAPI (case-colliding names; not needed for libc stdio/stdlib).
mkdir -p "$STAGE/usr/include"
if command -v rsync >/dev/null; then
  rsync -a --exclude 'linux' --exclude 'asm' --exclude 'asm-generic'     "$MUSL/include/" "$STAGE/usr/include/"
else
  cp -a "$MUSL/include/." "$STAGE/usr/include/"
  rm -rf "$STAGE/usr/include/linux" "$STAGE/usr/include/asm"          "$STAGE/usr/include/asm-generic" 2>/dev/null || true
fi
for f in crt1.o crti.o crtn.o Scrt1.o rcrt1.o libc.a libm.a; do
  if [ -f "$MUSL/lib/$f" ]; then
    cp -f "$MUSL/lib/$f" "$STAGE/usr/lib/$f"
  fi
done

cat > "$STAGE/home/user/hello.c" <<'C'
#include <stdio.h>
int main(void) {
    printf("hello from tcc on inDevOS\n");
    return 0;
}
C
cat > "$STAGE/home/user/work.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

struct pt { int x, y; char *label; };

static int sum_files(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    char buf[64];
    int n = (int)read(fd, buf, sizeof(buf)-1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = 0;
    return atoi(buf);
}

int main(void) {
    struct pt *p = malloc(sizeof(*p));
    if (!p) return 1;
    p->x = 3; p->y = 4;
    p->label = malloc(16);
    strcpy(p->label, "dot");
    int fd = open("/tmp/work.dat", O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if (fd < 0) { perror("open"); return 2; }
    dprintf(fd, "%d\n", p->x * p->y);
    close(fd);
    int v = sum_files("/tmp/work.dat");
    printf("work ok label=%s prod=%d file=%d\n", p->label, p->x*p->y, v);
    free(p->label);
    free(p);
    return (v == 12) ? 0 : 3;
}
C
cat > "$STAGE/home/user/a.c" <<'C'
int add(int a, int b) { return a + b; }
C
cat > "$STAGE/home/user/b.c" <<'C'
#include <stdio.h>
int add(int a, int b);
int main(void) {
    printf("multi=%d\n", add(40, 2));
    return 0;
}
C

# Host smoke with baked paths via -B/-I/-L against staged sysroot
set +e
"$BUILD/tcc.exe" -static \
  -I"$STAGE/usr/include" -I"$STAGE/usr/lib/tcc/include" \
  -B"$STAGE/usr/lib/tcc" \
  -L"$STAGE/usr/lib" \
  -o /tmp/tcc_hello_host "$STAGE/home/user/hello.c" 2>>"$LOG"
set -e
if [ -x /tmp/tcc_hello_host ]; then
  file /tmp/tcc_hello_host | tee -a "$LOG"
  (ldd /tmp/tcc_hello_host 2>&1 || true) | tee -a "$LOG"
  /tmp/tcc_hello_host | tee -a "$LOG"
else
  log "WARN: host hello compile failed (see log)"
fi

du -sh "$STAGE" "$STAGE/usr/include" "$STAGE/usr/lib" | tee -a "$LOG"
find "$STAGE" -type f | wc -l | tee -a "$LOG"
# Point tests/tcc/stage at Linux stage for install_tcc.sh
rm -rf "$LINK_STAGE"
ln -sfn "$STAGE" "$LINK_STAGE"
# Also write a small pointer file for Windows-side tooling
printf '%s
' "$STAGE" > "$ROOT/stage.path"
log "=== build done $(date) ==="
