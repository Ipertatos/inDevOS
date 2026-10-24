#!/usr/bin/env bash
# Idempotently patch netsurf-all-3.11 for inDevOS:
#  - libnsfb: add the /dev/wm surface ("-f wm", lowest enum => default)
#  - fbtk text widget (URL bar): Ctrl+A / Ctrl+C / Ctrl+V (fbtk_text_ctrl.py)
# Usage: apply_wm_surface.sh <path/to/netsurf-all-3.11/libnsfb> [wm_abi.h]
# wm_abi.h defaults to the repo's src/wm_abi.h (this dir is tests/netsurf/build/patches).
set -euo pipefail
P=$(cd "$(dirname "$0")" && pwd)
L=${1:?usage: apply_wm_surface.sh <netsurf-all-3.11/libnsfb> [wm_abi.h]}
ABI=${2:-$P/../../../../src/wm_abi.h}
[ -f "$ABI" ] || { echo "wm_abi.h not found at $ABI" >&2; exit 1; }
cp "$P/nsfb_wm.c" "$L/src/surface/wm.c"; cp "$ABI" "$L/src/surface/wm_abi.h"
grep -q NSFB_SURFACE_WM "$L/include/libnsfb.h" || \
  sed -i 's|^\(    NSFB_SURFACE_NONE = 0, /\*\*< No surface \*/\)|\1\n    NSFB_SURFACE_WM, /**< inDevOS /dev/wm window (lowest => default) */|' "$L/include/libnsfb.h"
grep -q 'wm\.c' "$L/src/surface/Makefile" || \
  sed -i 's|^SURFACE_HANDLER_yes := surface.c ram.c|SURFACE_HANDLER_yes := surface.c ram.c wm.c|' "$L/src/surface/Makefile"
grep -q NSFB_SURFACE_WM "$L/include/libnsfb.h" && grep -q 'wm\.c' "$L/src/surface/Makefile" || { echo "libnsfb patch failed" >&2; exit 1; }
python3 "$P/fbtk_text_ctrl.py" "$L/../netsurf/frontends/framebuffer/fbtk/text.c"
