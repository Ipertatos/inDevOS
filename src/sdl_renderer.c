#include "../include/SDL.h"
#include "../include/sdl_kernel_log.h"
#include "profiler.h"
#include "render.h"
#include "ktime.h"
#include "mem.h"
#include "gpu_hal.h"
#include <stdint.h>
#include <stdbool.h>

static bool sdl_rt_from_surface(r_target_t *t, SDL_Surface *surface)
{
    if (!t || !surface || !surface->pixels || surface->w <= 0 || surface->h <= 0)
        return false;
    r_target_from_bytes(t, surface->pixels, surface->w, surface->h, surface->pitch);
    if (surface->clip_rect.w > 0 && surface->clip_rect.h > 0) {
        r_clip(t,
               surface->clip_rect.x,
               surface->clip_rect.y,
               surface->clip_rect.x + surface->clip_rect.w,
               surface->clip_rect.y + surface->clip_rect.h);
    }
    return true;
}

static uint32_t sdl_renderer_color(SDL_Renderer *renderer)
{
    SDL_Surface *surface = renderer->window->surface;
    return SDL_MapRGBA(surface->format,
                       renderer->draw_color.r,
                       renderer->draw_color.g,
                       renderer->draw_color.b,
                       renderer->draw_color.a);
}

SDL_Renderer* SDL_CreateRenderer(SDL_Window* window, int index, uint32_t flags) {
    (void)index;
    if (!window) {
        sdl_kernel_log("[SDL_Renderer] Error: Invalid window\n");
        return NULL;
    }

    SDL_Renderer* renderer = (SDL_Renderer*)kalloc(sizeof(SDL_Renderer));
    if (!renderer) {
        sdl_kernel_log("[SDL_Renderer] Error: Failed to allocate renderer\n");
        return NULL;
    }

    renderer->window = window;
    renderer->flags = flags;
    renderer->draw_color.r = 0;
    renderer->draw_color.g = 0;
    renderer->draw_color.b = 0;
    renderer->draw_color.a = 255;

    uint32_t caps = gpu_hal_get_capabilities();
    const char *accel = (caps & GPU_CAP_2D) ? "GPU" : "CPU";
    sdl_kernel_log("[SDL_Renderer] Created renderer for window %dx%d (%s via %s)\n",
                   window->w, window->h, accel, gpu_hal_get_backend_name());
    return renderer;
}

void SDL_DestroyRenderer(SDL_Renderer* renderer) {
    if (!renderer) return;
    kfree(renderer);
    sdl_kernel_log("[SDL_Renderer] Renderer destroyed\n");
}

int SDL_SetRenderDrawColor(SDL_Renderer* renderer, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (!renderer) {
        return SDL_ERROR;
    }
    renderer->draw_color.r = r;
    renderer->draw_color.g = g;
    renderer->draw_color.b = b;
    renderer->draw_color.a = a;
    return SDL_SUCCESS;
}

int SDL_RenderClear(SDL_Renderer* renderer) {
    if (!renderer || !renderer->window || !renderer->window->surface)
        return SDL_ERROR;

    r_target_t t;
    if (!sdl_rt_from_surface(&t, renderer->window->surface))
        return SDL_ERROR;
    r_clip_reset(&t);
    r_fill(&t, 0, 0, t.w, t.h, sdl_renderer_color(renderer));
    return SDL_SUCCESS;
}

int SDL_RenderDrawPoint(SDL_Renderer* renderer, int x, int y) {
    if (!renderer || !renderer->window || !renderer->window->surface)
        return SDL_ERROR;

    r_target_t t;
    if (!sdl_rt_from_surface(&t, renderer->window->surface))
        return SDL_ERROR;
    r_put(&t, x, y, sdl_renderer_color(renderer));
    return SDL_SUCCESS;
}

int SDL_RenderDrawLine(SDL_Renderer* renderer, int x1, int y1, int x2, int y2) {
    if (!renderer || !renderer->window || !renderer->window->surface)
        return SDL_ERROR;

    r_target_t t;
    if (!sdl_rt_from_surface(&t, renderer->window->surface))
        return SDL_ERROR;
    r_line(&t, x1, y1, x2, y2, sdl_renderer_color(renderer));
    return SDL_SUCCESS;
}

int SDL_RenderDrawRect(SDL_Renderer* renderer, const SDL_Rect* rect) {
    if (!renderer || !renderer->window || !renderer->window->surface)
        return SDL_ERROR;

    r_target_t t;
    if (!sdl_rt_from_surface(&t, renderer->window->surface))
        return SDL_ERROR;

    int x, y, w, h;
    if (rect) {
        x = rect->x; y = rect->y; w = rect->w; h = rect->h;
    } else {
        x = 0; y = 0; w = t.w; h = t.h;
    }
    r_rect(&t, x, y, w, h, sdl_renderer_color(renderer));
    return SDL_SUCCESS;
}

int SDL_RenderFillRect(SDL_Renderer* renderer, const SDL_Rect* rect) {
    if (!renderer || !renderer->window || !renderer->window->surface)
        return SDL_ERROR;

    r_target_t t;
    if (!sdl_rt_from_surface(&t, renderer->window->surface))
        return SDL_ERROR;

    int x, y, w, h;
    if (rect) {
        x = rect->x; y = rect->y; w = rect->w; h = rect->h;
    } else {
        x = 0; y = 0; w = t.w; h = t.h;
    }
    r_fill(&t, x, y, w, h, sdl_renderer_color(renderer));
    return SDL_SUCCESS;
}

void SDL_RenderPresent(SDL_Renderer* renderer) {
    if (!renderer || !renderer->window)
        return;

    profiler_on_frame_present((uint64_t)(uintptr_t)(void *)renderer->window);
    profiler_on_sdl_present(renderer->window);
    SDL_UpdateWindowSurface(renderer->window);
    /* Steady cap. On the ACPI PM timer this busy-waits; it does not use HPET or HLT. */
    ktime_pace_hz(35);
}
