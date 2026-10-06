#!/usr/bin/env bash
set -e
export PATH=/home/ipertatos/.local/musl/bin:/usr/bin:/bin
cd /home/ipertatos/src/busybox-1.36.1

# Install mtd headers from previously downloaded deb if available
HDR=/home/ipertatos/.local/musl/include
if [ ! -d "$HDR/mtd" ]; then
  mkdir -p /tmp/linuxhdr && cd /tmp/linuxhdr
  if [ ! -f linux-libc-dev*.deb ]; then apt-get download linux-libc-dev; fi
  DEB=$(ls linux-libc-dev*.deb | head -1)
  dpkg-deb -x "$DEB" ./extract
  cp -a extract/usr/include/mtd "$HDR/" || true
  cd /home/ipertatos/src/busybox-1.36.1
fi
ls "$HDR/mtd" || echo "no mtd dir"

# Nuke every UBI/MTD/flash applet from .config
grep -E 'CONFIG_(UBI|NAND|MTD|FLASH|UBIFS)' .config | head -40 || true
sed -i -E 's/^(CONFIG_(UBI|NAND|MTD|FLASH)[A-Z0-9_]*)=y/# \1 is not set/' .config
# Also disable strings? keep strings. Disable setfattr already done.
# Force-disable by name
for o in $(grep -E '^CONFIG_(UBI|NAND|FLASH|MTD)' .config | sed 's/=.*//' || true); do
  sed -i "s/^${o}=y/# ${o} is not set/" .config
done
grep UBI .config | head -20 || true

yes "" | make oldconfig >/dev/null 2>&1 || true
sed -i "s/# CONFIG_STATIC is not set/CONFIG_STATIC=y/" .config
sed -i "s/# CONFIG_FEATURE_SH_STANDALONE is not set/CONFIG_FEATURE_SH_STANDALONE=y/" .config

rm -rf miscutils/*.o miscutils/built-in.o miscutils/lib.a
LOG=/mnt/c/osproj/bb_build.log
OUT=/mnt/c/osproj/bb_nohup.out
echo "REBUILD3 $(date)" | tee "$OUT"
make -j$(nproc) CC=musl-gcc HOSTCC=gcc SKIP_STRIP=y > "$LOG" 2>&1
EC=$?
echo "BUILD_EXIT=$EC" | tee -a "$OUT"
if [ $EC -eq 0 ] && [ -f busybox ]; then
  ls -la busybox | tee -a "$OUT"
  mkdir -p /mnt/c/osproj/tests/busybox/build
  cp -f busybox /mnt/c/osproj/tests/busybox/build/busybox.exe
  cp -f .config /mnt/c/osproj/tests/busybox/busybox.config
  ./busybox echo hello_bb | tee -a "$OUT"
  ./busybox 2>&1 | head -35 | tee -a "$OUT"
else
  echo FAILED | tee -a "$OUT"
  grep -iE "fatal error|error:" "$LOG" | sort -u | head -40 | tee -a "$OUT"
fi
exit $EC
