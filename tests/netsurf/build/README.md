# tests/netsurf/build: rebuilding NetSurf and dhcp for inDevOS

The binaries `tests/netsurf/netsurf` (static musl NetSurf 3.11, framebuffer
frontend drawing through `/dev/wm`) and `tests/netsurf/dhcp` are **not in git**
(see `tests/netsurf/.gitignore`). `make_install_iso.sh` skips them with a
message when they are missing, so a fresh clone still builds `os_install.iso`,
just without the browser and `/bin/dhcp`.

## Rebuild

On a Debian/Ubuntu x86-64 host (WSL works):

    sudo apt-get install -y musl-tools musl-dev make gcc flex bison gperf pkg-config \
        zlib1g-dev libpng-dev perl python3 curl xz-utils linux-libc-dev fonts-dejavu-core patch
    tests/netsurf/build/build_netsurf.sh            # full build, ~10-20 min
    ONLY_DHCP=1 tests/netsurf/build/build_netsurf.sh   # just dhcp

The script writes `tests/netsurf/netsurf`, `tests/netsurf/dhcp` and refreshes
`tests/netsurf/res/`, including six DejaVu TTFs (Sans/Serif/Mono, regular+bold,
from `$FONTDIR`, default `/usr/share/fonts/truetype/dejavu`). NetSurf is built
with freetype 2.13.3 (`NETSURF_FB_FONTLIB=freetype`); the old internal bitmap
font had no Greek/Cyrillic glyphs. Refresh `../MD5SUMS` after a rebuild.
Then run `./make_install_iso.sh` as usual.

- Work tree: `$NETSURF_WORK` (default `~/netsurf-build`, ~1.5 GB) holds the
  downloaded tarballs, the extracted sources, a static sysroot
  (OpenSSL 3.0.15, freetype 2.13.3, zlib 1.3.1, libpng 1.6.44, IJG jpeg 9f, expat 2.6.3,
  curl 8.10.1) and `log_*.txt`. Tarballs are SHA-256-checked. A failing step
  leaves its log there; delete `$NETSURF_WORK/sysroot/lib/lib<x>.a` to redo a lib.
- `J=<n>` sets make parallelism (default `nproc`).
- No absolute paths from the original build machine are used. The script
  finds the repo relative to itself, and the wm ABI header comes from `src/wm_abi.h`.
  The musl "kernel headers" are symlinks to the host's `/usr/include/linux`,
  `/usr/include/asm-generic` and `/usr/include/x86_64-linux-gnu/asm`
  (Debian multiarch layout; adjust on other distros).

`dhcp` alone, by hand:

    musl-gcc -static -Os -s -o tests/netsurf/dhcp tests/netsurf/dhcp.c

`dhcp` uses the inDevOS private syscall 1000 (op 1 = dhcp, op 2 = ddhcp,
op 3 = status). `dhcp -a` is what the NetSurf desktop shortcut runs.

## Patches (applied by build_netsurf.sh, idempotent)

- `patches/nsfb_wm.c`: libnsfb surface `wm` (`/dev/wm`, ABI from `src/wm_abi.h`),
  the default surface because it has the lowest enum. `NSFB_WM_DEBUG=1` logs events to stderr.
- `patches/apply_wm_surface.sh <netsurf-all-3.11/libnsfb> [wm_abi.h]`: installs
  the surface into libnsfb and runs:
- `patches/fbtk_text_ctrl.py <.../fbtk/text.c>`: Ctrl+A / Ctrl+C / Ctrl+V in the URL bar.
- `patches/html_preload_css.py <.../html/css.c>`: also load `<link rel=preload as=style>`
  stylesheets (many sites load their CSS that way and only add a `<noscript>` fallback).
- `patches/js_events.py <netsurf>`: fire `DOMContentLoaded` (document + window
  listeners) and run `window.addEventListener('load', ...)` listeners.
- `patches/libdom_class_cache.py <libdom/src/core/element.c>`: refresh libdom's
  class cache when an existing `class` attribute changes (JS `className=` /
  `setAttribute('class')` now restyle when done before layout).
- `patches/libdom_event_target.py <libdom/src/core/node.c>`: listeners on the event
  target ran twice (AT_TARGET and BUBBLING); now once.

## JavaScript engine: QuickJS (default)

`NETSURF_JS=quickjs` (default) builds Bellard's QuickJS 2025-04-26 (engine only,
`$NETSURF_WORK/sysroot/lib/libquickjs.a`) and NetSurf's JS backend from
`quickjs/qjs.c` (bindings, timers, fetch/XHR, post-layout relayout "rebox",
ES module loader via libcurl) and `quickjs/dom.js` (DOM prelude, minimal Intl),
compiled in with xxd. `NETSURF_JS=duktape` builds the stock Duktape backend
instead (use a fresh `$NETSURF_WORK` when switching an already patched tree).
Debug env: `NETSURF_QJS_DUMP=1` (DOM/box dumps), `NETSURF_GRID_DEBUG=1`.

Patches for it (unified diffs `patches/<name>.diff`, -p1 against netsurf-all-3.11,
applied by `patches/<name>.py <netsurf-all-3.11>` through `patches/nsdiff.py`;
needs GNU `patch`; idempotent):

- `js_quickjs`: `NETSURF_USE_QUICKJS` switch (-lquickjs), copies `quickjs/*` to
  `content/handlers/javascript/quickjs/`, KeyboardEvent.key names, keydown at the focused control.
- `libcss_grid`: CSS Grid properties in libcss (`grid/libcss_parse_grid.c`,
  `grid/libcss_select_grid.c`): grid-template-columns/rows (fr, repeat, auto-fill/fit,
  minmax), grid-auto-rows/columns/flow, grid-column/row (lines, span), gap.
- `layout_grid`: grid layout (`grid/layout_grid.c`); grid boxes use the flex box types
  and `layout_flex` dispatches to it; justify/align basics, auto rows sized to content.
  Also blockifies inline children of flex/grid containers, places inline-flex/grid on
  lines, and lets `box_coords` handle floats laid out as flex/grid items.
- `forms_orphans`: frees form controls outside any `<form>` (were leaked per relayout).
- `js_modules`: `<script type=module>` (external ones deferred), `type=importmap`, `nomodule`.

Not supported: subgrid, named lines/areas, dense packing, custom element upgrades.
Test pages: `tests/netsurf/test/` (grid1, inl, module + mod/, orphan).

## Reference

The binaries used for the gated ISOs (`os_install.iso` md5 de8f94c9...) had
these md5s: `netsurf` 043e3c39b68f887ddd1007ade1ab364b (9,397,632 B),
`dhcp` 1d60041ef367aa5e36741a363b0a3625 (30,312 B). A rebuild need not be
byte-identical. `../MD5SUMS` covers only the tracked runtime files.
See `NOTES.md` for build notes and the syscalls NetSurf uses.
