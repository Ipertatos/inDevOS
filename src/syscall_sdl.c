#include "syscall_internal.h"
#include "string.h"
#include "mem.h"
#include "uaccess.h"
#include "keyboard.h"
#include "mouse.h"
#include "atomic.h"
#include "desktop.h"
#include "profiler.h"
#include "../include/SDL.h"
#include "../include/sdl_kernel_log.h"

#define SDL_KB_STATE_SIZE 1024
extern volatile uint8_t sdl_keyboard_state[SDL_KB_STATE_SIZE];
extern volatile int32_t sdl_mouse_accum_dx;
extern volatile int32_t sdl_mouse_accum_dy;

static bool current_sdl_process_has_focus(void) {
    if (!desktop_is_running())
        return true;
    process_t *proc = get_current_process();
    if (proc && desktop_pid_has_focus(proc->pid))
        return true;
    /* Allow keys unless a different Ring-3 window is focused.  Covers
     * windows created with a stale/zero pid and the no-focus case. */
    desktop_window_t *fw = desktop_get_focused();
    if (!fw || fw->pid == 0)
        return true;
    return proc && fw->pid == proc->pid;
}

// SDL syscall implementations
int sys_sdl_init(uint32_t flags) {
    int r = SDL_Init(flags);
    /*
     * Raw keyboard mode: keep Zeus/shell from eating typed characters while a
     * Ring-3 SDL app runs; PS/2 IRQ still calls SDL_InjectKeyEvent first.
     */
    if (r == 0 && (flags & 0x40u)) /* SDL_INIT_EVENTS */
        keyboard_set_raw_mode(true);
    return r;
}

/* forward decls — handle tables defined just below */
static void sdl_handles_clear_all(void);

void sys_sdl_quit(void) {
    SDL_Quit();
    /* SDL_Quit() frees every window; drop our handle tables so stale
     * pointers can't be replayed through later syscalls. */
    sdl_handles_clear_all();
    keyboard_set_raw_mode(false);
}

/*
 * SDL handle registry — user programs pass SDL window/renderer pointers
 * back through syscalls. They must never be dereferenced unless they match a
 * handle the kernel itself returned, otherwise a hostile program can make the
 * kernel read/write arbitrary memory.
 */
#define SDL_MAX_HANDLES 32
static void *sdl_window_handles[SDL_MAX_HANDLES];
static void *sdl_renderer_handles[SDL_MAX_HANDLES];

static bool sdl_handle_register(void **table, void *h) {
    if (!h) return false;
    for (int i = 0; i < SDL_MAX_HANDLES; i++) {
        if (table[i] == NULL) {
            table[i] = h;
            return true;
        }
    }
    return false;
}

static bool sdl_handle_unregister(void **table, void *h) {
    for (int i = 0; i < SDL_MAX_HANDLES; i++) {
        if (table[i] == h) {
            table[i] = NULL;
            return true;
        }
    }
    return false;
}

static bool sdl_handle_valid(void **table, void *h) {
    if (!h) return false;
    for (int i = 0; i < SDL_MAX_HANDLES; i++) {
        if (table[i] == h)
            return true;
    }
    return false;
}

#define sdl_window_valid(w)   sdl_handle_valid(sdl_window_handles, (void *)(w))
#define sdl_renderer_valid(r) sdl_handle_valid(sdl_renderer_handles, (void *)(r))

static void sdl_handles_clear_all(void) {
    for (int i = 0; i < SDL_MAX_HANDLES; i++) {
        if (sdl_renderer_handles[i]) {
            SDL_DestroyRenderer((SDL_Renderer *)sdl_renderer_handles[i]);
            sdl_renderer_handles[i] = NULL;
        }
        sdl_window_handles[i] = NULL;
    }
}

SDL_Window* sys_sdl_create_window(const char* title, int x, int y, int w, int h) {
    /* title is an unvalidated user pointer — copy it to a kernel buffer */
    char ktitle[128] = "app";
    if (title && is_user_address((uint64_t)title)) {
        syscall_uaccess_ensure_user_cr3();
        if (!strncpy_from_user(ktitle, title, sizeof(ktitle)))
            strcpy(ktitle, "app");
    }
    SDL_Window *win = SDL_CreateWindow(ktitle, x, y, w, h, SDL_WINDOW_SHOWN);
    if (win && !sdl_handle_register(sdl_window_handles, win)) {
        SDL_DestroyWindow(win);
        return NULL;
    }
    return win;
}

void sys_sdl_destroy_window(SDL_Window* window) {
    if (!sdl_handle_unregister(sdl_window_handles, window))
        return;
    SDL_DestroyWindow(window);
}

SDL_Renderer* sys_sdl_create_renderer(SDL_Window* window, int index, uint32_t flags) {
    if (!sdl_window_valid(window))
        return NULL;
    SDL_Renderer *r = SDL_CreateRenderer(window, index, flags);
    if (r && !sdl_handle_register(sdl_renderer_handles, r)) {
        SDL_DestroyRenderer(r);
        return NULL;
    }
    return r;
}

void sys_sdl_destroy_renderer(SDL_Renderer* renderer) {
    if (!sdl_handle_unregister(sdl_renderer_handles, renderer))
        return;
    SDL_DestroyRenderer(renderer);
}

int sys_sdl_set_render_draw_color(SDL_Renderer* renderer, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (!sdl_renderer_valid(renderer)) return -1;
    return SDL_SetRenderDrawColor(renderer, r, g, b, a);
}

int sys_sdl_render_clear(SDL_Renderer* renderer) {
    if (!sdl_renderer_valid(renderer)) return -1;
    return SDL_RenderClear(renderer);
}

void sys_sdl_render_present(SDL_Renderer* renderer) {
    if (!sdl_renderer_valid(renderer)) return;
    SDL_RenderPresent(renderer);
}

int sys_sdl_get_keyboard_state(void *user_buf, uint64_t n) {
    static bool sdl_io_logged;
    if (!user_buf || n == 0)
        return -1;
    if (n > SDL_KB_STATE_SIZE)
        n = SDL_KB_STATE_SIZE;
    if (!access_ok_user_range((uint64_t)user_buf, (size_t)n))
        return -1;
    syscall_uaccess_ensure_user_cr3();
    if (!sdl_io_logged) {
        sdl_io_logged = true;
        sdl_kernel_log("[SDL] direct I/O: first GET_KEYBOARD_STATE syscall\n");
    }
    bool block_input = !current_sdl_process_has_focus();
    /*
     * Keyboard bitmap is updated from IRQ context (possibly another CPU).
     * Fence + snapshot so copy_to_user sees a coherent view and memcpy isn't
     * confused about aliasing with concurrent IRQ writers.
     */
    __sync_synchronize();
    uint8_t snap[SDL_KB_STATE_SIZE];
    size_t copy_len = (size_t)n;
    if (block_input) {
        memset(snap, 0, copy_len);
    } else {
        for (size_t i = 0; i < copy_len; i++)
            snap[i] = sdl_keyboard_state[i];
    }
    __sync_synchronize();
    return copy_to_user(user_buf, snap, copy_len) ? -1 : 0;
}

uint64_t sys_sdl_get_mouse_rel_accum(void) {
    uint64_t f = irq_save();
    int32_t dx = (int32_t)sdl_mouse_accum_dx;
    int32_t dy = (int32_t)sdl_mouse_accum_dy;
    bool block = !current_sdl_process_has_focus();
    if (block) {
        dx = 0;
        dy = 0;
    }
    sdl_mouse_accum_dx = 0;
    sdl_mouse_accum_dy = 0;
    irq_restore(f);
    return ((uint64_t)(uint32_t)dx << 32) | ((uint32_t)dy);
}

uint64_t sys_sdl_get_mouse_buttons_u64(void) {
    if (!current_sdl_process_has_focus())
        return 0;
    return (uint64_t)mouse_get_buttons() & 0xFFu;
}

int sys_sdl_poll_event(void *user_event) {
    if (!user_event || !access_ok_user_range((uint64_t)user_event, sizeof(SDL_Event)))
        return 0; /* like SDL_PollEvent: 0 = no event / invalid (not -1, avoids user busy-loops) */
    syscall_uaccess_ensure_user_cr3();
    if (!current_sdl_process_has_focus())
        return 0;
    SDL_Event kev;
    int ret = SDL_PollEvent(&kev);
    if (ret) {
        if (copy_to_user(user_event, &kev, sizeof(SDL_Event)) != 0)
            return 0; /* drop event on fault — avoid misleading return */
    }
    return ret;
}

uint32_t sys_sdl_get_ticks(void) {
    return SDL_GetTicks();
}

void sys_sdl_delay(uint32_t ms) {
    SDL_Delay(ms);
}

int sys_sdl_render_draw_rect(SDL_Renderer* renderer, const SDL_Rect* rect) {
    if (!sdl_renderer_valid(renderer)) return -1;
    SDL_Rect krect;
    if (rect) {
        syscall_uaccess_ensure_user_cr3();
        if (copy_from_user(&krect, rect, sizeof(krect)) != 0)
            return -1;
        return SDL_RenderDrawRect(renderer, &krect);
    }
    return SDL_RenderDrawRect(renderer, NULL);
}

int sys_sdl_render_fill_rect(SDL_Renderer* renderer, const SDL_Rect* rect) {
    if (!sdl_renderer_valid(renderer)) return -1;
    SDL_Rect krect;
    if (rect) {
        syscall_uaccess_ensure_user_cr3();
        if (copy_from_user(&krect, rect, sizeof(krect)) != 0)
            return -1;
        return SDL_RenderFillRect(renderer, &krect);
    }
    return SDL_RenderFillRect(renderer, NULL);
}

const char* sys_sdl_get_error(void) {
    return SDL_GetError();
}

int sys_sdl_copy_pixels(SDL_Renderer *renderer, const void *user_pixels, uint32_t width, uint32_t height) {
    if (!sdl_renderer_valid(renderer))
        return -1;
    if (!renderer->window || !renderer->window->surface || !user_pixels)
        return -1;
    SDL_Surface *surf = renderer->window->surface;
    if (width == 0 || height == 0)
        return -1;
    if ((int)width != surf->w || (int)height != surf->h)
        return -1;
    size_t row_bytes = (size_t)width * 4u;
    syscall_uaccess_ensure_user_cr3();
    if (surf->pitch == (int)row_bytes) {
        size_t nbytes = row_bytes * (size_t)height;
        if (copy_from_user(surf->pixels, user_pixels, nbytes) != 0)
            return -1;
    } else {
        for (uint32_t y = 0; y < height; y++) {
            void *dst = (uint8_t *)surf->pixels + (size_t)y * (size_t)surf->pitch;
            const void *src = (const uint8_t *)user_pixels + (size_t)y * row_bytes;
            if (copy_from_user(dst, src, row_bytes) != 0)
                return -1;
        }
    }

    /* Mark the desktop window dirty so the compositor repaints */
    if (desktop_is_running()) {
        process_t *proc = get_current_process();
        if (proc) {
            desktop_window_t *dw = desktop_find_window_by_pid(proc->pid);
            if (dw) dw->dirty = true;
        }
    }
    return 0;
}

int sys_desktop_embed_info(void *user_info) {
    process_t *proc = get_current_process();
    uint32_t pid = proc ? proc->pid : 0;
    return desktop_sys_embed_info(pid, user_info);
}

int sys_desktop_composite(const void *user_pixels, uint32_t w, uint32_t h) {
    process_t *proc = get_current_process();
    uint32_t pid = proc ? proc->pid : 0;
    /* Count this as a presented frame: apps that render through the desktop
     * compositor (DOOM) never call SDL_RenderPresent, so 'prof stats' used to
     * report frames=0 for them. */
    profiler_on_frame_present((uint64_t)pid);
    return desktop_sys_composite(pid, user_pixels, w, h);
}

