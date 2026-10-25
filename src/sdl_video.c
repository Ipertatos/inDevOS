#include "../include/SDL.h"
#include "../include/sdl_kernel_log.h"
#include "utils.h"
#include "mem.h"
#include "term.h"
#include "gpu_hal.h"
#include "desktop.h"
#include "render.h"
#include "syscall.h"
#include <stdint.h>
#include <stdbool.h>
#include "string.h"

extern struct limine_framebuffer *fb;
extern void putpixel(uint64_t x, uint64_t y, uint32_t color);
extern void fillrect(uint32_t color, uint64_t x, uint64_t y, uint64_t w, uint64_t h);
extern uint32_t fb_get_width(void);
extern uint32_t fb_get_height(void);

/* Pixels point into limine framebuffer; do not kfree(surface->pixels). */
#define SDL_SURFACE_FLAG_FB_ALIAS 0x80000000u

/*
 * Copy a region from the Limine FB to the HAL primary surface so the
 * virtio-gpu (or other non-Limine) scanout stays in sync.  No-op when
 * the HAL backend IS the Limine FB.
 */
static void sdl_sync_to_hal(int x, int y, int w, int h)
{
    gpu_surface_t *hal = gpu_hal_get_primary_surface();
    if (!hal || !hal->pixels || !fb || !fb->address)
        return;
    if (hal->pixels == (uint32_t *)fb->address)
        return;

    int hal_w = (int)hal->width;
    int hal_h = (int)hal->height;
    int fb_w  = (int)fb->width;
    int fb_h  = (int)fb->height;

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > fb_w)  w = fb_w  - x;
    if (x + w > hal_w) w = hal_w - x;
    if (y + h > fb_h)  h = fb_h  - y;
    if (y + h > hal_h) h = hal_h - y;
    if (w <= 0 || h <= 0) return;

    const uint8_t *src_base = (const uint8_t *)fb->address;
    uint8_t       *dst_base = (uint8_t *)hal->pixels;
    size_t row_bytes = (size_t)w * 4u;

    for (int row = 0; row < h; row++) {
        const uint8_t *src = src_base + (size_t)(y + row) * (size_t)fb->pitch + (size_t)x * 4u;
        uint8_t       *dst = dst_base + (size_t)(y + row) * (size_t)hal->pitch + (size_t)x * 4u;
        memcpy(dst, src, row_bytes);
    }
}

// SDL Video subsystem state
static bool video_initialized = false;
static SDL_Window* window_list = NULL;
static uint32_t next_window_id = 1;

static SDL_Surface *sdl_create_surface_framebuffer_view(int win_x, int win_y, int width, int height);

// Default pixel format (32-bit RGBA)
static SDL_PixelFormat default_format = {
    .format = SDL_PIXELFORMAT_RGBA8888,
    .BitsPerPixel = 32,
    .BytesPerPixel = 4,
    .Rmask = 0xFF000000,
    .Gmask = 0x00FF0000,
    .Bmask = 0x0000FF00,
    .Amask = 0x000000FF,
    .Rshift = 24,
    .Gshift = 16,
    .Bshift = 8,
    .Ashift = 0
};

int SDL_VideoInit(void) {
    if (video_initialized) {
        return SDL_SUCCESS;
    }

    gpu_surface_t *hal = gpu_hal_get_primary_surface();
    if (hal && hal->pixels) {
        sdl_kernel_log("[SDL_Video] Initializing video subsystem\n");
        sdl_kernel_log("[SDL_Video] HAL backend: %s  %dx%d %d bpp\n",
                       gpu_hal_get_backend_name(),
                       (int)hal->width, (int)hal->height, (int)hal->bpp);
        video_initialized = true;
        return SDL_SUCCESS;
    }

    if (!fb) {
        sdl_kernel_log("[SDL_Video] Error: No framebuffer available\n");
        return SDL_ERROR;
    }

    sdl_kernel_log("[SDL_Video] Initializing video subsystem\n");
    sdl_kernel_log("[SDL_Video] Framebuffer (legacy): %dx%d, %d bpp\n",
                   (int)fb->width, (int)fb->height, (int)fb->bpp);

    video_initialized = true;
    return SDL_SUCCESS;
}

// Quit SDL video subsystem
void SDL_VideoQuit(void) {
    if (!video_initialized) {
        return;
    }
    
    // Clean up any remaining windows
    while (window_list) {
        SDL_Window* w = window_list;
        window_list = w->next;
        w->next = NULL;
        SDL_DestroyWindow(w);
    }
    
    video_initialized = false;
    sdl_kernel_log("[SDL_Video] Video subsystem shut down\n");
}

// Create a window
SDL_Window* SDL_CreateWindow(const char* title, int x, int y, int w, int h, uint32_t flags) {
    if (!video_initialized) {
        sdl_kernel_log("[SDL_Video] Error: Video not initialized\n");
        return NULL;
    }
    
    SDL_Window* window = (SDL_Window*)kalloc(sizeof(SDL_Window));
    if (!window) {
        sdl_kernel_log("[SDL_Video] Error: Failed to allocate window\n");
        return NULL;
    }
    memset(window, 0, sizeof(SDL_Window));
    
    int fb_width  = (int)fb_get_width();
    int fb_height = (int)fb_get_height();
    
    if (w <= 0 || w > fb_width) w = fb_width;
    if (h <= 0 || h > fb_height) h = fb_height;
    
    window->id = next_window_id++;
    window->x = (x < 0) ? 0 : x;
    window->y = (y < 0) ? 0 : y;
    window->w = w;
    window->h = h;
    window->flags = flags;

    /* When the desktop compositor is running, ALL windows use off-screen
     * surfaces and the compositor handles presentation.  Register the
     * window with the desktop so it gets a slot in the Z-order. */
    if (desktop_is_running()) {
        window->surface = SDL_CreateRGBSurface(0, w, h, 32, default_format.Rmask,
                                               default_format.Gmask, default_format.Bmask,
                                               default_format.Amask);
        if (window->surface) {
            process_t *proc = get_current_process();
            uint32_t pid = proc ? proc->pid : 0;
            desktop_window_t *dw = desktop_create_window(
                pid, (x < 0) ? 50 : x, (y < 0) ? 50 : y, w, h,
                title ? title : "Window");
            if (dw && window->surface->pixels) {
                /* Share the SDL surface buffer with the compositor */
                if (dw->pixels) kfree(dw->pixels);
                dw->pixels      = (uint32_t *)window->surface->pixels;
                dw->pixel_pitch = (uint32_t)window->surface->pitch;
            }
        }
    } else {
        /* Legacy path: first window may use framebuffer alias */
        if (!window_list) {
            window->surface = sdl_create_surface_framebuffer_view(window->x, window->y, w, h);
        }
        if (!window->surface) {
            window->surface = SDL_CreateRGBSurface(0, w, h, 32, default_format.Rmask,
                                                   default_format.Gmask, default_format.Bmask,
                                                   default_format.Amask);
        }
    }

    if (!window->surface) {
        kfree(window);
        sdl_kernel_log("[SDL_Video] Error: Failed to create window surface\n");
        return NULL;
    }

    window->next = window_list;
    window_list = window;
    sdl_kernel_log("[SDL_Video] Created window: %dx%d at (%d,%d)%s\n",
                   w, h, window->x, window->y,
                   desktop_is_running() ? " [desktop]" : "");

    if (desktop_is_running())
        desktop_present_now();
    
    return window;
}

static SDL_Surface *sdl_create_surface_framebuffer_view(int win_x, int win_y, int width, int height) {
    if (!fb || !fb->address || fb->bpp != 32 || width <= 0 || height <= 0)
        return NULL;
    if (win_x < 0 || win_y < 0)
        return NULL;
    if (win_x + width > (int)fb->width || win_y + height > (int)fb->height)
        return NULL;
    if ((int)fb->pitch < width * 4)
        return NULL;

    SDL_Surface *surface = (SDL_Surface *)kalloc(sizeof(SDL_Surface));
    if (!surface)
        return NULL;
    surface->format = (SDL_PixelFormat *)kalloc(sizeof(SDL_PixelFormat));
    if (!surface->format) {
        kfree(surface);
        return NULL;
    }
    *(surface->format) = default_format;

    surface->flags = SDL_SURFACE_FLAG_FB_ALIAS;
    surface->w = width;
    surface->h = height;
    surface->pitch = (int)fb->pitch;
    surface->locked = 0;
    surface->refcount = 1;
    surface->clip_rect.x = 0;
    surface->clip_rect.y = 0;
    surface->clip_rect.w = width;
    surface->clip_rect.h = height;
    surface->pixels = (uint8_t *)fb->address + (size_t)win_y * fb->pitch + (size_t)win_x * 4u;

    uint8_t *row = surface->pixels;
    for (int y = 0; y < height; y++) {
        memset(row + (size_t)y * (size_t)surface->pitch, 0, (size_t)width * 4u);
    }

    sdl_kernel_log("[SDL_Video] FB-alias surface %dx%d at (%d,%d) pitch=%u (HAL: %s)\n",
                   width, height, win_x, win_y, (unsigned)fb->pitch,
                   gpu_hal_get_backend_name());
    return surface;
}

/* Drop an SDL window whose surface shares this pixel buffer.
 * Frees the surface (and therefore the pixels). Does not touch the desktop
 * window: the caller is closing from whichever thread owns the WM, and
 * SDL_DestroyWindow() looks up the desktop window via get_current_process(),
 * which is wrong when the shell/kernel thread handles the click.
 * Returns true if a matching SDL window was released. */
bool sdl_destroy_window_sharing_pixels(uint32_t *pixels) {
    if (!pixels)
        return false;
    SDL_Window **pp = &window_list;
    while (*pp) {
        SDL_Window *window = *pp;
        if (window->surface && window->surface->pixels == (void *)pixels) {
            *pp = window->next;
            window->next = NULL;
            /* Drop the syscall handle and any renderer bound to this window
             * so a stale pointer can never validate again. */
            extern void sys_sdl_forget_window(void *window);
            sys_sdl_forget_window(window);
            SDL_FreeSurface(window->surface);
            window->surface = NULL;
            kfree(window);
            sdl_kernel_log("[SDL_Video] Closed window sharing desktop pixels\n");
            return true;
        }
        pp = &(*pp)->next;
    }
    return false;
}

static void *surface_pixels_of(SDL_Window *w) {
    return (w && w->surface) ? w->surface->pixels : NULL;
}

// Destroy a window
void SDL_DestroyWindow(SDL_Window* window) {
    if (!window) return;

    if (desktop_is_running()) {
        process_t *proc = get_current_process();
        desktop_window_t *dw = window->surface
            ? desktop_find_window_by_pixels(window->surface->pixels) : NULL;
        if (!dw && proc) dw = desktop_find_window_by_pid(proc->pid);
        if (dw) {
            if (window->surface && dw->pixels == (uint32_t *)window->surface->pixels)
                dw->pixels = NULL; /* SDL_FreeSurface owns this buffer. */
            desktop_destroy_window(dw);
            desktop_present_now();
        }
    }

    if (window->surface) {
        SDL_FreeSurface(window->surface);
    }

    SDL_Window** pp = &window_list;
    while (*pp) {
        if (*pp == window) {
            *pp = window->next;
            break;
        }
        pp = &(*pp)->next;
    }
    window->next = NULL;
    
    kfree(window);
    sdl_kernel_log("[SDL_Video] Window destroyed\n");
}

// Get window surface
SDL_Surface* SDL_GetWindowSurface(SDL_Window* window) {
    if (!window) return NULL;
    return window->surface;
}

int SDL_UpdateWindowSurface(SDL_Window* window) {
    if (!window || !window->surface)
        return SDL_ERROR;

    /* When the desktop compositor is running, the client surface is already
     * shared with the desktop window.  Mark it dirty and let the compositor
     * handle presentation — no direct FB writes. */
    if (desktop_is_running()) {
        process_t *proc = get_current_process();
        if (proc) {
            desktop_window_t *dw = desktop_find_window_by_pixels(surface_pixels_of(window));
            if (!dw) dw = desktop_find_window_by_pid(proc->pid);
            if (dw) dw->dirty = true;
            if (desktop_present_window_now(proc->pid))
                return SDL_SUCCESS;
        }
        desktop_present_now();
        return SDL_SUCCESS;
    }

    SDL_Surface* surface = window->surface;

    if (surface->flags & SDL_SURFACE_FLAG_FB_ALIAS) {
        sdl_sync_to_hal(window->x, window->y, surface->w, surface->h);
        gpu_hal_present_rect((uint32_t)window->x, (uint32_t)window->y,
                             (uint32_t)surface->w, (uint32_t)surface->h);
        return SDL_SUCCESS;
    }

    int dst_x = window->x;
    int dst_y = window->y;
    int w = surface->w;
    int h = surface->h;
    int src_pitch = surface->pitch;
    const uint8_t *src_base = (const uint8_t *)surface->pixels;

    if (w <= 0 || h <= 0 || !src_base)
        return SDL_ERROR;

    if (!fb || !fb->address || fb->bpp != 32)
        return SDL_ERROR;

    void  *dst_buf   = fb->address;
    int    dst_pitch = (int)fb->pitch;
    int    dst_bpp   = (int)fb->bpp;
    int    fbw       = (int)fb_get_width();
    int    fbh       = (int)fb_get_height();

    int src_x = 0;
    int src_y = 0;
    if (dst_x < 0) { src_x = -dst_x; w += dst_x; dst_x = 0; }
    if (dst_y < 0) { src_y = -dst_y; h += dst_y; dst_y = 0; }
    if (w <= 0 || h <= 0) return SDL_SUCCESS;
    if (dst_x + w > fbw) w = fbw - dst_x;
    if (dst_y + h > fbh) h = fbh - dst_y;
    if (w <= 0 || h <= 0) return SDL_SUCCESS;

    if (dst_bpp == 32 && surface->format && surface->format->BytesPerPixel == 4) {
        uint8_t *dst_base = (uint8_t *)dst_buf;
        size_t row_bytes = (size_t)w * 4u;
        for (int y = 0; y < h; y++) {
            const uint8_t *src_row = src_base + (size_t)(src_y + y) * (size_t)src_pitch +
                                     (size_t)src_x * 4u;
            uint8_t *dst_row =
                dst_base + (size_t)(dst_y + y) * (size_t)dst_pitch + (size_t)dst_x * 4u;
            memcpy(dst_row, src_row, row_bytes);
        }
        sdl_sync_to_hal(dst_x, dst_y, w, h);
        gpu_hal_present_rect((uint32_t)dst_x, (uint32_t)dst_y,
                             (uint32_t)w, (uint32_t)h);
        return SDL_SUCCESS;
    }

    {
        uint32_t *pixels = (uint32_t *)surface->pixels;
        int spp = surface->pitch / 4;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                uint32_t pixel = pixels[(src_y + y) * spp + (src_x + x)];
                putpixel((uint64_t)(dst_x + x), (uint64_t)(dst_y + y), pixel);
            }
        }
        gpu_hal_present_rect((uint32_t)dst_x, (uint32_t)dst_y,
                             (uint32_t)w, (uint32_t)h);
    }

    return SDL_SUCCESS;
}

// Get window size
void SDL_GetWindowSize(SDL_Window* window, int* w, int* h) {
    if (!window) {
        if (w) *w = 0;
        if (h) *h = 0;
        return;
    }
    
    if (w) *w = window->w;
    if (h) *h = window->h;
}

// Create RGB surface
SDL_Surface* SDL_CreateRGBSurface(uint32_t flags, int width, int height, int depth,
                                  uint32_t Rmask, uint32_t Gmask, uint32_t Bmask, uint32_t Amask) {
    if (width <= 0 || height <= 0) {
        sdl_kernel_log("[SDL_Video] Error: Invalid surface dimensions\n");
        return NULL;
    }
    
    SDL_Surface* surface = (SDL_Surface*)kalloc(sizeof(SDL_Surface));
    if (!surface) {
        sdl_kernel_log("[SDL_Video] Error: Failed to allocate surface\n");
        return NULL;
    }
    
    // Create pixel format
    surface->format = (SDL_PixelFormat*)kalloc(sizeof(SDL_PixelFormat));
    if (!surface->format) {
        kfree(surface);
        sdl_kernel_log("[SDL_Video] Error: Failed to allocate pixel format\n");
        return NULL;
    }
    
    // Set up pixel format (force 32-bit for simplicity)
    *(surface->format) = default_format;
    
    surface->flags = flags;
    surface->w = width;
    surface->h = height;
    surface->pitch = width * 4; // 4 bytes per pixel (32-bit)
    surface->locked = 0;
    surface->refcount = 1;
    
    // Set clip rectangle to entire surface
    surface->clip_rect.x = 0;
    surface->clip_rect.y = 0;
    surface->clip_rect.w = width;
    surface->clip_rect.h = height;
    
    // Allocate pixel buffer
    size_t pixel_buffer_size = surface->pitch * height;
    surface->pixels = kalloc(pixel_buffer_size);
    if (!surface->pixels) {
        kfree(surface->format);
        kfree(surface);
        sdl_kernel_log("[SDL_Video] Error: Failed to allocate pixel buffer\n");
        return NULL;
    }
    
    memset(surface->pixels, 0, pixel_buffer_size);
    
    sdl_kernel_log("[SDL_Video] Created surface: %dx%d\n", width, height);
    return surface;
}

// Free surface
void SDL_FreeSurface(SDL_Surface* surface) {
    if (!surface) return;
    
    surface->refcount--;
    if (surface->refcount > 0) return;
    
    if (surface->pixels && !(surface->flags & SDL_SURFACE_FLAG_FB_ALIAS)) {
        kfree(surface->pixels);
    }
    if (surface->format) {
        kfree(surface->format);
    }
    
    kfree(surface);
}

// Fill rectangle on surface
int SDL_FillRect(SDL_Surface* dst, const SDL_Rect* rect, uint32_t color) {
    if (!dst || !dst->pixels) {
        return SDL_ERROR;
    }

    r_target_t t;
    r_target_from_bytes(&t, dst->pixels, dst->w, dst->h, dst->pitch);
    if (rect)
        r_fill(&t, rect->x, rect->y, rect->w, rect->h, color);
    else
        r_fill(&t, 0, 0, dst->w, dst->h, color);
    return SDL_SUCCESS;
}

// Blit surface to surface
int SDL_BlitSurface(SDL_Surface* src, const SDL_Rect* srcrect, SDL_Surface* dst, SDL_Rect* dstrect) {
    if (!src || !dst || !src->pixels || !dst->pixels) {
        return SDL_ERROR;
    }
    
    SDL_Rect src_rect, dst_rect;
    
    // Set up source rectangle
    if (srcrect) {
        src_rect = *srcrect;
    } else {
        src_rect.x = 0;
        src_rect.y = 0;
        src_rect.w = src->w;
        src_rect.h = src->h;
    }
    
    // Set up destination rectangle
    if (dstrect) {
        dst_rect.x = dstrect->x;
        dst_rect.y = dstrect->y;
        dst_rect.w = src_rect.w;
        dst_rect.h = src_rect.h;
    } else {
        dst_rect.x = 0;
        dst_rect.y = 0;
        dst_rect.w = src_rect.w;
        dst_rect.h = src_rect.h;
    }
    
    // Simple clipping
    if (dst_rect.x < 0) {
        src_rect.x -= dst_rect.x;
        src_rect.w += dst_rect.x;
        dst_rect.w += dst_rect.x;
        dst_rect.x = 0;
    }
    if (dst_rect.y < 0) {
        src_rect.y -= dst_rect.y;
        src_rect.h += dst_rect.y;
        dst_rect.h += dst_rect.y;
        dst_rect.y = 0;
    }
    
    if (dst_rect.x + dst_rect.w > dst->w) {
        int overflow = (dst_rect.x + dst_rect.w) - dst->w;
        dst_rect.w -= overflow;
        src_rect.w -= overflow;
    }
    if (dst_rect.y + dst_rect.h > dst->h) {
        int overflow = (dst_rect.y + dst_rect.h) - dst->h;
        dst_rect.h -= overflow;
        src_rect.h -= overflow;
    }
    
    if (src_rect.w <= 0 || src_rect.h <= 0 || dst_rect.w <= 0 || dst_rect.h <= 0) {
        return SDL_SUCCESS; // Nothing to blit
    }
    
    uint32_t* src_pixels = (uint32_t*)src->pixels;
    uint32_t* dst_pixels = (uint32_t*)dst->pixels;
    int src_pixels_per_line = src->pitch / 4;
    int dst_pixels_per_line = dst->pitch / 4;
    int row_w = dst_rect.w;

    for (int y = 0; y < dst_rect.h; y++) {
        uint32_t *dst_row = dst_pixels + (dst_rect.y + y) * dst_pixels_per_line + dst_rect.x;
        uint32_t *src_row = src_pixels + (src_rect.y + y) * src_pixels_per_line + src_rect.x;
        memcpy(dst_row, src_row, (size_t)row_w * sizeof(uint32_t));
    }
    
    // Update dstrect if provided
    if (dstrect) {
        dstrect->x = dst_rect.x;
        dstrect->y = dst_rect.y;
        dstrect->w = dst_rect.w;
        dstrect->h = dst_rect.h;
    }
    
    return SDL_SUCCESS;
}

// Map RGB to pixel value
uint32_t SDL_MapRGB(const SDL_PixelFormat* format, uint8_t r, uint8_t g, uint8_t b) {
    if (!format) return 0;
    
    return ((uint32_t)r << format->Rshift) |
           ((uint32_t)g << format->Gshift) |
           ((uint32_t)b << format->Bshift) |
           (0xFF << format->Ashift); // Full alpha
}

// Map RGBA to pixel value
uint32_t SDL_MapRGBA(const SDL_PixelFormat* format, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (!format) return 0;
    
    return ((uint32_t)r << format->Rshift) |
           ((uint32_t)g << format->Gshift) |
           ((uint32_t)b << format->Bshift) |
           ((uint32_t)a << format->Ashift);
}

// Get pixel from surface
uint32_t SDL_GetPixel(SDL_Surface* surface, int x, int y) {
    if (!surface || !surface->pixels || x < 0 || y < 0 || x >= surface->w || y >= surface->h) {
        return 0;
    }
    
    uint32_t* pixels = (uint32_t*)surface->pixels;
    return pixels[y * (surface->pitch / 4) + x];
}

// Put pixel on surface
void SDL_PutPixel(SDL_Surface* surface, int x, int y, uint32_t pixel) {
    if (!surface || !surface->pixels || x < 0 || y < 0 || x >= surface->w || y >= surface->h) {
        return;
    }
    
    uint32_t* pixels = (uint32_t*)surface->pixels;
    pixels[y * (surface->pitch / 4) + x] = pixel;
}




