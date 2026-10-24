#!/usr/bin/env bash
# Build the static musl x86-64 NetSurf 3.11 framebuffer browser and /bin/dhcp for inDevOS.
#
# Installs into tests/netsurf/ (the parent of this directory):
#   netsurf   static, stripped NetSurf (draws through /dev/wm)  -> /bin/netsurf
#   res/      runtime resources (internal font, so fonts/ is dropped) -> /usr/share/netsurf
#   dhcp      static musl build of dhcp.c                          -> /bin/dhcp
# make_install_iso.sh then puts them into the install rootfs.
#
# Host prerequisites (Debian/Ubuntu): musl-tools musl-dev make gcc flex bison gperf
#   pkg-config zlib1g-dev libpng-dev perl python3 curl xz-utils linux-libc-dev
# Work tree (sources, sysroot, logs; ~1.5 GB): $NETSURF_WORK, default ~/netsurf-build.
# Tarballs are downloaded there on first run and checked against SHA-256 below.
# Env: J=<jobs> (default nproc), NETSURF_WORK=<dir>, ONLY_DHCP=1 (just rebuild dhcp).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$(cd "$HERE/.." && pwd)                       # tests/netsurf
NS=${NETSURF_WORK:-$HOME/netsurf-build}
SRC=$NS/src; SR=$NS/sysroot; J=${J:-$(nproc)}
step() { echo "=== $1 $(date +%T)"; }

step dhcp
musl-gcc -static -Os -s -o "$OUT/dhcp" "$OUT/dhcp.c"
[ "${ONLY_DHCP:-0}" = 1 ] && { ls -l "$OUT/dhcp"; exit 0; }

mkdir -p "$SRC" "$SR/lib" "$SR/include" "$NS/kh"
fetch() {   # fetch <file> <sha256> <url>
  [ -f "$SRC/$1" ] || { step "download $1"; curl -fL --retry 3 -o "$SRC/$1.part" "$3"; mv "$SRC/$1.part" "$SRC/$1"; }
  echo "$2  $SRC/$1" | sha256sum -c --quiet
}
unpack() {  # unpack <file> <dir>
  [ -d "$SRC/$2" ] || tar xf "$SRC/$1" -C "$SRC"
}
fetch openssl-3.0.15.tar.gz 23c666d0edf20f14249b3d8f0368acaee9ab585b09e1de82107c66e1f3ec9533 https://github.com/openssl/openssl/releases/download/openssl-3.0.15/openssl-3.0.15.tar.gz
fetch zlib-1.3.1.tar.gz     9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23 https://zlib.net/fossils/zlib-1.3.1.tar.gz
fetch libpng-1.6.44.tar.xz  60c4da1d5b7f0aa8d158da48e8f8afa9773c1c8baa5d21974df61f1886b8ce8e https://download.sourceforge.net/libpng/libpng-1.6.44.tar.xz
fetch jpegsrc.v9f.tar.gz    04705c110cb2469caa79fb71fba3d7bf834914706e9641a4589485c1f832565b https://www.ijg.org/files/jpegsrc.v9f.tar.gz
fetch expat-2.6.3.tar.xz    274db254a6979bde5aad404763a704956940e465843f2a9bd9ed7af22e2c0efc https://github.com/libexpat/libexpat/releases/download/R_2_6_3/expat-2.6.3.tar.xz
fetch curl-8.10.1.tar.xz    73a4b0e99596a09fa5924a4fb7e4b995a85fda0d18a2c02ab9cf134bebce04ee https://curl.se/download/curl-8.10.1.tar.xz
fetch netsurf-all-3.11.tar.gz 4dea880ff3c2f698bfd62c982b259340f9abcd7f67e6c8eb2b32c61f71644b7b https://download.netsurf-browser.org/netsurf/releases/source-full/netsurf-all-3.11.tar.gz
unpack openssl-3.0.15.tar.gz openssl-3.0.15; unpack zlib-1.3.1.tar.gz zlib-1.3.1; unpack libpng-1.6.44.tar.xz libpng-1.6.44
unpack jpegsrc.v9f.tar.gz jpeg-9f; unpack expat-2.6.3.tar.xz expat-2.6.3; unpack curl-8.10.1.tar.xz curl-8.10.1
unpack netsurf-all-3.11.tar.gz netsurf-all-3.11

cd "$SRC"
# musl has no kernel headers: expose the host's linux/asm headers after musl's own
ln -sfn /usr/include/linux "$NS/kh/linux"; ln -sfn /usr/include/asm-generic "$NS/kh/asm-generic"
ln -sfn /usr/include/x86_64-linux-gnu/asm "$NS/kh/asm"
export CC=musl-gcc
export CFLAGS="-O2 -idirafter $NS/kh"
export CPPFLAGS="-I$SR/include"
export LDFLAGS="-static -L$SR/lib"
export PKG_CONFIG_LIBDIR=$SR/lib/pkgconfig PKG_CONFIG_PATH= PKG_CONFIG="pkg-config --static"

[ -f "$SR/lib/libssl.a" ] || { step openssl; (cd openssl-3.0.15 && ./Configure linux-x86_64 no-shared no-tests no-afalgeng no-async no-engine no-dso -static \
   -idirafter "$NS/kh" --prefix="$SR" --libdir=lib --openssldir=/etc/ssl && make -j$J build_libs && make install_dev) >"$NS/log_openssl.txt" 2>&1; }
[ -f "$SR/lib/libz.a" ] || { step zlib; (cd zlib-1.3.1 && ./configure --static --prefix="$SR" && make -j$J && make install) >"$NS/log_zlib.txt" 2>&1; }
[ -f "$SR/lib/libpng.a" ] || { step libpng; (cd libpng-1.6.44 && ./configure --prefix="$SR" --disable-shared --enable-static && make -j$J && make install) >"$NS/log_png.txt" 2>&1; }
[ -f "$SR/lib/libjpeg.a" ] || { step jpeg; (cd jpeg-9f && ./configure --prefix="$SR" --disable-shared --enable-static && make -j$J && make install) >"$NS/log_jpeg.txt" 2>&1; }
[ -f "$SR/lib/libexpat.a" ] || { step expat; (cd expat-2.6.3 && ./configure --prefix="$SR" --disable-shared --enable-static --without-docbook --without-examples --without-tests && make -j$J && make install) >"$NS/log_expat.txt" 2>&1; }
[ -f "$SR/lib/libcurl.a" ] || { step curl; (cd curl-8.10.1 && LIBS="-lssl -lcrypto -lz -pthread" ./configure --prefix="$SR" --disable-shared --enable-static \
   --with-openssl="$SR" --with-zlib="$SR" --without-libpsl --without-brotli --without-zstd --without-nghttp2 --without-libidn2 --without-librtmp \
   --without-libssh2 --disable-ldap --disable-ldaps --disable-rtsp --disable-dict --disable-telnet --disable-tftp --disable-pop3 --disable-imap \
   --disable-smtp --disable-gopher --disable-mqtt --disable-smb --disable-manual --disable-docs --disable-ntlm --enable-ipv6 \
   --with-ca-bundle=/etc/ssl/cert.pem && make -j$J && make install) >"$NS/log_curl.txt" 2>&1; }

step netsurf
bash "$HERE/patches/apply_wm_surface.sh" "$SRC/netsurf-all-3.11/libnsfb"
export CFLAGS="$CFLAGS $CPPFLAGS"   # env (not make args) so component CFLAGS still append
cd "$SRC/netsurf-all-3.11"
# top Makefile's build-stamp otherwise skips everything; force libnsfb + gui to pick up patches
rm -f inst-framebuffer/build-stamp netsurf/build/Linux-framebuffer/frontends_framebuffer_gui.o
# host tools (nsgenbind, split-messages, font converter) use the host gcc (zlib1g-dev, libpng-dev)
make -j$J TARGET=framebuffer CC=musl-gcc HOST_CC=gcc BUILD_CC=gcc \
  NETSURF_FB_FRONTEND=linux NETSURF_FB_FONTLIB=internal \
  NETSURF_FB_RESPATH='$${HOME}/.netsurf/:$${NETSURFRES}:/etc/netsurf/:/cdrom/etc/netsurf/:/usr/share/netsurf/:/cdrom/usr/share/netsurf/:./frontends/framebuffer/res' \
  NETSURF_USE_DUKTAPE=YES NETSURF_USE_WEBP=NO NETSURF_USE_VIDEO=NO NETSURF_USE_HARU_PDF=NO \
  >"$NS/log_netsurf.txt" 2>&1 || { tail -40 "$NS/log_netsurf.txt"; exit 1; }
B=$(find . -maxdepth 2 -name nsfb -type f | head -1)
strip -o "$OUT/netsurf" "$B"
rm -rf "$OUT/res.new"; cp -rL netsurf/frontends/framebuffer/res "$OUT/res.new"; rm -rf "$OUT/res.new/fonts"
rm -rf "$OUT/res"; mv "$OUT/res.new" "$OUT/res"
ls -l "$OUT/netsurf" "$OUT/dhcp"; echo "res: $(du -sh "$OUT/res" | cut -f1)"
