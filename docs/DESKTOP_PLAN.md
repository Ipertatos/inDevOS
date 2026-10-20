# Desktop shell, icons and file manager: survey and plan

Status: plan only, nothing implemented. Written 2026-10-20 against `cbc858b`
(master). Builds on the kernel window manager in `src/desktop.c`, not LuaDE.

## 1. What exists today

### 1.1 Window manager / compositor (`src/desktop.c`, ~3000 lines)

* Static pool of `DESKTOP_MAX_WINDOWS` (32) `desktop_window_t`, doubly linked
  Z-order list (`win_bottom` .. `win_top`), focus id and pointer-grab id.
* Each window owns a kernel `kalloc` XRGB8888 buffer (`pixels`, pitch = w*4)
  guarded by `content_lock`. Chrome (title bar, traffic-light buttons, border,
  shadow, rounded corners) is drawn by the WM; clients only fill the client rect.
* Compositor draws into a private backbuffer (wallpaper cache -> windows ->
  menubar -> dock -> start menu / switcher -> cursor) and copies the damaged area
  to the framebuffer. Damage is a **single bounding rect** (`dt_damage_add`), so
  two small changes far apart repaint everything between them.
* Pacing: `desktop_composite_frame()` is called from the kernel main loop
  (`kernel.c:446`) and throttled to ~60 fps on HPET ticks; between frames it
  still runs `dt_wm_pointer_frame()` so clicks are not lost. Also called
  synchronously from `desktop_present_now()` and the present syscalls.
* Kernel apps are ticked from the compositor: `notepad_tick`, `files_tick`,
  `kterm_tick`, `settings_app_tick` (desktop.c ~1772).
* Drawing library `src/render.c` (`r_target_t`): fill, alpha blend
  (`r_over`, `r_fill_blend`, `r_rrect_blend`), gradients, circles, rounded
  rects, shadows, `r_blit` / `r_blit_keyed`, text. Alpha blending already
  exists in the renderer; window composition itself is opaque or colour-keyed.
* UI chrome already present: top menubar with logo/start menu and clock, a
  bottom dock (`dt_dock_apps[]`: Note, Term, Files, Settings, Doom; optional PNG
  dock icons), Alt-Tab style switcher, minimise/maximise/fullscreen, edge
  resize, title double-click to maximise (400 ms).
* Desktop icons already exist in a primitive form: `dt_refresh_desktop_icons()`
  reads the first non-empty of `/home/user/Desktop`, `/Desktop`, `/os/Desktop`
  (max 32 entries), draws a coloured badge with the first letter plus the name
  in a fixed grid (88x80), and on double-click (400 ms) does
  `exec_spawn(path, NULL)` for files. Folders only log
  `[desktop] open folder`. No icon images, no selection, no drag, no right-click,
  no refresh when the folder changes, no associations.

### 1.2 How apps get a window

1. **Kernel-side apps** (pid 0 windows): `notepad.c`, `files.c`, `kterm.c`,
   `settings_app.c`. They call `desktop_create_window(0, ...)` and draw with
   `render.c`. Input is hand-routed by `desktop.c`: keys via
   `desktop_gui_enqueue_key_sdl()` -> `files_on_key()` etc., mouse via
   `desktop_handle_mouse()` -> `*_on_mouse(rx, ry, true)` (**left button down
   only**; no move, up, right-click or wheel reach kernel apps).
2. **Native inDevOS user programs** (int 0x80 ABI) through the in-kernel SDL shim
   (`SYS_SDL_CREATE_WINDOW` etc., `sdl_video.c`) which creates a desktop window
   owned by the pid; `SYS_DESKTOP_COMPOSITE` (70) copies a client buffer in
   (with in-kernel integer upscaling, used by Doom), `SYS_DESKTOP_EMBED_INFO`
   (49) and `SYS_DESKTOP_POLL_EVENT` (94) read events from the per-process
   `gui_event_queue_t` (32 entries; `include/indevos_gui.h`: PAINT, CLICK,
   RIGHT_CLICK, CLOSE, KEY, KEYUP, MOUSE_DOWN/UP/MOVE/WHEEL, RESIZE).
   Windows are looked up **by pid, first match** (`desktop_find_window_by_pid`),
   so in practice one window per process.
3. **Linux-ABI programs** (static musl `.exe`): no window API at all. The only
   graphics path is `/dev/fb0` (`lx_fbdev.c`), which takes over the whole
   display and hands it back on exit. AF_UNIX sockets, memfd and SysV/POSIX shm
   are not implemented (`linux_syscall.c` sockets are AF_INET only).

So: user space can create windows only through the native SDL shim; Linux
programs (the ones that matter now: busybox, tcc output, ported apps) cannot.

### 1.3 Launchers, file browser, editor, terminal

* `dt_launch_terminal` -> `kterm_open()`, `dt_launch_files` -> `files_open()`,
  `dt_launch_settings` -> `settings_app_open()`. They are defined once in
  `desktop.c` (~1198); only the forward declarations are duplicated (lines
  132-135 and 159-161) - harmless, can be merged.
* `dt_dock_spawn()` uses `exec_spawn` with fallback paths and remembers the pid
  per slot; the pid is only cleared in `dt_close_window`, so an app that exits
  on its own may still look "running" in the dock.
* `dt_launcher_spawn_default_app()` probes a list of demo binaries, else opens an
  "About" kernel text window.
* **Files** (`files.c`, 255 lines): kernel window 560x420, list of at most 40
  entries (dirs first, sorted), Up button, arrow keys / Enter / Backspace,
  click enters folders. Files: status "Not a folder" - cannot open or run
  anything, no file operations, no scrolling past 40 entries.
* **Notepad** (`notepad.c`, 655 lines): kernel editor, 32 KB buffer, 128-char
  path, open/save via `vfs_read_file_alloc` / `vfs_write_file`, selection,
  clipboard (`clipboard.c`, 64 KB). Good basis for a "text editor" association.
* **kterm** (`kterm.c`, 552 lines): runs `busybox sh -i` on two kernel pipes,
  but it is line-based: the window does line editing/echo and strips CSI escape
  sequences (`kt_esc`). Full-screen TUI programs (busybox `vi`, `less`, `top`)
  will not work in it; they work on the text VTs. A VT100 cell-grid terminal
  would be needed for `vi` as the editor of last resort.
* No image viewer exists. `png.c` (812 lines) decodes 8-bit non-interlaced PNG
  to RGBA (used for wallpaper / dock icons). No BMP/JPEG decoder in the kernel.
* Fonts: a single built-in 8x16 bitmap (`font_16x8`, ASCII 32-126) used by
  `r_char`/`r_text` with a soft outline. No other sizes, no TTF, no UTF-8.

### 1.4 VFS and process APIs relevant to a file manager

* `vfs.h`: `vfs_readdir(path, vfs_dirent_t*, max)` (name[256], inode, type,
  size - no timestamps or mode), `vfs_open/read/write/seek/close`,
  `vfs_create`, `vfs_mkdir(_p)`, `vfs_unlink` (file or empty dir),
  `vfs_rename`, `vfs_exists`, `vfs_read_file_alloc`, `vfs_write_file`,
  `vfs_ioctl`. Up to 8 mount points (FAT32 via `vfs_fat32.c`, ext4 read via
  `vfs_ext4.c`, device nodes). No copy, no recursive delete, no mtime.
  From user space the Linux ABI exposes the same through
  `getdents64`/`stat`/`rename`/`unlink`/`mkdir`, already exercised by busybox
  `cp -r`, `mv`, `rm -r`.
* Spawning from kernel code: `exec_spawn(path, argv)` returns a pid at once;
  `exec_spawn_set_stdio_pipes(in, out)` sets stdio for the next spawn. There is
  no cwd or envp parameter (cwd lives in `process_t.cwd`, set via
  `process_set_cwd`; children inherit the kernel session cwd). Exit is noticed
  by polling `process_pid_alive(pid)` (kterm does this) or, for windowed apps,
  by `desktop_destroy_windows_for_pid`. No exit callback.
* `.exe` resolution: `stat`/`access` fall back to `<path>.exe`; `exec.c` also
  runs `.lua` scripts through `lua_runtime.c` when `ENABLE_LUA`.

### 1.5 LuaDE (not planned around; removal cost)

Files: `luade.c` (545), `luade_graphics.c/.h` (520), `luade_input.c/.h`
(258), `luade.h`. Hooks to remove:

* `kernel.c:428-432` (`luade_tick` in the main loop)
* `keyboard.c:421-436` (`luade_owns_input` / `luade_keyboard_event`)
* `mouse.c:4-5, 351, 363, 592, 602` (`luade_mouse_event`)
* `vconsole.c:185-186, 229-230` (`luade_is_running`, `luade_notify_vt_return`)
* `lx_fbdev.c:143-144` (`luade_notify_vt_return`)
* `idt.c:706-708, 753-755` (`luade_print_last_lua_location` in fault dumps -
  move this helper into `lua_runtime.c` rather than drop it)
* `zeus.c:431` (completion list), `969-976`, `1049-1067` (`luade`,
  `luadestop` commands); `zeus_cmds.c:262` help text
* `Makefile:83` filter list
* `docs/LUA.md` and any `.lua` desktop scripts on the disk image

Keep `lua_runtime.c` and `third_party/lua` (Zeus/exec run `.lua` scripts).
Estimated half a day plus a test pass; needs Georgios's OK because it deletes
files.

## 2. Architecture choice

### Option A: everything in the kernel (extend `desktop.c`, `files.c`)
* Effort: lowest to first result (~1-2 weeks for icons + a usable file
  manager); reuses `render.c`, `png.c`, `vfs_*`, kernel-app pattern.
* Cost: every bug (bad PNG, FAT32 edge case during a recursive copy, buffer
  overflow in a path join) is a kernel crash. File copies of big files block
  the main loop unless threaded. Apps cannot be written in normal Linux C,
  cannot be debugged with `lxtrace`, cannot use musl/stb libraries.

### Option B: user-space window protocol (`/dev/wm`) + apps as `.exe`
* A char device modelled on what `/dev/fb0` already does (vfs ioctl +
  `sys_mmap_device` + poll): open = connection; ioctls create windows; each
  window surface is mmapped shared into the client; events are `read()` from
  the fd and `poll()`-able. Process exit closes the fd and destroys its
  windows.
* Effort: ~2 weeks for protocol + client library before the first app.
* Gains: crash isolation (a crashing file manager or image viewer just closes);
  apps are ordinary static musl programs built like busybox/tcc tests; can
  use stb_image, stb_truetype, fork/execve, threads; same API later usable by
  ported toolkits (an SDL2 video backend on `/dev/wm` would bring many apps).
* Why a device and not a socket: AF_UNIX, memfd and shm do not exist yet;
  a device reuses tested paths (ioctl dispatch, device mmap, poll wakeups).
  A Wayland-like socket protocol can be layered later.

### Recommendation: hybrid, B for apps

* **Kernel keeps the desktop shell**: WM, compositor, menubar, dock, desktop
  icon layer, context menus, notifications, and a small *launch service*
  (associations + spawn). These are always-on, small, and need direct access
  to input and the framebuffer.
* **Apps become `.exe` clients of `/dev/wm`**: file manager (`files.exe`),
  image viewer (`imgview.exe`), text editor (`textedit.exe`, port of
  `notepad.c`), later settings. Kernel `files.c` / `notepad.c` stay as the
  fallback until the `.exe` versions pass tests, then are removed (with OK).
* Native SDL-shim apps (Doom) keep working unchanged.
* If Georgios wants something visible fast, Phase 1 (shell + associations)
  is pure kernel work and delivers clickable icons that open files before the
  protocol exists.

## 3. Feature design

### 3.1 Desktop icons
* Source: `/home/user/Desktop` (created by `vfs_seed_standard_layout` if
  missing; drop the `/os` fallback). Items: real files/folders plus
  shortcut files `*.desktop` (INI: `Name=`, `Exec=`, `Icon=`, `Path=`).
  Seed shortcuts: Files, Terminal, Text Editor, Doom, Settings.
* Icon images: 32x32 (and 48x48) ARGB PNGs in `/usr/share/icons/indevos/`
  (`folder.png`, `file.png`, `text.png`, `image.png`, `exe.png`, `wad.png`,
  `drive.png`, ...), decoded once with `png.c` and cached; built-in vector
  fallback drawn with `render.c` so a missing file is never fatal. Icon chosen
  by the association table (3.3).
* Behaviour: single click selects (highlight), Ctrl-click multi-select,
  rubber-band select, double-click / Enter opens via the launch service,
  drag to move with snap-to-grid; layout saved to
  `/home/user/.config/desktop/layout` (`name x y`), new items auto-placed
  in free cells. Right-click menu: Open, Open with..., Rename, Delete, Copy
  path, New folder, New text file, Open terminal here, Refresh, Change
  wallpaper. Label truncation with ellipsis, two lines max.
* Refresh: re-read on focus/after any launch-service file op and every few
  seconds (no inotify yet).

### 3.2 File manager (`files.exe`)
* Window: toolbar (Back, Forward, Up, Refresh, view toggle), editable path bar
  (also breadcrumb buttons), left sidebar (Home, Desktop, Documents, `/`,
  `/bin`, mounted drives), main list or icon grid, status bar (n items,
  selection size, free space when the FS reports it).
* Views: details list (name, size, type; date once the VFS exposes mtime -
  add `stat` mtime from FAT32 dir entries), icon grid; sort by name/size/type;
  show hidden toggle; type-ahead find; scrollbars and wheel; no 40-entry cap.
* Open: double-click/Enter -> folder navigates; file goes to the launch
  service (`/dev/wm` ioctl `WM_OPEN_PATH`, or the app forks/execs itself from
  the same association file). `.exe` runs with cwd = its folder; "Run in
  terminal" for console programs (opens kterm with the command).
* Operations, all with confirmation dialogs and progress for long copies:
  copy / cut / paste (Ctrl-C/X/V, internal clipboard of paths), drag-and-drop
  within the window, delete (confirm; recursive for folders), rename (F2,
  inline), new folder, new file, properties (size, type, path). Overwrite
  prompt (Replace / Skip / Cancel). Copy loop in user space (read/write 64 KB
  chunks) so a stuck FAT32 write cannot freeze the desktop.
* Later: trash folder (`/home/user/.local/share/Trash`), thumbnails for PNG.

### 3.3 File associations
* File `/etc/indevos/open-with` (plain text, LF), first match wins:
  ```
  # pattern      icon     command               (%f path, %d dir)
  dir            folder   /bin/files.exe %f
  *.exe          exe      %f                    (run, cwd = %d)
  *.txt|*.c|*.h|*.md|*.sh|*.lua|*.cfg|*.conf  text  /bin/textedit.exe %f
  *.png|*.bmp|*.jpg|*.jpeg|*.gif  image  /bin/imgview.exe %f
  *.wad          wad      /bin/doom.exe -iwad %f   (verify Doom's argv flag)
  *.lua          script   /bin/lua %f
  *              file     /bin/textedit.exe %f     (fallback; or "Open with")
  ```
* Per-user override in `/home/user/.config/open-with`. "Open with..." lists
  every distinct command in the table. Content sniffing for extensionless
  files: ELF magic -> run; PNG/JPEG magic -> image; mostly printable -> text.
* Kernel launch service `desktop_open_path(path)`: parse table (cached,
  reloaded on mtime/size change), build argv, spawn via a new
  `exec_spawn_ex(path, argv, envp, cwd, stdio)` (extends `exec_spawn` with
  cwd/env), track pid for dock/taskbar, show a toast on failure instead of
  only a serial line.

### 3.4 Image viewer and text editor
* `imgview.exe`: stb_image (public domain: PNG incl. interlaced/16-bit, JPEG,
  BMP, GIF), fit/1:1/zoom with wheel, pan, next/previous in folder, set as
  wallpaper.
* `textedit.exe`: port `notepad.c` logic to user space, no 32 KB cap, Save As,
  find, unsaved-changes prompt on close, line numbers. Kernel notepad stays
  until replaced.
* Optional later: a VT100 grid mode for kterm so `vi`/`less` work in a window.

## 4. Compositor and input upgrades needed

1. **Damage list**: up to ~16 rects with merge-on-overlap, fall back to full
   screen; present only those rects (blinking caret + clock + cursor no longer
   repaint a whole band).
2. **Per-window pixel format**: opaque XRGB, colour-keyed (current) and
   premultiplied ARGB (for shaped popups, icons, toasts, drag images).
   Icon blending on the desktop layer via existing `r_over`.
3. **Window ids instead of pid lookup**: several windows per process (file
   manager + dialogs); `desktop_find_window_by_pid` remains for the SDL shim.
   Raise the pool to 64. Window types: normal, dialog (transient for a parent,
   modal), popup/menu (no chrome, closes on outside click), tooltip.
4. **Full input routing**: deliver move, up, right button, middle, wheel
   (PS/2 IntelliMouse delta is parsed in `mouse.c` but not routed by the WM),
   modifiers, key repeat, and text (character) events to both kernel apps and
   clients; enter/leave; double-click synthesis in one place (WM computes
   `clicks=2` with configurable 400 ms / 4 px) instead of per-feature timers.
5. **Context menu widget** in the WM (shared by desktop, title bar and
   offered to clients through `WM_POPUP`).
6. **Cursor shapes** (arrow, I-beam, resize, busy) and a drag image.
7. **Fonts**: load PSF2 bitmap fonts from disk for the kernel UI (e.g. Terminus
   8x16, 12x24, 16x32; OFL-licensed) with HiDPI scaling on the laptop; in user
   space use stb_truetype + one TTF (DejaVu Sans, permissive licence) for
   proportional text. Keep the built-in 8x16 as fallback.
8. **Toasts/notifications** for launch errors and long copy completion.

## 5. `/dev/wm` protocol sketch (Phase 2)

* `open("/dev/wm", O_RDWR)` -> connection (one per process is typical).
* `ioctl WM_CREATE {w, h, format, type, parent, title[64]}` -> `{id,
  mmap_off, stride}`; `mmap(NULL, 2*stride*h, PROT_READ|PROT_WRITE,
  MAP_SHARED, fd, mmap_off)` gives two buffers (double-buffered).
  Surfaces come from contiguous physical pages (PMM) because
  `sys_mmap_device` maps a physical range; alternatively add a page-list
  mapper so the heap does not fragment for large windows.
* `ioctl WM_PRESENT {id, buffer_index, n_rects, rects[16]}` -> compositor
  switches front buffer and adds damage; returns immediately; a FRAME event
  tells the client it can draw again (simple flow control).
* Events: `read(fd, struct wm_event[], n)`; `poll` POLLIN.
  `struct wm_event { u32 type, win; i32 x, y; u32 button, clicks, key,
  keysym, mods; char text[8]; }` - types: EXPOSE, FRAME, CLOSE_REQUEST,
  RESIZE (client then `WM_RESIZE_ACK` with a new surface), FOCUS_IN/OUT,
  ENTER/LEAVE, MOTION, BUTTON_DOWN/UP, WHEEL, KEY_DOWN/UP, TEXT, DROP (paths).
* Other ioctls: `WM_DESTROY`, `WM_SET_TITLE`, `WM_SET_GEOMETRY`,
  `WM_SET_CURSOR`, `WM_POPUP {x, y}`, `WM_CLIPBOARD_GET/SET` (backed by
  `clipboard.c`), `WM_OPEN_PATH` (launch service), `WM_SCREEN_INFO`.
* Close/crash: fd release destroys all windows of the connection; the WM never
  blocks on a client (it composites the last presented buffer).
* Client library `libwm` (static, musl, under `apps/lib/`): connect, window,
  event loop, ARGB drawing (port of `render.c`), PNG via stb_image, text via
  PSF/stb_truetype, widgets (button, list view with scrollbar, text field,
  menu, dialog, file-op progress). Native int80 programs keep the SDL shim.

## 6. Phases and rough sizes

| Phase | Content | Size |
|---|---|---|
| 0 | Compositor/input groundwork: damage list, ARGB windows, window ids & types, full input + double-click, WM context menu, PSF2 fonts, toasts, merge duplicate prototypes, dock pid cleanup on exit | 4-6 days |
| 1 | Desktop shell in kernel: icon layer rewrite (images, selection, drag/snap, layout file, context menu, refresh), `.desktop` shortcuts, association file + launch service + `exec_spawn_ex` (cwd/env), make kernel `files.c` open files via it as stopgap | 4-6 days |
| 2 | `/dev/wm` device + `libwm` + a demo client; crash-isolation tests | 8-12 days |
| 3 | `files.exe` file manager (views, path bar, sidebar, ops with confirmations, DnD, progress) | 8-12 days |
| 4 | `imgview.exe` (stb_image) and `textedit.exe` (notepad port); switch associations to them; retire kernel `files.c`/`notepad.c` with OK | 5-8 days |
| 5 | Polish: VT100 kterm (vi/less in a window), trash, thumbnails, LuaDE removal (with OK), SDL2 backend on `/dev/wm` (optional) | 1-2 weeks |

First visible result (icons that open files) at the end of Phase 1, about
two weeks of focused work for Phases 0-1.

## 7. Test approach (QEMU)

* Extend `scripts/desktop_app_probe.sh` / `qmp_scenario.py`: add mouse steps
  via QMP `input-send-event` (relative PS/2: home to (0,0) with a large
  negative move, then move to target; or add `-device usb-tablet` for
  absolute positioning), `click`, `dblclick`, `rclick`, `drag`, `key` combos.
  Unique QMP/serial ports per run, at most two QEMUs at once.
* Test disk content (sata_disk.img, test-only, not committed):
  `/home/user/Desktop/{notes.txt, pic.png, hello.exe, folder/}`, a `.wad`
  shortcut when doom1.wad is present locally, a 5 MB file for copy progress.
* Assertions from serial markers (`[desktop] open <path> -> <cmd> pid N`,
  `[wm] create id N pid N`, `[files] copy done bytes N`, `[wm] client N gone,
  windows destroyed`) plus screenshots at each step for visual review.
* Scenarios: double-click each desktop icon type; right-click > New folder >
  rename > delete with confirm; copy/paste file and verify size with busybox
  `ls -l` on the VT; run `.exe` and check cwd (`pwd` output in a test exe);
  kill `files.exe` with SIGSEGV (`kill -11`) and confirm desktop survives and
  windows vanish; resize/maximise client windows; 30 windows open at once.
* Run the existing Ctrl-C suite, Doom start/quit, fbgrad and the UEFI
  install-and-boot test after each phase; BIOS and UEFI.
* Host-side unit test (gcc in WSL) for the association parser and path join /
  normalisation helpers.

## 8. Small issues found during the survey

* Duplicate static prototypes in `desktop.c` (132-135 vs 159-161).
* `dt_dock_spawned_pid[]` is cleared only when the window is closed by the WM,
  not when the process exits, so the dock can mark a dead app as running and
  refuse to relaunch it.
* Desktop folder icons do nothing on double-click (log only).
* `files.c` silently truncates directories to 40 entries.
* Kernel apps receive only left-button-down (no wheel, right click, release).
* kterm strips escape sequences, so TUI programs render incorrectly there.
