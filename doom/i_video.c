// Emacs style mode select   -*- C++ -*- 
//-----------------------------------------------------------------------------
//
// Copyright (C) 1993-1996 by id Software, Inc.
// inDevOS port by inDevOS team
//
// This source is available for distribution and/or modification
// only under the terms of the DOOM Source Code License as
// published by id Software. All rights reserved.
//
// DESCRIPTION:
//      inDevOS video: SDL syscalls (same path as sdl_user_demo / kernel SDL)
//
//-----------------------------------------------------------------------------

/* Before any glibc stdio/stdlib headers — syscalls.h defines static inline wrappers */
#define INDEVOS_NO_BUILTIN_MALLOC 1
#include "../include/syscalls.h"
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "doomstat.h"
#include "i_system.h"
#include "v_video.h"
#include "m_argv.h"
#include "d_main.h"
#include "doomdef.h"
#include "d_event.h"

// DOOM screen is 320x200
#define DOOM_WIDTH  320
#define DOOM_HEIGHT 200
/* Window size only — pixels are sent native 320x200; the kernel scales. */
#define DOOM_MAX_SCALE 2

// Match kernel SDL.h / sdl_user_demo
#define SDL_INIT_VIDEO   0x00000020u
#define SDL_INIT_EVENTS  0x00000040u
#define SDL_RENDERER_SOFTWARE 0x00000001u

/* SDL_Keycode values (include/SDL.h) — keep in sync for sdl_keyboard_state[] */
#define SDLK_ESCAPE    0x1Bu
#define SDLK_RETURN    '\r'
#define SDLK_TAB       '\t'
#define SDLK_BACKSPACE '\b'
#define SDLK_SPACE     ' '
#define SDLK_UP        0x100
#define SDLK_DOWN      0x101
#define SDLK_LEFT      0x102
#define SDLK_RIGHT     0x103
#define SDLK_LSHIFT    0x200
#define SDLK_RSHIFT    0x201
#define SDLK_LCTRL     0x202
#define SDLK_RCTRL     0x203
#define SDLK_LALT      0x204
#define SDLK_RALT      0x205
#define SDLK_MINUS     '-'
#define SDLK_EQUALS    '='
#define SDLK_COMMA     ','
#define SDLK_PERIOD    '.'
#define SDLK_SLASH     '/'

// Scaling factor and display geometry (computed at init from framebuffer size)
static int scale;
static int disp_w, disp_h;     // full framebuffer size (= SDL window)
static int view_w, view_h;     // scaled doom area within the window
static int off_x, off_y;       // top-left offset to center the view
static int use_desktop_composite = -1; /* lazy: 1=composite syscall, 0=SDL */

// RGBA pixels for kernel surface (SDL_MapRGBA byte order in uint32)
static uint32_t palette[256];

static void *sdl_window;
static void *sdl_renderer;
static uint32_t *rgba_native;  /* 320x200 RGBA, kernel integer-scales */

static uint8_t prev_kb[1024];
static int sdl_video_ready;
static int g_desktop_embed;

unsigned char *doom_screen = NULL;

void I_ShutdownGraphics(void);

// DOOM key codes (from doomdef.h / doomkeys.h)
#define KEY_RIGHTARROW  0xae
#define KEY_LEFTARROW   0xac
#define KEY_UPARROW     0xad
#define KEY_DOWNARROW   0xaf
#define KEY_ESCAPE      27
#define KEY_ENTER       13
#define KEY_TAB         9
#define KEY_PAUSE       0xff
#define KEY_EQUALS      0x3d
#define KEY_MINUS       0x2d
#define KEY_RSHIFT      (0x80+0x36)
#define KEY_RCTRL       (0x80+0x1d)
#define KEY_RALT        (0x80+0x38)
#define KEY_LALT        KEY_RALT
#define KEY_CAPSLOCK    (0x80+0x3a)

extern void D_PostEvent(event_t *ev);

/* Desktop compositor treats the high byte as opacity. */
static inline uint32_t doom_rgb_to_pixel(unsigned r, unsigned g, unsigned b)
{
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static const struct { int sym; int doom; } doom_key_pairs[] = {
    { SDLK_ESCAPE, KEY_ESCAPE },
    { SDLK_RETURN, KEY_ENTER },
    { SDLK_TAB, KEY_TAB },
    { SDLK_BACKSPACE, KEY_BACKSPACE },
    { SDLK_SPACE, ' ' },
    { SDLK_UP, KEY_UPARROW },
    { SDLK_DOWN, KEY_DOWNARROW },
    { SDLK_LEFT, KEY_LEFTARROW },
    { SDLK_RIGHT, KEY_RIGHTARROW },
    { SDLK_LSHIFT, KEY_RSHIFT },
    { SDLK_RSHIFT, KEY_RSHIFT },
    { SDLK_LCTRL, KEY_RCTRL },
    { SDLK_RCTRL, KEY_RCTRL },
    { SDLK_LALT, KEY_RALT },
    { SDLK_RALT, KEY_RALT },
    { SDLK_MINUS, KEY_MINUS },
    { SDLK_EQUALS, KEY_EQUALS },
    { ',', ',' },
    { '.', '.' },
    { SDLK_SLASH, '/' },
    { '0', '0' }, { '1', '1' }, { '2', '2' }, { '3', '3' }, { '4', '4' },
    { '5', '5' }, { '6', '6' }, { '7', '7' }, { '8', '8' }, { '9', '9' },
    { 'a', 'a' }, { 'b', 'b' }, { 'c', 'c' }, { 'd', 'd' }, { 'e', 'e' },
    { 'f', 'f' }, { 'g', 'g' }, { 'h', 'h' }, { 'i', 'i' }, { 'j', 'j' },
    { 'k', 'k' }, { 'l', 'l' }, { 'm', 'm' }, { 'n', 'n' }, { 'o', 'o' },
    { 'p', 'p' }, { 'q', 'q' }, { 'r', 'r' }, { 's', 's' }, { 't', 't' },
    { 'u', 'u' }, { 'v', 'v' }, { 'w', 'w' }, { 'x', 'x' }, { 'y', 'y' },
    { 'z', 'z' },
};

static void post_key(int down, int doom_key)
{
    event_t ev;
    ev.type = down ? ev_keydown : ev_keyup;
    ev.data1 = doom_key;
    ev.data2 = 0;
    ev.data3 = 0;
    D_PostEvent(&ev);
}

static void poll_sdl_input(void)
{
    uint8_t cur[1024];
    unsigned i;

    if (!sdl_video_ready)
        return;

    if (indevos_get_keyboard_state(cur, sizeof(cur)) != 0)
        return;

    if ((cur[SDLK_LCTRL] || cur[SDLK_RCTRL]) && cur['c']) {
        I_ShutdownGraphics();
        print("DOOM: terminated by Ctrl+C\n");
        exit(0);
    }

    for (i = 0; i < sizeof(doom_key_pairs) / sizeof(doom_key_pairs[0]); i++) {
        int sym = doom_key_pairs[i].sym;
        int dk = doom_key_pairs[i].doom;
        if (sym < 0 || sym >= 1024)
            continue;
        if (cur[sym] != prev_kb[sym]) {
            post_key(cur[sym], dk);
            prev_kb[sym] = cur[sym];
        }
    }

    {
        uint64_t rel = indevos_get_mouse_rel_accum();
        int dx = (int)(int32_t)(rel >> 32);
        int dy = (int)(int32_t)(rel & 0xFFFFFFFFu);
        uint64_t bt = indevos_get_mouse_buttons();
        int btn = (int)(bt & 7u);
        event_t ev;
        ev.type = ev_mouse;
        ev.data1 = btn;
        ev.data2 = dx;
        ev.data3 = dy;
        D_PostEvent(&ev);
    }
}

void I_InitGraphics(void)
{
    extern unsigned char *screens[5];

    indevos_desktop_embed_info_t emb;
    memset(&emb, 0, sizeof(emb));
    g_desktop_embed = (indevos_desktop_embed_query(&emb) == 0 && emb.active &&
                       emb.content_w > 0 && emb.content_h > 0);

    if (g_desktop_embed) {
        disp_w = (int)emb.content_w;
        disp_h = (int)emb.content_h;
    } else {
        uint64_t ds = indevos_get_display_size();
        disp_w = (int)(ds >> 32);
        disp_h = (int)(ds & 0xFFFFFFFF);
    }

    if (disp_w < DOOM_WIDTH || disp_h < DOOM_HEIGHT) {
        disp_w = DOOM_WIDTH;
        disp_h = DOOM_HEIGHT;
    }

    int sx = disp_w / DOOM_WIDTH;
    int sy = disp_h / DOOM_HEIGHT;
    scale = sx < sy ? sx : sy;
    if (scale < 1) scale = 1;
    if (scale > DOOM_MAX_SCALE) scale = DOOM_MAX_SCALE;

    view_w = DOOM_WIDTH * scale;
    view_h = DOOM_HEIGHT * scale;
    off_x = (disp_w - view_w) / 2;
    off_y = (disp_h - view_h) / 2;

    /* Always init SDL events so raw keyboard mode is on, even when the
     * desktop already created a window for this pid. */
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        I_Error("SDL_Init failed (see kernel log / SDL_GetError)");
    }

    if (!g_desktop_embed) {
        int win_x = off_x;
        int win_y = off_y;
        disp_w = view_w;
        disp_h = view_h;
        off_x = 0;
        off_y = 0;

        sdl_window = SDL_CreateWindow("DOOM", win_x, win_y, disp_w, disp_h, 0);
        if (!sdl_window) {
            I_Error("SDL_CreateWindow failed");
        }

        sdl_renderer = SDL_CreateRenderer(sdl_window, -1, SDL_RENDERER_SOFTWARE);
        if (!sdl_renderer) {
            I_Error("SDL_CreateRenderer failed");
        }
    } else {
        sdl_window = NULL;
        sdl_renderer = NULL;
    }

    rgba_native = (uint32_t *)malloc((size_t)DOOM_WIDTH * (size_t)DOOM_HEIGHT * sizeof(uint32_t));
    if (!rgba_native) {
        I_Error("Failed to allocate RGBA buffer");
    }
    memset(rgba_native, 0, (size_t)DOOM_WIDTH * (size_t)DOOM_HEIGHT * sizeof(uint32_t));

    doom_screen = (unsigned char *)malloc(DOOM_WIDTH * DOOM_HEIGHT);
    if (!doom_screen) {
        I_Error("Failed to allocate screen buffer");
    }

    memset(doom_screen, 0, DOOM_WIDTH * DOOM_HEIGHT);
    memset(prev_kb, 0, sizeof(prev_kb));
    sdl_video_ready = 1;

    screens[0] = doom_screen;

    for (int j = 0; j < 256; j++) {
        palette[j] = doom_rgb_to_pixel((unsigned)j, (unsigned)j, (unsigned)j);
    }

    {
        char vbuf[96];
        snprintf(vbuf, sizeof(vbuf),
                 g_desktop_embed
                     ? "DOOM desktop window %dx%d  scale %d  view %dx%d+%d+%d\n"
                     : "DOOM fullscreen %dx%d  scale %d  view %dx%d+%d+%d\n",
                 disp_w, disp_h, scale, view_w, view_h, off_x, off_y);
        print(vbuf);
    }
}

void I_ShutdownGraphics(void)
{
    sdl_video_ready = 0;

    if (!g_desktop_embed) {
        if (sdl_renderer) {
            SDL_DestroyRenderer(sdl_renderer);
            sdl_renderer = NULL;
        }
        if (sdl_window) {
            SDL_DestroyWindow(sdl_window);
            sdl_window = NULL;
        }
    }
    SDL_Quit();

    if (rgba_native) {
        free(rgba_native);
        rgba_native = NULL;
    }
    if (doom_screen) {
        free(doom_screen);
        doom_screen = NULL;
    }
}

void I_SetPalette(byte *pal)
{
    for (int i = 0; i < 256; i++) {
        unsigned r = (unsigned)pal[0];
        unsigned g = (unsigned)pal[1];
        unsigned b = (unsigned)pal[2];
        pal += 3;
        palette[i] = doom_rgb_to_pixel(r, g, b);
    }
}

void I_UpdateNoBlit(void)
{
}


extern uint64_t get_ticks(void);
extern void hpet_sleep_counter(uint64_t ms);

/* Cap presents at TICRATE so a frame that did not sleep still cannot
 * spin the CPU. TryRunTics waits on the same clock; if it already slept,
 * this deadline is in the past and the call is a no-op. */
static void doom_pace_frame(void)
{
    static uint64_t next_ms;
    uint64_t now = get_ticks();
    if (next_ms && now < next_ms) {
        uint64_t wait = next_ms - now;
        if (wait > 100)
            wait = 100;
        hpet_sleep_counter(wait);
    }
    next_ms = get_ticks() + (1000 / TICRATE);
}

void I_FinishUpdate(void)
{
    int x, y;

    if (!sdl_video_ready || !doom_screen || !rgba_native)
        return;

    /* Poll every presented frame — NetUpdate only calls I_StartTic when
     * I_GetTime advanced, so a slow present used to drop all keys. */
    poll_sdl_input();

    /* Palette → native 320x200 RGBA. Kernel integer-scales into the window. */
    for (y = 0; y < DOOM_HEIGHT; y++) {
        uint32_t *dst = rgba_native + y * DOOM_WIDTH;
        const byte *src = doom_screen + y * DOOM_WIDTH;
        for (x = 0; x < DOOM_WIDTH; x++)
            dst[x] = palette[src[x]];
    }

    /* Desktop window exists after SDL_CreateWindow — use one composite syscall
     * instead of copy_pixels + present (saves a syscall and a kernel memcpy). */
    if (use_desktop_composite < 0) {
        indevos_desktop_embed_info_t emb;
        if (g_desktop_embed) {
            use_desktop_composite = 1;
        } else if (indevos_desktop_embed_query(&emb) == 0 && emb.active) {
            use_desktop_composite = 1;
        } else {
            use_desktop_composite = 0;
        }
    }
    if (use_desktop_composite) {
        indevos_desktop_composite(rgba_native, DOOM_WIDTH, DOOM_HEIGHT);
        doom_pace_frame();
        return;
    }

    if (!sdl_renderer)
        return;

    if (SDL_CopyPixelsFromUser(sdl_renderer, rgba_native,
                               DOOM_WIDTH, DOOM_HEIGHT) != 0)
        return;
    SDL_RenderPresent(sdl_renderer);
    doom_pace_frame();
}

void I_ReadScreen(byte *scr)
{
    if (doom_screen) {
        memcpy(scr, doom_screen, DOOM_WIDTH * DOOM_HEIGHT);
    }
}

void I_StartTic(void)
{
    poll_sdl_input();
}

void I_StartFrame(void)
{
    poll_sdl_input();
}
