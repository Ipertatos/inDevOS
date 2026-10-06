# Herb Engine (herb-engine-C-3D) on inDevOS

Port of [herb-engine-C-3D](https://github.com/42henry/herb-engine-C-3D) as a **user-space** app using the SDL syscall layer and a new pixel-upload syscall.

## Build (host)

From the repo root:

```bash
./build_userapp.sh herb_engine_indevos.c herb_engine.exe
```

This uses `include/herb_indevos_compat.h` for freestanding `malloc`/`realloc`/`qsort`/math, includes `third_party/herb-engine-C-3D/herbengineC3D.c`, and links with `-lgcc` (no `libm`; math is inlined in the compat header).

**Stack alignment:** the build passes **`-mstackrealign`**. Without it, GCC emits `movaps` to RBP-relative locals; the kernel `iretq` initial `RSP` can leave the stack 8 mod 16 relative to what those prologues assume, causing an immediate **#GP (General Protection Fault)** in user mode (often at the first `movaps`).

Copy `herb_engine.exe` to your disk image (e.g. `putfilesinsata.sh` if configured).

## Run (in inDevOS)

```text
exec herb_engine.exe
```

## Kernel requirements

Rebuild the kernel after pulling this tree so these are included:

- **`SYS_SDL_COPY_PIXELS` (44)** — copies a user RGBA8888 buffer into the kernel SDL surface (`SDL_CopyPixelsFromUser` in `include/syscalls.h`).
- **Keyboard → SDL** — `keyboard.c` calls `SDL_InjectKeyEvent` on every PS/2 key event; `SDL_InjectKeyEvent` no-ops until `SDL_EventsInit()` has run (see `docs/INPUT_PATHS_INDEVOS.md`).
- **Event ABI** — `include/sdl_event_types.h` must match the kernel `SDL_Event` (`sizeof` = 28 bytes); `herb_engine_indevos.c` includes it.
- **Mouse → SDL** — `sdl_events.c` `SDL_PumpEvents` reads PS/2 mouse position/buttons.

## Resolution

Default is **1024×576** (`WIDTH` / `HEIGHT` in `herb_engine_indevos.c`). Adjust to match your framebuffer if needed.

## Upstream source

Refresh vendored files with:

```bash
make fetch-herb-engine
```

Edits to `third_party/herb-engine-C-3D/herbengineC3D.c` for inDevOS are guarded by `__INDEVOS_USERAPP__`; re-apply or merge after updating upstream.
