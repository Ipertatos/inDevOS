#!/usr/bin/env bash
# Workstream copy: build stage copy with tcc sources + static musl GNU make.
set -euo pipefail
export PATH="/home/ipertatos/.local/musl/bin:/usr/bin:/bin:$PATH"
WS=/mnt/c/Users/froze/ws_tcc
ORIG=/home/ipertatos/indev_tcc/stage
STAGE=/home/ipertatos/indev_tcc_ws/stage
SRC=/home/ipertatos/src/tinycc
W=/home/ipertatos/indev_tcc_ws/work
mkdir -p "$W"
if [ ! -d "$STAGE" ]; then mkdir -p "$(dirname "$STAGE")"; cp -a "$ORIG" "$STAGE"; fi
# --- tcc sources (configured as on-OS build) ---
D="$STAGE/usr/src/tcc"; rm -rf "$D"; mkdir -p "$D/include" "$D/lib"
cd "$SRC"
for f in tcc.c libtcc.c tccpp.c tccgen.c tccdbg.c tccelf.c tccasm.c tccrun.c tcctools.c \
         x86_64-gen.c x86_64-link.c i386-asm.c tcc.h libtcc.h tcctok.h elf.h stab.h stab.def \
         dwarf.h i386-asm.h x86_64-asm.h i386-tok.h config.h tccdefs_.h tcclib.h VERSION; do
  [ -f "$f" ] && cp "$f" "$D/"
done
# tccpe/macho/coff not needed for x86_64 ELF; tcc.h includes only by target
cp -a include/. "$D/include/"
cp -a lib/*.c lib/*.S "$D/lib/" 2>/dev/null || true
cat > "$D/build.sh" <<'S'
#!/bin/sh
# Self-host: build tcc with tcc on inDevOS
cd /usr/src/tcc
tcc -O2 -DONE_SOURCE=1 -o /tmp/tcc2 tcc.c && echo SELFHOST_BUILD_OK
S
cat > "$D/Makefile" <<'S'
# minimal Makefile for on-OS self-host test
CC = tcc
CFLAGS = -O2 -DONE_SOURCE=1
all: /tmp/tcc3
/tmp/tcc3: tcc.c
	$(CC) $(CFLAGS) -o $@ tcc.c
	@echo MAKE_SELFHOST_OK
S
# --- GNU make, static musl ---
if [ ! -x "$W/make-4.4.1/make" ]; then
  cd "$W"
  [ -f make-4.4.1.tar.gz ] || curl -sSLO https://ftp.gnu.org/gnu/make/make-4.4.1.tar.gz
  rm -rf make-4.4.1; tar xzf make-4.4.1.tar.gz; cd make-4.4.1
  CC=musl-gcc LDFLAGS=-static ./configure --disable-nls --without-guile --disable-load >/dev/null
  make -j"$(nproc)" >/dev/null
fi
file "$W/make-4.4.1/make"
cp -f "$W/make-4.4.1/make" "$STAGE/bin/make.exe"
mkdir -p "$STAGE/home/user"
cat > "$STAGE/home/user/mk/Makefile" 2>/dev/null || { mkdir -p "$STAGE/home/user/mk"; }
cat > "$STAGE/home/user/mk/Makefile" <<'S'
all: hi
	./hi
hi: hi.c
	tcc -o hi hi.c
S
printf '#include <stdio.h>\nint main(void){puts("MAKE_HELLO_OK");return 0;}\n' > "$STAGE/home/user/mk/hi.c"
printf '#include <stdio.h>\nint main(void){puts("TCC2_HELLO_OK");return 0;}\n' > "$STAGE/home/user/h2.c"
printf '%s\n' "$STAGE" > "$WS/tests/tcc/stage.path"
du -sh "$STAGE"
echo BUILD_SELFHOST_DONE
