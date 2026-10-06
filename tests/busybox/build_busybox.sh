#!/usr/bin/env bash
set -e
export PATH=/home/ipertatos/.local/musl/bin:/usr/bin:/bin
cd /home/ipertatos/src/busybox-1.36.1

# Provide linux/*.h for musl (no sudo): extract from ubuntu package into musl include
HDR=/home/ipertatos/.local/musl/include
if [ ! -f "$HDR/linux/kd.h" ]; then
  mkdir -p /tmp/linuxhdr && cd /tmp/linuxhdr
  apt-get download linux-libc-dev 2>/dev/null || true
  DEB=$(ls linux-libc-dev*.deb 2>/dev/null | head -1)
  if [ -n "$DEB" ]; then
    dpkg-deb -x "$DEB" ./extract
    mkdir -p "$HDR"
    cp -a extract/usr/include/linux "$HDR/"
    cp -a extract/usr/include/asm-generic "$HDR/" 2>/dev/null || true
    # Prefer x86_64 asm
    if [ -d extract/usr/include/x86_64-linux-gnu/asm ]; then
      cp -a extract/usr/include/x86_64-linux-gnu/asm "$HDR/"
    elif [ -d extract/usr/include/asm ]; then
      cp -a extract/usr/include/asm "$HDR/"
    fi
    echo "Installed linux headers into $HDR"
  else
    echo "WARN: could not download linux-libc-dev; will disable console applets"
  fi
  cd /home/ipertatos/src/busybox-1.36.1
fi

# Disable applets that need kernel UAPI we may still lack, and networking
disable() {
  local o="$1"
  sed -i "s/^$o=y/# $o is not set/" .config || true
}
enable() {
  local o="$1"
  sed -i "s/^# $o is not set/$o=y/" .config || true
}

enable CONFIG_STATIC
enable CONFIG_FEATURE_SH_STANDALONE
enable CONFIG_FEATURE_SH_NOFORK

for o in CONFIG_KBD_MODE CONFIG_LOADFONT CONFIG_LOADKMAP CONFIG_OPENVT CONFIG_DEALLOCVT \
  CONFIG_CHVT CONFIG_SETCONSOLE CONFIG_SETFONT CONFIG_SETKEYCODES CONFIG_SHOWKEY \
  CONFIG_DUMPKMAP CONFIG_FGCONSOLE CONFIG_INIT CONFIG_LINUXRC CONFIG_FEATURE_USE_INITTAB \
  CONFIG_HALT CONFIG_POWEROFF CONFIG_REBOOT CONFIG_BOOTCHARTD CONFIG_PIVOT_ROOT \
  CONFIG_SWITCH_ROOT CONFIG_RUN_INIT CONFIG_CTTYHACK \
  CONFIG_HTTPD CONFIG_FTPD CONFIG_TELNET CONFIG_TELNETD CONFIG_NC CONFIG_WGET \
  CONFIG_TFTP CONFIG_TFTPD CONFIG_UDHCPC CONFIG_UDHCPD CONFIG_IFCONFIG CONFIG_IFUP \
  CONFIG_IFDOWN CONFIG_IP CONFIG_ROUTE CONFIG_PING CONFIG_PING6 CONFIG_TRACEROUTE \
  CONFIG_NSLOOKUP CONFIG_BRCTL CONFIG_ARP CONFIG_ARPING CONFIG_NAMEIF \
  CONFIG_FEATURE_MOUNT_NFS CONFIG_FEATURE_HAVE_RPC CONFIG_SSL_CLIENT \
  CONFIG_TC CONFIG_VLOCK CONFIG_WALL CONFIG_LOAD_POLICY; do
  disable "$o"
done

sed -i "/^[[:space:]]*=/d" .config
yes "" | make oldconfig >/dev/null 2>&1 || true
# Force STATIC again after oldconfig
enable CONFIG_STATIC
enable CONFIG_FEATURE_SH_STANDALONE
grep -E "^CONFIG_STATIC=|^CONFIG_ASH=|^CONFIG_VI=|^CONFIG_FEATURE_SH_STANDALONE=|^CONFIG_INIT " .config || true
grep CONFIG_INIT .config | head -3
ls "$HDR/linux/kd.h" "$HDR/linux/vt.h" 2>/dev/null || echo "headers missing (ok if applets disabled)"

LOG=/mnt/c/osproj/bb_build.log
echo "start $(date)" | tee "$LOG"
make -j$(nproc) CC=musl-gcc HOSTCC=gcc SKIP_STRIP=y >> "$LOG" 2>&1
EC=$?
echo "BUILD_EXIT=$EC" | tee -a "$LOG"
tail -40 "$LOG"
if [ $EC -eq 0 ] && [ -f busybox ]; then
  ls -la busybox | tee -a "$LOG"
  mkdir -p /mnt/c/osproj/tests/busybox/build
  cp -f busybox /mnt/c/osproj/tests/busybox/build/busybox.exe
  cp -f .config /mnt/c/osproj/tests/busybox/busybox.config
  ./busybox echo hello | tee -a "$LOG"
  ./busybox 2>&1 | head -30 | tee -a "$LOG"
fi
exit $EC
