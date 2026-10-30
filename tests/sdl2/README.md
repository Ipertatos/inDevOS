# SDL2 for inDevOS

SDL2 2.30.9 cross-built static against musl, with a native video driver
(`indevos`) on top of `/dev/wm` (ABI v1, see `userlib/libwm/wm_abi.h`).
SDL2 programs open as normal windows on the inDevOS desktop.

    tests/sdl2/build_sdl2.sh            # SDL2 + sdl2demo + chocolate-doom
    tests/sdl2/build_sdl2.sh sdl|demo|doom

The script downloads the release tarballs (sha256-checked) into `~/sdl2-build`,
applies `patches/sdl2-2.30.9-indevos.diff` (CMake option `SDL_INDEVOS`, config
define, bootstrap entry), copies in `indevos/` and `wm_abi.h`, and installs
into `~/sdl2-build/sysroot`. `make_install_iso.sh` picks up the binaries from
`tests/sdl2/build/`.

## Driver (`indevos/`)

| SDL part      | inDevOS backend |
|---------------|-----------------|
| video         | `/dev/wm`: one WM window per SDL window, double-buffered shared ARGB surface, `WM_PRESENT` of the dirty bounding box (software framebuffer; the SDL software renderer is used) |
| events        | `struct wm_event` reads: motion, buttons (with click count), wheel, keys (kernel keycodes -> SDL scancodes, US layout text input), focus, close, resize |
| resize        | user drags -> `SDL_WINDOWEVENT_RESIZED`; surface re-acked (`WM_RESIZE_ACK`) when SDL recreates the framebuffer. `SDL_SetWindowSize` recreates the WM window |
| wait          | `SDL_WaitEvent*` sleeps in `poll()` on the wm fd + a wakeup pipe |
| timers        | unix `clock_gettime(CLOCK_MONOTONIC)` |
| threads       | pthreads |
| audio         | dummy / disk (no sound device yet) |
| joystick etc. | dummy |

If `/dev/wm` cannot be opened (no desktop) the driver is skipped; set
`SDL_VIDEODRIVER=dummy` (or `--driver=dummy`) for headless use.

## Programs

* `/bin/sdl2demo.exe [seconds]` - animated window (software renderer, streaming
  texture, worker thread, timer) that prints every key/mouse/window event to
  stdout and lists them in the window. Esc or the close button quits.
* `/bin/sdl2demo.exe --selftest [--driver=dummy|indevos]` - headless checks
  used by ostest `sdl2.headless` and `sdl2.wm`.
* `/bin/chocolate-doom.exe -iwad /doom1.wad [-nosound]` - Chocolate Doom
  3.1.0 (SDL2_mixer and SDL2_net disabled). `patches/chocolate-doom-3.1.0-indevos.diff`
  makes it start windowed and accept the software renderer's "no texture size
  limit" (max 0x0), which otherwise yields a 0x0 upscale texture.
