#!/usr/bin/env bash
# Cross-build SDL2 (static, musl) with the native inDevOS /dev/wm video driver,
# then the SDL2 test programs.
#
#   tests/sdl2/build_sdl2.sh [all|sdl|demo|doom]      (default: all)
#
# Outputs (not committed):
#   ~/sdl2-build/sysroot/{include/SDL2,lib/libSDL2.a,lib/libSDL2_test.a,...}
#   tests/sdl2/build/sdl2demo.exe        SDL2 demo + --selftest (ostest sdl2.*)
#   tests/sdl2/build/chocolate-doom.exe  Chocolate Doom 3.1.0 (no mixer/net)
# make_install_iso.sh installs both into /bin when present.
#
# SDL2 config: video = indevos (+ dummy, offscreen), audio = dummy/disk,
# timers = unix clock_gettime, threads = pthreads, joystick/haptic = dummy.
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD
HERE=$ROOT/tests/sdl2
WHAT=${1:-all}
CC=${MUSL_GCC:-$(command -v musl-gcc || echo "$HOME/.local/musl/bin/musl-gcc")}
[[ -x $CC ]] || { echo "musl-gcc not found (set MUSL_GCC)"; exit 1; }
MUSL_LIBDIR=$(dirname "$("$CC" -print-file-name=libc.a)")
WORK=${SDL2_WORK:-$HOME/sdl2-build}
SYSROOT=$WORK/sysroot
JOBS=${JOBS:-$(nproc)}
GEN=$(command -v ninja >/dev/null && echo Ninja || echo "Unix Makefiles")

SDL_VER=2.30.9
SDL_URL=https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VER/SDL2-$SDL_VER.tar.gz
SDL_SHA=24b574f71c87a763f50704bbb630cbe38298d544a1f890f099a4696b1d6beba4
DOOM_VER=3.1.0
DOOM_URL=https://github.com/chocolate-doom/chocolate-doom/archive/refs/tags/chocolate-doom-$DOOM_VER.tar.gz
DOOM_SHA=f2c64843dcec312032b180c3b2f34b4cb26c4dcdaa7375a1601a3b1df11ef84d

mkdir -p "$WORK" "$HERE/build"
fetch() { # url sha file
    if [[ ! -f $WORK/$3 ]] || ! echo "$2  $WORK/$3" | sha256sum -c --quiet - 2>/dev/null; then
        curl -fL --retry 3 -o "$WORK/$3.part" "$1"
        mv "$WORK/$3.part" "$WORK/$3"
    fi
    echo "$2  $WORK/$3" | sha256sum -c --quiet -
}

# cmake toolchain: musl-gcc, static, never pick up host (glibc) libraries.
TC=$WORK/musl-toolchain.cmake
cat > "$TC" <<TCEOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER "$CC")
set(CMAKE_FIND_ROOT_PATH "$SYSROOT" "$(dirname "$MUSL_LIBDIR")")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
set(PKG_CONFIG_EXECUTABLE "/bin/false" CACHE FILEPATH "no host pkg-config")
TCEOF

build_sdl() {
    fetch "$SDL_URL" "$SDL_SHA" "SDL2-$SDL_VER.tar.gz"
    rm -rf "$WORK/SDL2-$SDL_VER" "$WORK/sdl-obj"
    tar -C "$WORK" -xzf "$WORK/SDL2-$SDL_VER.tar.gz"
    local S=$WORK/SDL2-$SDL_VER
    (cd "$S" && patch -p1 --no-backup-if-mismatch < "$HERE/patches/sdl2-$SDL_VER-indevos.diff")
    mkdir -p "$S/src/video/indevos"
    cp "$HERE"/indevos/SDL_indevos*.[ch] "$S/src/video/indevos/"
    cp "$ROOT/userlib/libwm/wm_abi.h" "$S/src/video/indevos/wm_abi.h"
    cmake -S "$S" -B "$WORK/sdl-obj" -G "$GEN" \
        -DCMAKE_TOOLCHAIN_FILE="$TC" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$SYSROOT" \
        -DCMAKE_C_FLAGS="-O2 -fno-stack-protector" \
        -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_STATIC_PIC=OFF -DSDL_TEST=ON -DSDL_TESTS=OFF \
        -DSDL_INDEVOS=ON -DSDL_DUMMYVIDEO=ON -DSDL_OFFSCREEN=ON -DSDL_DUMMYAUDIO=ON -DSDL_DISKAUDIO=ON \
        -DSDL_X11=OFF -DSDL_WAYLAND=OFF -DSDL_KMSDRM=OFF -DSDL_OPENGL=OFF -DSDL_OPENGLES=OFF -DSDL_VULKAN=OFF \
        -DSDL_DIRECTFB=OFF -DSDL_RPI=OFF -DSDL_VIVANTE=OFF -DSDL_METAL=OFF \
        -DSDL_ALSA=OFF -DSDL_PULSEAUDIO=OFF -DSDL_PIPEWIRE=OFF -DSDL_JACK=OFF -DSDL_SNDIO=OFF -DSDL_OSS=OFF \
        -DSDL_ESD=OFF -DSDL_ARTS=OFF -DSDL_NAS=OFF -DSDL_FUSIONSOUND=OFF -DSDL_LIBSAMPLERATE=OFF \
        -DSDL_LIBUDEV=OFF -DSDL_DBUS=OFF -DSDL_IBUS=OFF -DSDL_HIDAPI=OFF -DSDL_RPATH=OFF \
        -DSDL_JOYSTICK=ON -DSDL_HAPTIC=ON -DSDL_SENSOR=ON -DSDL_POWER=ON \
        -DSDL_PTHREADS=ON -DSDL_CLOCK_GETTIME=ON -DSDL_DLOPEN=OFF -DSDL_LOADSO=OFF \
        -DSDL_WERROR=OFF -DSDL_DISABLE_INSTALL_DOCS=ON
    cmake --build "$WORK/sdl-obj" -j "$JOBS"
    cmake --install "$WORK/sdl-obj"
    grep -q '#define SDL_VIDEO_DRIVER_INDEVOS 1' "$SYSROOT/include/SDL2/SDL_config.h" \
        || { echo "SDL_VIDEO_DRIVER_INDEVOS not enabled"; exit 1; }
    echo "SDL2 $SDL_VER installed to $SYSROOT ($(stat -c %s "$SYSROOT/lib/libSDL2.a") bytes libSDL2.a)"
}

build_demo() {
    "$CC" -static -O2 -Wall -Wextra -Wno-unused-parameter -I"$SYSROOT/include/SDL2" -D_REENTRANT \
        "$HERE/sdl2demo.c" -L"$SYSROOT/lib" -lSDL2_test -lSDL2 -lm -lpthread -o "$HERE/build/sdl2demo.exe"
    echo "built tests/sdl2/build/sdl2demo.exe ($(stat -c %s "$HERE/build/sdl2demo.exe") bytes)"
}

build_doom() {
    fetch "$DOOM_URL" "$DOOM_SHA" "chocolate-doom-$DOOM_VER.tar.gz"
    rm -rf "$WORK/chocolate-doom-chocolate-doom-$DOOM_VER" "$WORK/doom-obj"
    tar -C "$WORK" -xzf "$WORK/chocolate-doom-$DOOM_VER.tar.gz"
    local S=$WORK/chocolate-doom-chocolate-doom-$DOOM_VER
    (cd "$S" && patch -p1 --no-backup-if-mismatch < "$HERE/patches/chocolate-doom-$DOOM_VER-indevos.diff")
    cmake -S "$S" -B "$WORK/doom-obj" -G "$GEN" \
        -DCMAKE_TOOLCHAIN_FILE="$TC" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$SYSROOT" \
        -DCMAKE_C_FLAGS="-O2 -fno-stack-protector -Wno-error" \
        -DENABLE_SDL2_MIXER=OFF -DENABLE_SDL2_NET=OFF \
        -DCMAKE_DISABLE_FIND_PACKAGE_PNG=ON -DCMAKE_DISABLE_FIND_PACKAGE_SampleRate=ON \
        -DCMAKE_DISABLE_FIND_PACKAGE_FluidSynth=ON
    cmake --build "$WORK/doom-obj" -j "$JOBS" --target chocolate-doom
    cp "$WORK/doom-obj/src/chocolate-doom" "$HERE/build/chocolate-doom.exe"
    echo "built tests/sdl2/build/chocolate-doom.exe ($(stat -c %s "$HERE/build/chocolate-doom.exe") bytes)"
}

case $WHAT in
    sdl) build_sdl ;;
    demo) build_demo ;;
    doom) build_doom ;;
    all) build_sdl; build_demo; build_doom ;;
    *) echo "usage: $0 [all|sdl|demo|doom]"; exit 2 ;;
esac
