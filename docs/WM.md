# inDevOS Desktop WM — Phase 1

## Before

The modern DE in `desktop.c` already had windows, decorations, and a
compositor, but the kernel treated **any focused Ring-3 window** as
“owns the framebuffer”:

- `kernel.c` skipped `desktop_composite_frame()` whenever the focused
  window had `pid != 0`.
- Only `desktop_poll_input()` ran — chrome hit-testing and focus still
  partially worked, but **client GUI mouse events were not delivered**,
  and with multiple windows the stack/chrome stopped updating.
- That made the desktop feel like one rendered program (especially with
  SDL / Doom-style clients) rather than a window manager.

LuaDE (`luade_*.c`) remains a separate fullscreen Lua draw path and is
not required for the C WM; SDL clients already register via
`desktop_create_window` + shared client buffers.

## After (Phase 1)

Clear WM / compositor split inside `desktop.c` + `desktop.h`:

| Concern | Owner |
|--------|--------|
| Focus id, raise-on-focus, Z-order | WM |
| Title bar / traffic lights / border | WM (drawn each composite) |
| Title-bar move, edge resize, pointer grab | WM |
| Hit-test → chrome action vs client | WM |
| Pointer/keyboard to focused client only | WM → `process_gui_enqueue_by_pid` |
| Wallpaper + stack + panel + cursor | Compositor |
| Client pixels | SDL / `SYS_DESKTOP_COMPOSITE` / kernel apps |

New / tightened APIs:

- `desktop_wm_needs_composite()` — kernel uses this instead of naive
  “focused app owns FB”.
- `desktop_count_visible_windows()`
- Shared `dt_wm_pointer_frame()` used by both `desktop_poll_input` and
  `desktop_composite_frame` (focus, chrome, drag/resize, client events).

Fail-closed OOM in `desktop_create_window` is unchanged.

## What works now

- Click-to-focus + raise-on-focus
- Z-order stacking when multiple windows are visible (full composite)
- WM chrome (title bar, close / min / max)
- Title-bar drag move; edge resize (existing geometry path)
- Pointer grab while dragging/resizing (no client events during grab)
- Client-relative MOUSE_MOVE / DOWN / UP / CLICK / RIGHT_CLICK to focused pid
- Keyboard still via `desktop_gui_enqueue_key_sdl` / `desktop_handle_key`
- Single-client fast-present path retained when WM says composite is unnecessary

## Phase 2 (not landed)

- Multi-monitor / per-output layout
- Server-side decoration themes / shadow policy polish
- Pointer warping, modal grabs, always-on-top
- Stronger damage tracking for chrome-only updates during single-client FPS
- Integrate LuaDE as a WM client (draw into a desktop window) instead of
  fullscreen FB takeover

## How to try in QEMU

From the Windows tree (`C:\osproj`), usual GPU/desktop runners:

```text
.\build_windows.ps1
.\run_gpu_windows.ps1
```

(or `run_windows.bat` / project README equivalents)

In the guest:

1. Start the desktop (project’s usual `desktop` / shell command).
2. Launch two dock apps (e.g. terminal + files) — both frames should
   stay stacked; click either to focus/raise.
3. Drag by the title bar; use traffic lights; click client area and
   confirm the focused app receives input.
4. Fullscreen-ish single game client should still fast-present; opening
   a second window should force full compositing again.

Do **not** commit/push from this pass — local WIP only.
