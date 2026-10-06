# Keyboard & mouse paths in inDevOS (kernel → user apps)

This documents how input reaches **user-mode ELF apps** (e.g. `herb.exe`) vs the shell/desktop.

## User apps: SDL syscalls (recommended)

| Stage | Component | What happens |
|-------|-----------|----------------|
| Init | User | `SDL_Init(SDL_INIT_VIDEO \| SDL_INIT_EVENTS \| …)` → `SYS_SDL_INIT` → `SDL_Init()` → `SDL_EventsInit()` installs the **event queue** (`events_initialized`). |
| Poll | User | `SDL_PollEvent(&e)` → `SYS_SDL_POLL_EVENT` → kernel `sys_sdl_poll_event()` → `SDL_PollEvent()` → `SDL_PumpEvents()` + dequeue. |
| **Direct (recommended for games)** | User | `SYS_SDL_GET_KEYBOARD_STATE` (45) copies `sdl_keyboard_state[]` (1024 bytes, indexed by `SDL_Keycode`). `SYS_SDL_GET_MOUSE_REL_ACCUM` (46) returns accumulated PS/2 dx/dy since last call. `SYS_SDL_GET_MOUSE_BUTTONS` (47) returns `mouse_get_buttons()`. **Herb** uses these so input does not depend on the SDL event queue or `copy_to_user(sizeof(SDL_Event))`. |
| Copy | Kernel → user | One `SDL_Event` is **`copy_to_user`** into the app buffer. Size must match **`include/sdl_event_types.h`** (`sizeof(SDL_Event)` is **28** bytes with current GCC). |

**Keyboard (hardware → SDL queue)**  
- **IRQ / PS/2**: `keyboard_handler()` in `src/keyboard.c` reads port `0x60`, then calls **`SDL_InjectKeyEvent(is_down, scancode)`** (`src/sdl_events.c`).  
- **`SDL_InjectKeyEvent`** maps scancode → `SDL_Keycode`, **`push_event()`** into the ring buffer. It **no-ops** if `SDL_EventsInit()` has not run yet (`!events_initialized`).  
- **No dependency on `SDL_IsInitialized()`** for injection: the queue only needs **`SDL_EventsInit`** (done during `SDL_Init` when `SDL_INIT_EVENTS` is set).

**Mouse (hardware → SDL queue)**  
- **PS/2**: `mouse_handler()` → `mouse_process_packet()` in `src/mouse.c` updates global **`mouse_x` / `mouse_y` / `mouse_buttons`**, desktop hooks, etc.  
- **SDL**: `SDL_PumpEvents()` in `src/sdl_events.c` calls **`mouse_get_position()`** / **`mouse_get_buttons()`**, compares to previous state, and **pushes** `SDL_MOUSEMOTION` / `SDL_MOUSEBUTTON*` events.  
- **USB mice** (`src/usb.c`) update a separate path for desktop; **SDL pump** currently reads **PS/2 globals** only. If you use USB-only, wire those reports into the same globals or extend `SDL_PumpEvents`.

**Uaccess / CR3**  
- `sys_sdl_poll_event` and `sys_sdl_copy_pixels` call **`vmm_switch_address_space(current_process->pml4)`** before **`copy_to_user` / `copy_from_user`** so user pages are mapped if a preempting IRQ left the kernel page tables loaded.

**Invalid poll**  
- Bad/null user pointer: **`sys_sdl_poll_event` returns `0`** (same as “no event”), not `-1`, so `while (SDL_PollEvent(&e))` does not spin forever.

## Shell / kernel console (non-SDL)

- **`keyboard.c`**: ring buffer `buffer[]`, flags like `escape_pressed`, layout tables, `ps2_translate2ascii`, consumed by `cmd.c` / terminal.  
- This path is **parallel** to SDL; it does not require `SDL_Init`.

## Desktop / Lua / TUI

- **Mouse**: `desktop_mouse_event_handler`, `luade_mouse_event`, etc. (see `mouse.c`, `desktop_integration.c`).  
- **SDL bridge**: `src/sdl_integration.c` can inject SDL events when building the **host SDL** path (not the user-syscall path).

## SDL messages on serial (QEMU `-serial file:serial.log`)

Kernel SDL used to call only `printf()`, which goes through `term_write()`. When **`term_serial_suppress(true)`** is active (e.g. TUI), **serial output is skipped**, so you see **no `[SDL] ...` lines on COM1**.

**Fix:** `sdl_kernel_log()` in `src/sdl_kernel_log.c` sends once to **`serial_puts()`** (COM1), then to **`fb_console_write()`** only for the on-screen console. It does **not** call **`printf()`**/`term_write()`, because those would **duplicate** every line on serial when `term_serial_suppress` is false (`term_write` sends to both serial and framebuffer).

## Critical: IRQ + event queue lock

The event queue uses a **spinlock** (`event_queue_lock`). Spinlocks here **do not** implicitly mask interrupts. If `SDL_PollEvent` → `pop_event` holds the lock **with interrupts enabled**, a **keyboard IRQ** can run `SDL_InjectKeyEvent` → `push_event` and **block on the same spinlock** while the syscall still holds it → **deadlock** (no keys/mouse ever delivered).

**Fix:** `push_event` / `pop_event` (and queue reset in `SDL_EventsQuit`) wrap the critical section with **`irq_save()` / `irq_restore()`** from `atomic.h` so IRQs cannot preempt into the same lock on that CPU.

## Shared ABI: `include/sdl_event_types.h`

Kernel `include/SDL.h` includes **`sdl_event_types.h`**. User apps that receive events via **`SYS_SDL_POLL_EVENT`** should include the **same header** (as `herb_engine_indevos.c` does) so **`SDL_Event`** layout matches **`copy_to_user`**.

## Quick rebuild checklist

1. **`make`** (full kernel) after changing `SDL.h` / `sdl_event_types.h` / `syscall.c` / `keyboard.c` / `sdl_events.c`.  
2. **`./build_userapp.sh herb_engine_indevos.c herb.exe`** for the user ELF.  
3. If **`make`** does not rebuild `obj/sdl_events.o` after header-only edits, run **`touch src/sdl_events.c`** or **`rm obj/sdl_events.o && make`**.
