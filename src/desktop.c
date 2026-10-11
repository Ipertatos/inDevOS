/**
 * @file desktop.c
 * @brief Desktop environment compositor, window manager, and panel
 *
 * Phase 1 WM layer (this file):
 *   focus model (single focused id), Z-order list, WM-drawn chrome,
 *   hit-testing, raise-on-click, title-bar move, edge resize, pointer
 *   grab during drag/resize, and client-only input routing.
 *
 * Compositor stacks wallpaper -> windows (bottom..top) with decorations
 * -> menubar/dock/menus -> cursor. Clients draw only into win->pixels
 * (client rect); SDL/SYS_DESKTOP_COMPOSITE share that buffer.
 *
 * desktop_poll_input() and desktop_composite_frame() share the same WM
 * pointer pipeline so Ring-3 fast-present paths still get focus/chrome
 * semantics. desktop_wm_needs_composite() tells the kernel when a full
 * stack redraw is required (multi-window, drag, menus, dirty chrome).
 */
#include "desktop.h"
#include "notepad.h"
#include "files.h"
#include "kterm.h"
#include "settings_app.h"
#include "ktime.h"
#include "render.h"
#include "mem.h"
#include "utils.h"
#include "serial.h"
#include "mouse.h"
#include "keyboard.h"
#include "gpu_hal.h"
#include "hpet.h"
#include "rtc.h"
#include "exec.h"
#include "syscall.h"
#include "exec.h"
#include "png.h"
#include "vfs.h"
#include "thread.h"
#include "string.h"
#include "uaccess.h"
#include "settings.h"
#include "vconsole.h"
#include <stdbool.h>
#include <stdint.h>

/* ---- extern helpers from utils.c ---- */
extern void putpixel(uint64_t x, uint64_t y, uint32_t color);
extern void fillrect(uint32_t color, uint64_t x, uint64_t y, uint64_t w, uint64_t h);
extern uint32_t fb_get_width(void);
extern uint32_t fb_get_height(void);
extern struct limine_framebuffer *fb;
extern void SDL_ResetInputState(void);
extern int snprintf(char *str, size_t size, const char *fmt, ...);

/* ---- global desktop state ---- */
static bool              desktop_running;
static spinlock_t        desktop_lock;
static desktop_window_t  windows[DESKTOP_MAX_WINDOWS];
static desktop_window_t *win_bottom;   /* lowest Z  */
static desktop_window_t *win_top;      /* highest Z */
static uint32_t          next_win_id = 1;
static uint32_t          dt_focused_id = 0;   /* 0 = none */
static uint32_t          dt_grab_id = 0;      /* pointer grab during drag/resize */

/* Mouse state cache */
static int  dt_mouse_x, dt_mouse_y;
static uint8_t dt_mouse_btn;
static uint8_t dt_prev_btn;

/* Dirty flag: set whenever anything needs repaint */
static volatile bool dt_dirty = true;

/* Clip rectangle honoured by all drawing primitives (exclusive x1/y1).
 * Defined here because wallpaper blit honours it before the pixel
 * helpers section. */
static int dt_clip_x0 = 0, dt_clip_y0 = 0;
static int dt_clip_x1 = 1 << 30, dt_clip_y1 = 1 << 30;

/* Compositor pacing (~60 fps) + panel clock cache (avoid CMOS on every pass) */
#define DT_RTC_POLL_MS 250u
static uint64_t      dt_last_frame = 0;
static uint64_t      dt_last_clock = 0;
static rtc_time_t    dt_panel_rtc_cache;
static uint64_t      dt_last_rtc_poll_ms = 0;

/* Compositor-private backbuffer.  All dt_putpixel/dt_fill writes go here when
 * non-NULL, and we memcpy the whole buffer to the framebuffer once per frame.
 * Allocated lazily because the early-boot fb_console kalloc fails (mem not
 * yet initialised when fb_console_init runs). */
static uint32_t *dt_backbuffer = NULL;
static uint32_t  dt_bb_w = 0;
static uint32_t  dt_bb_h = 0;
static uint32_t  dt_bb_pitch_pix = 0;   /* pixels per row in backbuffer */
static bool      dt_bb_alloc_failed = false;

/* GUI poll queue: last client-relative coords for MOVE coalescing */
static int  dt_last_gui_rx = -1, dt_last_gui_ry = -1;
static uint32_t dt_last_gui_pid = 0;

/* Wallpaper (PNG, optional) */
static uint32_t *dt_wallpaper_pixels;
static uint32_t  dt_wallpaper_w, dt_wallpaper_h;
static bool      dt_wallpaper_tried;

/* Baked full-screen wallpaper — orbs/gradients/PNG stretch once, blit after. */
static uint32_t *dt_wp_cache;
static uint32_t  dt_wp_cw, dt_wp_ch;
static bool      dt_wp_cache_failed;

/* Desktop folder icons (VFS) */
#define DT_ICON_MAX 32
static vfs_dirent_t dt_icon_entries[DT_ICON_MAX];
static int          dt_icon_count;
static char         dt_icon_base_path[256] = "/os";
static uint64_t     dt_icon_last_click_tick;
static int          dt_icon_last_idx;

static const char *const dt_wallpaper_try_paths[DESKTOP_WALLPAPER_PATHS] = {
    "/os/wallpaper.png",
    "/wallpaper.png",
    "/os/desktop.png",
};

typedef struct {
    const char *label;
    const char *spawn_path;            /* primary path */
    const char *const *alt_paths;      /* NULL-terminated list of fallbacks */
    void (*launch)(void);              /* kernel app; takes priority if set */
} dt_dock_app_t;

static void dt_launch_terminal(void);
static void dt_toggle_fullscreen(desktop_window_t *w);
static void dt_launch_files(void);
static void dt_launch_settings(void);

static const char *const dt_alt_doom[] = {
    "/bin/doom.exe",
    "/bin/doom",
    "/doom.exe",
    "/doom",
    "/os/apps/doom.exe",
    "/os/doom.exe",
    NULL,
};
static const dt_dock_app_t dt_dock_apps[] = {
    { "Note",     NULL, NULL,        notepad_open },
    { "Term",     NULL, NULL,        dt_launch_terminal },
    { "Files",    NULL, NULL,        dt_launch_files },
    { "Settings", NULL, NULL,        dt_launch_settings },
    { "Doom",     "/bin/doom", dt_alt_doom, NULL },
};
#define DT_DOCK_COUNT ((int)(sizeof(dt_dock_apps) / sizeof(dt_dock_apps[0])))

static inline void dt_putpixel(int x, int y, uint32_t c);
static void dt_fill(uint32_t c, int x, int y, int w, int h);
static void dt_draw_text(int x, int y, const char *s, uint32_t color, int scr_w, int scr_h);
static void dt_open_kernel_text_window(const char *title, const char *const *lines);
static void dt_launch_terminal(void);
static void dt_launch_files(void);
static void dt_launch_settings(void);
static void dt_draw_dock_glyph(int x, int y, int slot, uint32_t col);
static void dt_blit_icon(int dx, int dy, int dw, int dh, const uint32_t *src, int sw, int sh);
static void dt_draw_start_menu(int scr_w, int scr_h);
static void dt_draw_switcher(int scr_w, int scr_h);
static int dt_start_hit(int mx, int my);
static void dt_start_activate(int idx);
static void dt_rt_bind(void);
static void dt_damage_add(int x, int y, int w, int h);
static void dt_dock_geom(int scr_w, int scr_h, int *x, int *y, int *w, int *h);
static int dt_point_in_rect(int px, int py, int x, int y, int w, int h);

static r_target_t dt_rt;
static bool       dt_rt_ready;

#define DT_DOCK_TILE    44
#define DT_DOCK_PAD     10
#define DT_DOCK_H       56
#define DT_DOCK_MARGIN  14
#define DT_LOGO_W       96
#define DT_TAB_W        100
#define DT_TAB_H        22
#define DT_CLOSE_PAD    12
#define DT_RESIZE_BORDER 6
#define DT_EDGE_N        1
#define DT_EDGE_S        2
#define DT_EDGE_W        4
#define DT_EDGE_E        8
#define DT_MIN_CONTENT_W 160
#define DT_MIN_CONTENT_H 80
#define DT_TITLE_DBLCLK_MS 400

static uint64_t dt_title_last_click;
static uint32_t dt_title_last_id;
static int dt_hover_btn = -1;
static uint32_t dt_hover_win_id;

static struct {
    uint32_t wallpaper, wallpaper_bot, titlebar_focus, titlebar_unfoc;
    uint32_t border, border_focus, panel_bg, panel_active, menubar_bg;
    uint32_t dock_btn, text_white, text_gray, surface, accent;
} dt_pal;

#undef DT_COL_WALLPAPER
#undef DT_COL_WALLPAPER_BOT
#undef DT_COL_TITLEBAR_FOCUS
#undef DT_COL_TITLEBAR_UNFOC
#undef DT_COL_BORDER
#undef DT_COL_BORDER_FOCUS
#undef DT_COL_PANEL_BG
#undef DT_COL_PANEL_ACTIVE
#undef DT_COL_MENUBAR_BG
#undef DT_COL_DOCK_BTN
#undef DT_COL_TEXT_WHITE
#undef DT_COL_TEXT_GRAY
#undef DT_COL_SURFACE
#undef DT_COL_ACCENT
#define DT_COL_WALLPAPER      dt_pal.wallpaper
#define DT_COL_WALLPAPER_BOT  dt_pal.wallpaper_bot
#define DT_COL_TITLEBAR_FOCUS dt_pal.titlebar_focus
#define DT_COL_TITLEBAR_UNFOC dt_pal.titlebar_unfoc
#define DT_COL_BORDER         dt_pal.border
#define DT_COL_BORDER_FOCUS   dt_pal.border_focus
#define DT_COL_PANEL_BG       dt_pal.panel_bg
#define DT_COL_PANEL_ACTIVE   dt_pal.panel_active
#define DT_COL_MENUBAR_BG     dt_pal.menubar_bg
#define DT_COL_DOCK_BTN       dt_pal.dock_btn
#define DT_COL_TEXT_WHITE     dt_pal.text_white
#define DT_COL_TEXT_GRAY      dt_pal.text_gray
#define DT_COL_SURFACE        dt_pal.surface
#define DT_COL_ACCENT         dt_pal.accent

static bool dt_start_open;
static bool dt_switcher_open;
static int  dt_switcher_sel;
static int  dt_start_hover = -1;
static uint32_t *dt_dock_icon[8];
static uint32_t  dt_dock_icon_w[8], dt_dock_icon_h[8];
static bool      dt_dock_icons_tried;

static void dt_get_client_rect(const desktop_window_t *w, int *cx, int *cy, int *cw, int *ch) {
    if (w->decorated) {
        *cx = w->x + DESKTOP_BORDER_W;
        *cy = w->y + DESKTOP_BORDER_W + DESKTOP_TITLEBAR_H;
        *cw = w->content_w;
        *ch = w->content_h;
    } else {
        *cx = w->x;
        *cy = w->y;
        *cw = w->content_w;
        *ch = w->content_h;
    }
}

static void dt_win_frame(const desktop_window_t *w, int *fx, int *fy, int *fw, int *fh) {
    *fx = w->x;
    *fy = w->y;
    *fw = w->content_w + (w->decorated ? 2 * DESKTOP_BORDER_W : 0);
    *fh = w->content_h + (w->decorated ? DESKTOP_TITLEBAR_H + 2 * DESKTOP_BORDER_W : 0);
}

static void dt_close_rect(const desktop_window_t *w, int *cx, int *cy, int *cs) {
    *cs = DESKTOP_CLOSE_BTN_SZ;
    *cx = w->x + DESKTOP_BORDER_W + DT_CLOSE_PAD;
    *cy = w->y + DESKTOP_BORDER_W + (DESKTOP_TITLEBAR_H - DESKTOP_CLOSE_BTN_SZ) / 2;
}

static void dt_traffic_rect(const desktop_window_t *w, int which, int *cx, int *cy, int *cs) {
    dt_close_rect(w, cx, cy, cs);
    *cx += which * (*cs + 8);
}

static int dt_hit_traffic(const desktop_window_t *w, int mx, int my) {
    for (int i = 0; i < 3; i++) {
        int bx, by, bs;
        dt_traffic_rect(w, i, &bx, &by, &bs);
        if (dt_point_in_rect(mx, my, bx, by, bs, bs))
            return i;
    }
    return -1;
}

static void dt_damage_window(const desktop_window_t *w) {
    int fx, fy, fw, fh;
    dt_win_frame(w, &fx, &fy, &fw, &fh);
    dt_damage_add(fx - DESKTOP_SHADOW_PAD, fy - 2,
                  fw + DESKTOP_SHADOW_PAD * 2, fh + DESKTOP_SHADOW_PAD + 4);
}

static bool dt_set_content_size(desktop_window_t *w, int nw, int nh) {
    if (nw < DT_MIN_CONTENT_W) nw = DT_MIN_CONTENT_W;
    if (nh < DT_MIN_CONTENT_H) nh = DT_MIN_CONTENT_H;
    if (nw == w->content_w && nh == w->content_h)
        return true;

    size_t sz = (size_t)nw * (size_t)nh * 4;
    uint32_t *np = (uint32_t *)kalloc(sz);
    if (!np)
        return false;
    memset(np, 0, sz);

    int cw = nw < w->content_w ? nw : w->content_w;
    int ch = nh < w->content_h ? nh : w->content_h;
    spinlock_lock(&w->content_lock);
    if (w->pixels) {
        uint32_t old_pitch = w->pixel_pitch / 4;
        for (int y = 0; y < ch; y++)
            memcpy(np + (size_t)y * (size_t)nw,
                   w->pixels + (size_t)y * old_pitch,
                   (size_t)cw * 4);
        kfree(w->pixels);
    }
    w->pixels = np;
    w->content_w = nw;
    w->content_h = nh;
    w->pixel_pitch = (uint32_t)nw * 4;
    spinlock_unlock(&w->content_lock);

    if (w->pid) {
        gui_event_t ev = { GUI_EVENT_RESIZE, nw, nh, 0 };
        process_gui_enqueue_by_pid(w->pid, &ev);
    }
    return true;
}

static void dt_save_restore_geom(desktop_window_t *w) {
    w->restore_x = w->x;
    w->restore_y = w->y;
    w->restore_w = w->content_w;
    w->restore_h = w->content_h;
}

static void dt_restore_window(desktop_window_t *w) {
    dt_damage_window(w);
    if (w->minimized) {
        w->minimized = false;
        w->visible = true;
    }
    if (w->maximized) {
        w->maximized = false;
        w->x = w->restore_x;
        w->y = w->restore_y;
        dt_set_content_size(w, w->restore_w, w->restore_h);
    }
    dt_damage_window(w);
    dt_dirty = true;
}

static void dt_minimize_window(desktop_window_t *w) {
    if (w->minimized)
        return;
    dt_damage_window(w);
    w->minimized = true;
    w->focused = false;
    w->dragging = false;
    w->resize_edges = 0;
    dt_dirty = true;
}

static void dt_maximize_window(desktop_window_t *w) {
    if (w->minimized)
        dt_restore_window(w);
    if (w->maximized) {
        dt_restore_window(w);
        return;
    }
    int scr_w = (int)fb_get_width();
    int scr_h = (int)fb_get_height();
    int dock_x, dock_y, dock_w, dock_h;
    dt_dock_geom(scr_w, scr_h, &dock_x, &dock_y, &dock_w, &dock_h);

    dt_save_restore_geom(w);
    dt_damage_window(w);
    w->maximized = true;
    w->x = 0;
    w->y = DESKTOP_MENUBAR_H;
    int cw = scr_w - 2 * DESKTOP_BORDER_W;
    int ch = dock_y - DESKTOP_MENUBAR_H - DESKTOP_TITLEBAR_H - 2 * DESKTOP_BORDER_W - 8;
    if (ch < DT_MIN_CONTENT_H) ch = DT_MIN_CONTENT_H;
    dt_set_content_size(w, cw, ch);
    dt_damage_window(w);
    dt_dirty = true;
}

static int dt_resize_hit(const desktop_window_t *w, int mx, int my) {
    if (!w->resizable || w->maximized || w->minimized || !w->decorated)
        return 0;
    int fx, fy, fw, fh;
    dt_win_frame(w, &fx, &fy, &fw, &fh);
    int e = 0;
    if (mx >= fx - DT_RESIZE_BORDER && mx < fx + DT_RESIZE_BORDER) e |= DT_EDGE_W;
    if (mx >= fx + fw - DT_RESIZE_BORDER && mx < fx + fw + DT_RESIZE_BORDER) e |= DT_EDGE_E;
    if (my >= fy - DT_RESIZE_BORDER && my < fy + DT_RESIZE_BORDER) e |= DT_EDGE_N;
    if (my >= fy + fh - DT_RESIZE_BORDER && my < fy + fh + DT_RESIZE_BORDER) e |= DT_EDGE_S;
    /* Interior of the titlebar is a drag, not a north-resize, except corners. */
    if ((e & DT_EDGE_N) && !(e & (DT_EDGE_W | DT_EDGE_E)) &&
        my >= fy && my < fy + DESKTOP_TITLEBAR_H)
        e &= ~DT_EDGE_N;
    return e;
}

static void dt_dock_geom(int scr_w, int scr_h, int *x, int *y, int *w, int *h) {
    *h = DT_DOCK_H;
    *w = DT_DOCK_COUNT * DT_DOCK_TILE +
         (DT_DOCK_COUNT > 0 ? (DT_DOCK_COUNT - 1) * DESKTOP_DOCK_BTN_GAP : 0) +
         2 * DT_DOCK_PAD;
    if (*w > scr_w - 24) *w = scr_w - 24;
    *x = (scr_w - *w) / 2;
    *y = scr_h - *h - DT_DOCK_MARGIN;
}

static int dt_point_in_rect(int px, int py, int x, int y, int w, int h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

void desktop_gui_enqueue_key_sdl(int32_t keycode, bool pressed) {
    desktop_window_t *fw = desktop_get_focused();
    if (!fw) return;
    if (notepad_is_window(fw)) {
        notepad_on_key(keycode, pressed);
        return;
    }
    if (files_is_window(fw)) {
        files_on_key(keycode, pressed);
        return;
    }
    if (kterm_is_window(fw)) {
        kterm_on_key(keycode, pressed);
        return;
    }
    if (settings_app_is_window(fw)) {
        settings_app_on_key(keycode, pressed);
        return;
    }
    if (!fw->pid) return;
    gui_event_t ev;
    ev.type = pressed ? GUI_EVENT_KEY : GUI_EVENT_KEYUP;
    ev.arg1 = keycode;
    ev.arg2 = 0;
    ev.arg3 = 0;
    process_gui_enqueue_by_pid(fw->pid, &ev);
}

static void dt_refresh_desktop_icons(void) {
    dt_icon_count = 0;
    vfs_dirent_t tmp[DT_ICON_MAX];
    static const char *paths[] = {
        "/home/user/Desktop", "/Desktop", "/os/Desktop"
    };
    for (unsigned p = 0; p < sizeof(paths) / sizeof(paths[0]); p++) {
        int n = vfs_readdir(paths[p], tmp, DT_ICON_MAX);
        if (n > 0) {
            int w = 0;
            while (paths[p][w] && w < (int)sizeof(dt_icon_base_path) - 1) {
                dt_icon_base_path[w] = paths[p][w];
                w++;
            }
            dt_icon_base_path[w] = '\0';
            int wn = 0;
            for (int i = 0; i < n && wn < DT_ICON_MAX; i++) {
                if (tmp[i].name[0] == '.' && tmp[i].name[1] == '\0') continue;
                if (tmp[i].name[0] == '.' && tmp[i].name[1] == '.' && tmp[i].name[2] == '\0') continue;
                dt_icon_entries[wn++] = tmp[i];
            }
            dt_icon_count = wn;
            serial_printf("[desktop] desktop icons from %s (%d entries)\n", paths[p], dt_icon_count);
            return;
        }
    }
}

#define DT_WP_BUF_MAX (2u * 1024u * 1024u)

static void dt_wallpaper_try_load(void) {
    /* Atomic test-and-set so concurrent callers (e.g. compositor invoked from
     * more than one execution context) cannot both enter the loader and double-
     * decode the PNG.  __sync_bool_compare_and_swap returns true only for the
     * caller that successfully flipped false -> true. */
    if (!__sync_bool_compare_and_swap(&dt_wallpaper_tried, false, true)) return;
    for (unsigned i = 0; i < DESKTOP_WALLPAPER_PATHS; i++) {
        int fd = vfs_open(dt_wallpaper_try_paths[i], VFS_O_READ);
        if (fd < 0) continue;
        uint8_t *buf = (uint8_t *)kalloc(DT_WP_BUF_MAX);
        if (!buf) {
            vfs_close(fd);
            continue;
        }
        uint32_t len = 0;
        while (len < DT_WP_BUF_MAX) {
            int r = vfs_read(fd, buf + len, DT_WP_BUF_MAX - len);
            if (r <= 0) break;
            len += (uint32_t)r;
        }
        vfs_close(fd);
        png_image_t *img = png_decode(buf, len);
        kfree(buf);
        if (!img || !img->valid || !img->pixels) {
            if (img) png_free(img);
            continue;
        }
        dt_wallpaper_pixels = img->pixels;
        dt_wallpaper_w = img->width;
        dt_wallpaper_h = img->height;
        img->pixels = NULL;
        png_free(img);
        serial_printf("[desktop] wallpaper %s %ux%u\n",
                      dt_wallpaper_try_paths[i], dt_wallpaper_w, dt_wallpaper_h);
        return;
    }
}

static void dt_stretch_into(uint32_t *dst, int dw, int dh, int dpitch)
{
    if (!dt_wallpaper_pixels || dt_wallpaper_w == 0 || dt_wallpaper_h == 0)
        return;
    uint32_t sw = dt_wallpaper_w, sh = dt_wallpaper_h;
    if (dh <= 0 || dw <= 0) return;

    /* Bilinear scale in 16.16 fixed point, sampling pixel centres. */
    int64_t step_x = ((int64_t)sw << 16) / dw;
    int64_t step_y = ((int64_t)sh << 16) / dh;
    int64_t fy = step_y / 2 - 0x8000;
    for (int y = 0; y < dh; y++, fy += step_y) {
        int64_t cy = fy < 0 ? 0 : fy;
        uint32_t y0 = (uint32_t)(cy >> 16);
        uint32_t wy = (uint32_t)(cy & 0xFFFF) >> 8;
        if (y0 >= sh - 1) { y0 = sh - 1; wy = 0; }
        uint32_t y1 = y0 + (wy ? 1 : 0);
        const uint32_t *r0 = dt_wallpaper_pixels + y0 * sw;
        const uint32_t *r1 = dt_wallpaper_pixels + y1 * sw;
        uint32_t *dst_row = dst + (uint32_t)y * (uint32_t)dpitch;

        int64_t fx = step_x / 2 - 0x8000;
        for (int x = 0; x < dw; x++, fx += step_x) {
            int64_t cx = fx < 0 ? 0 : fx;
            uint32_t x0 = (uint32_t)(cx >> 16);
            uint32_t wx = (uint32_t)(cx & 0xFFFF) >> 8;
            if (x0 >= sw - 1) { x0 = sw - 1; wx = 0; }
            uint32_t x1 = x0 + (wx ? 1 : 0);

            uint32_t a = r0[x0], b = r0[x1], c = r1[x0], d = r1[x1];
            uint32_t out = 0;
            for (int sh_bits = 0; sh_bits < 24; sh_bits += 8) {
                uint32_t ca = (a >> sh_bits) & 0xFF, cb = (b >> sh_bits) & 0xFF;
                uint32_t cc = (c >> sh_bits) & 0xFF, cd = (d >> sh_bits) & 0xFF;
                uint32_t top = ca * (256 - wx) + cb * wx;
                uint32_t bot = cc * (256 - wx) + cd * wx;
                uint32_t v = (top * (256 - wy) + bot * wy) >> 16;
                out |= v << sh_bits;
            }
            dst_row[x] = out | 0xFF000000u;
        }
    }
}

static void dt_bake_wallpaper(int w, int h)
{
    if (w <= 0 || h <= 0) return;
    if (dt_wp_cache && dt_wp_cw == (uint32_t)w && dt_wp_ch == (uint32_t)h)
        return;
    if (dt_wp_cache_failed) return;

    if (dt_wp_cache) {
        kfree(dt_wp_cache);
        dt_wp_cache = NULL;
    }
    dt_wp_cache = (uint32_t *)kalloc((size_t)w * (size_t)h * 4u);
    if (!dt_wp_cache) {
        dt_wp_cache_failed = true;
        serial_puts("[desktop] wallpaper cache alloc failed\n");
        return;
    }
    dt_wp_cw = (uint32_t)w;
    dt_wp_ch = (uint32_t)h;

    dt_wallpaper_try_load();
    if (dt_wallpaper_pixels && dt_wallpaper_w > 0 && dt_wallpaper_h > 0) {
        dt_stretch_into(dt_wp_cache, w, h, w);
        return;
    }

    r_target_t t;
    r_target_init(&t, dt_wp_cache, w, h, w);
    r_vgrad(&t, 0, 0, w, h, DT_COL_WALLPAPER, DT_COL_WALLPAPER_BOT);
    r_circle_blend(&t, w * 7 / 10, h / 4, w / 5, 0x283DDC97);
    r_circle_blend(&t, w / 5, h * 2 / 3, w / 6, 0x225B8DEF);
    r_circle_blend(&t, w / 2, h / 2, w / 8, 0x18A78BFA);
    r_fill_blend(&t, 0, 0, w, 80, 0x28000000);
    r_fill_blend(&t, 0, h - 90, w, 90, 0x30000000);
}

static int dt_icon_hit(int mx, int my, int scr_w, int scr_h) {
    if (my < DESKTOP_TOP_DEADSPACE || my >= scr_h - DESKTOP_PANEL_H)
        return -1;
    int col0 = DESKTOP_ICON_GRID;
    int row0 = DESKTOP_TOP_DEADSPACE + DESKTOP_ICON_GRID;
    int maxcols = (scr_w - col0) / DESKTOP_ICON_COL_W;
    if (maxcols < 1) maxcols = 1;
    for (int i = 0; i < dt_icon_count; i++) {
        int col = i % maxcols;
        int row = i / maxcols;
        int ix = col0 + col * DESKTOP_ICON_COL_W;
        int iy = row0 + row * DESKTOP_ICON_ROW_H;
        if (mx >= ix && mx < ix + DESKTOP_ICON_COL_W - DESKTOP_ICON_GRID &&
            my >= iy && my < iy + DESKTOP_ICON_ROW_H - DESKTOP_ICON_GRID)
            return i;
    }
    return -1;
}

static void dt_draw_desktop_icons(int scr_w, int scr_h) {
    (void)scr_w;
    (void)scr_h;
    int col0 = DESKTOP_ICON_GRID;
    int row0 = DESKTOP_TOP_DEADSPACE + DESKTOP_ICON_GRID;
    int maxcols = (scr_w - col0) / DESKTOP_ICON_COL_W;
    if (maxcols < 1) maxcols = 1;
    static const uint32_t badge[] = {
        0xFF2DD4BF, 0xFF60A5FA, 0xFFA78BFA, 0xFFF87171, 0xFFFBBF24
    };
    for (int i = 0; i < dt_icon_count; i++) {
        int col = i % maxcols;
        int row = i / maxcols;
        int ix = col0 + col * DESKTOP_ICON_COL_W;
        int iy = row0 + row * DESKTOP_ICON_ROW_H;
        uint32_t colr = badge[i % 5];
        if (dt_rt_ready) {
            r_rrect_blend(&dt_rt, ix, iy, 56, 40, 8, 0xCC1A2035);
            r_rrect(&dt_rt, ix + 14, iy + 6, 28, 22, 6, colr);
            char ch[2] = { dt_icon_entries[i].name[0], 0 };
            if (ch[0] >= 'a' && ch[0] <= 'z') ch[0] = (char)(ch[0] - 32);
            r_text(&dt_rt, ix + 24, iy + 8, ch, DT_COL_TEXT_WHITE);
            r_text(&dt_rt, ix, iy + 48, dt_icon_entries[i].name, DT_COL_TEXT_WHITE);
        } else {
            dt_fill(DT_COL_PANEL_ACTIVE, ix, iy, 56, 40);
            dt_draw_text(ix + 4, iy + 48, dt_icon_entries[i].name, DT_COL_TEXT_WHITE, scr_w, scr_h);
        }
    }
}

static void dt_icon_double_click(int idx) {
    if (idx < 0 || idx >= dt_icon_count) return;
    char path[256];
    int bl = 0;
    const char *base = dt_icon_base_path;
    while (base[bl] && bl < (int)sizeof(path) - 2) { path[bl] = base[bl]; bl++; }
    if (bl > 0 && path[bl - 1] != '/') { path[bl++] = '/'; }
    const char *name = dt_icon_entries[idx].name;
    int ni = 0;
    while (name[ni] && bl < (int)sizeof(path) - 1) { path[bl++] = name[ni++]; }
    path[bl] = '\0';

    if (dt_icon_entries[idx].type == VFS_DIRECTORY) {
        serial_printf("[desktop] open folder %s\n", path);
        return;
    }
    int pid = exec_spawn(path, NULL);
    if (pid > 0)
        serial_printf("[desktop] launched %s pid %d\n", path, pid);
    else
        serial_printf("[desktop] could not exec %s\n", path);
}

static void dt_handle_desktop_icon_click(int mx, int my, int scr_w, int scr_h) {
    int idx = dt_icon_hit(mx, my, scr_w, scr_h);
    if (idx < 0) return;
    uint64_t now = hpet_get_ticks();
    if (idx == dt_icon_last_idx && (now - dt_icon_last_click_tick) < 400) {
        dt_icon_double_click(idx);
        dt_icon_last_idx = -1;
    } else {
        dt_icon_last_idx = idx;
        dt_icon_last_click_tick = now;
    }
}

/* ===================================================================
 * Pixel helpers
 * =================================================================== */

/* Lazily allocate the compositor backbuffer.  Called from
 * desktop_composite_frame() each iteration; returns true once the buffer is
 * usable.  The buffer is sized to the current framebuffer. */
static bool dt_ensure_backbuffer(void) {
    if (dt_backbuffer) return true;
    if (dt_bb_alloc_failed) return false;
    if (!fb) return false;
    uint32_t w = fb->width;
    uint32_t h = fb->height;
    if (w == 0 || h == 0) return false;
    size_t bytes = (size_t)w * (size_t)h * 4;
    dt_backbuffer = (uint32_t *)kalloc(bytes);
    if (!dt_backbuffer) {
        dt_bb_alloc_failed = true;
        serial_printf("[desktop] backbuffer alloc FAILED for %ux%u (%zu bytes) — "
                      "compositor will draw direct (flicker)\n", w, h, bytes);
        return false;
    }
    dt_bb_w = w;
    dt_bb_h = h;
    dt_bb_pitch_pix = w;
    memset(dt_backbuffer, 0, bytes);
    serial_printf("[desktop] backbuffer allocated %ux%u (%zu bytes)\n",
                  w, h, bytes);
    return true;
}

/* ===================================================================
 * Clip rectangle + frame damage tracking (dirty rectangles)
 * ===================================================================
 * Each frame accumulates a single damage rectangle (union of everything
 * that changed: cursor old/new position, dirty windows, dragged window
 * old/new rects, clock).  All drawing primitives honour the clip rect,
 * so a mouse move only recomposites + presents a few hundred pixels
 * instead of the full ~16 MB scene at 1080p.
 * (dt_clip_* variables are defined near the top of this file.)
 */
static void dt_rt_bind(void) {
    dt_rt_ready = false;
    if (dt_backbuffer) {
        r_target_init(&dt_rt, dt_backbuffer, (int)dt_bb_w, (int)dt_bb_h, (int)dt_bb_pitch_pix);
        dt_rt_ready = true;
    } else if (fb && fb->address) {
        r_target_from_bytes(&dt_rt, fb->address, (int)fb->width, (int)fb->height, (int)fb->pitch);
        dt_rt_ready = true;
    }
    if (dt_rt_ready)
        r_clip(&dt_rt, dt_clip_x0, dt_clip_y0, dt_clip_x1, dt_clip_y1);
}

static inline void dt_clip_set(int x0, int y0, int x1, int y1) {
    dt_clip_x0 = x0; dt_clip_y0 = y0;
    dt_clip_x1 = x1; dt_clip_y1 = y1;
    if (dt_rt_ready)
        r_clip(&dt_rt, x0, y0, x1, y1);
}

static inline void dt_clip_clear(void) {
    dt_clip_set(0, 0, 1 << 30, 1 << 30);
}

/* Per-frame damage accumulator (exclusive x1/y1). */
static int  dt_dmg_x0, dt_dmg_y0, dt_dmg_x1, dt_dmg_y1;
static bool dt_dmg_any = false;

static void dt_damage_add(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (!dt_dmg_any) {
        dt_dmg_x0 = x; dt_dmg_y0 = y;
        dt_dmg_x1 = x + w; dt_dmg_y1 = y + h;
        dt_dmg_any = true;
        return;
    }
    if (x < dt_dmg_x0) dt_dmg_x0 = x;
    if (y < dt_dmg_y0) dt_dmg_y0 = y;
    if (x + w > dt_dmg_x1) dt_dmg_x1 = x + w;
    if (y + h > dt_dmg_y1) dt_dmg_y1 = y + h;
}

static inline void dt_putpixel(int x, int y, uint32_t c) {
    if (dt_rt_ready) {
        r_put(&dt_rt, x, y, c);
        return;
    }
    if (x < 0 || y < 0) return;
    if (x < dt_clip_x0 || x >= dt_clip_x1 ||
        y < dt_clip_y0 || y >= dt_clip_y1) return;
    putpixel((uint64_t)x, (uint64_t)y, c);
}

static void dt_fill(uint32_t c, int x, int y, int w, int h) {
    if (dt_rt_ready) {
        r_fill(&dt_rt, x, y, w, h, c);
        return;
    }
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w <= 0 || h <= 0) return;
    int x1i = x + w, y1i = y + h;
    if (x < dt_clip_x0) x = dt_clip_x0;
    if (y < dt_clip_y0) y = dt_clip_y0;
    if (x1i > dt_clip_x1) x1i = dt_clip_x1;
    if (y1i > dt_clip_y1) y1i = dt_clip_y1;
    if (x >= x1i || y >= y1i) return;
    fillrect(c, (uint64_t)x, (uint64_t)y, (uint64_t)(x1i - x), (uint64_t)(y1i - y));
}

/* Copy a rectangle of the composited backbuffer to the real framebuffer.
 * With dirty-rect tracking only the damaged region is transferred per frame
 * instead of the full ~16 MB screen. (x1/y1 exclusive.) */
static void dt_present_backbuffer(int x0, int y0, int x1, int y1) {
    if (!dt_backbuffer || !fb) return;
    r_target_t src;
    r_target_init(&src, dt_backbuffer, (int)dt_bb_w, (int)dt_bb_h, (int)dt_bb_pitch_pix);
    r_copy_rect(&src, fb->address, fb->pitch, x0, y0, x1, y1);
}

/* Nearest-neighbour scale of the client buffer into a screen rectangle.
 * Used by fullscreen so the client's pixels fill the display without
 * resizing (and freeing) the buffer Doom/SDL still owns. */
static void dt_scale_client_to(desktop_window_t *win,
                               uint32_t *dst, int dst_pitch_pix,
                               int dx, int dy, int dw, int dh,
                               int clip_x0, int clip_y0, int clip_x1, int clip_y1) {
    int sw = win->content_w;
    int sh = win->content_h;
    int sp = (int)(win->pixel_pitch / 4u);
    if (!dst || !win->pixels || sw < 1 || sh < 1 || dw < 1 || dh < 1 || sp < 1)
        return;
    spinlock_lock(&win->content_lock);
    for (int y = 0; y < dh; y++) {
        int py = dy + y;
        if (py < clip_y0 || py >= clip_y1)
            continue;
        int sy = (int)(((int64_t)y * sh) / dh);
        if (sy >= sh) sy = sh - 1;
        const uint32_t *srow = win->pixels + (size_t)sy * (size_t)sp;
        uint32_t *drow = dst + (size_t)py * (size_t)dst_pitch_pix;
        for (int x = 0; x < dw; x++) {
            int px = dx + x;
            if (px < clip_x0 || px >= clip_x1)
                continue;
            int sx = (int)(((int64_t)x * sw) / dw);
            if (sx >= sw) sx = sw - 1;
            drow[px] = srow[sx] | 0xFF000000u;
        }
    }
    spinlock_unlock(&win->content_lock);
}

static bool dt_present_window_fast(desktop_window_t *win) {
    /* Minimized windows must not paint the client. The compositor owns the
     * screen again and draws wallpaper where the window was. */
    if (!win || !win->visible || win->minimized || !win->pixels || !fb || !fb->address)
        return false;
    /* Fullscreen enter/leave sets dt_dirty. A client-rect blit would
     * leave the previous fullscreen (or chrome) pixels on screen. */
    if (dt_dirty)
        return false;

    if (win->fullscreen) {
        int scr_w = (int)fb->width;
        int scr_h = (int)fb->height;
        int dy = DESKTOP_MENUBAR_H;
        int dh = scr_h - dy;
        if (dh < 1)
            return false;
        gpu_surface_t *hal = gpu_hal_get_primary_surface();
        bool hal_scanout = hal && hal->pixels &&
                           hal->pixels != (uint32_t *)fb->address;
        uint32_t *dst = hal_scanout ? hal->pixels : (uint32_t *)fb->address;
        uint32_t dst_pitch = hal_scanout ? hal->pitch : fb->pitch;
        int dst_pitch_pix = (int)(dst_pitch / 4u);
        if (dst_pitch_pix < 1)
            return false;
        dt_scale_client_to(win, dst, dst_pitch_pix, 0, dy, scr_w, dh,
                           0, dy, scr_w, scr_h);
        gpu_hal_present_rect(0, (uint32_t)dy, (uint32_t)scr_w, (uint32_t)dh);
        win->dirty = false;
        return true;
    }

    int cx, cy, cw, ch;
    dt_get_client_rect(win, &cx, &cy, &cw, &ch);
    int src_x = 0;
    int src_y = 0;
    if (cx < 0) { src_x = -cx; cw += cx; cx = 0; }
    if (cy < 0) { src_y = -cy; ch += cy; cy = 0; }
    if (cx >= (int)fb->width || cy >= (int)fb->height)
        return false;
    if (cx + cw > (int)fb->width) cw = (int)fb->width - cx;
    if (cy + ch > (int)fb->height) ch = (int)fb->height - cy;
    if (cw <= 0 || ch <= 0)
        return false;

    uint32_t w = (uint32_t)cw;
    uint32_t h = (uint32_t)ch;
    uint32_t row_bytes = w * 4;
    const uint8_t *src = (const uint8_t *)win->pixels;

    gpu_surface_t *hal = gpu_hal_get_primary_surface();
    bool hal_scanout = hal && hal->pixels &&
                       hal->pixels != (uint32_t *)fb->address;

    spinlock_lock(&win->content_lock);
    /* One copy to the active scanout buffer.  Previously this path memcpy'd
     * the same client rect into backbuffer + Limine FB + virtio FB (~19 MB for
     * a 1600x1000 doom frame) before flushing — a major FPS killer. */
    void *dst_base = hal_scanout ? (void *)hal->pixels : fb->address;
    uint32_t dst_pitch = hal_scanout ? hal->pitch : fb->pitch;
    for (uint32_t row = 0; row < h; row++) {
        memcpy((uint8_t *)dst_base + ((uint32_t)cy + row) * dst_pitch + (uint32_t)cx * 4,
               src + ((uint32_t)src_y + row) * win->pixel_pitch + (uint32_t)src_x * 4,
               row_bytes);
    }
    spinlock_unlock(&win->content_lock);

    gpu_hal_present_rect((uint32_t)cx, (uint32_t)cy, w, h);
    win->dirty = false;
    return true;
}

/* ===================================================================
 * Text rendering (8x16 font via software renderer)
 * =================================================================== */

static void dt_draw_text(int x, int y, const char *s, uint32_t color,
                         int scr_w, int scr_h) {
    (void)scr_w;
    (void)scr_h;
    if (dt_rt_ready) {
        r_text(&dt_rt, x, y, s, color);
        return;
    }
    while (*s) {
        if (dt_rt_ready)
            r_char(&dt_rt, x, y, *s, color);
        x += 8;
        s++;
    }
}

/* ===================================================================
 * Cursor (outlined arrow)
 * =================================================================== */

static const char *cursor_shape[] = {
    "X",
    "XX",
    "XXX",
    "XXXX",
    "XXXXX",
    "XXXXXX",
    "XXXXXXX",
    "XXXXXXXX",
    "XXXXXXXXX",
    "XXXXXXXXXX",
    "XXXXXXX",
    "XXXX.XX",
    "XXX..XX",
    "XX...XX",
    "X.....X",
    "......X",
    NULL
};

static void dt_draw_cursor(int mx, int my, int scr_w, int scr_h) {
    (void)scr_w;
    (void)scr_h;
    for (int pass = 0; pass < 2; pass++) {
        uint32_t col = pass ? DT_COL_CURSOR_FILL : DT_COL_CURSOR_BORDER;
        int expand = pass ? 0 : 1;
        for (int row = 0; cursor_shape[row]; row++) {
            const char *line = cursor_shape[row];
            for (int coln = 0; line[coln]; coln++) {
                if (line[coln] != 'X') continue;
                if (pass == 0) {
                    dt_putpixel(mx + coln - expand, my + row, col);
                    dt_putpixel(mx + coln + expand, my + row, col);
                    dt_putpixel(mx + coln, my + row - expand, col);
                    dt_putpixel(mx + coln, my + row + expand, col);
                } else {
                    dt_putpixel(mx + coln, my + row, col);
                }
            }
        }
    }
}

/* ===================================================================
 * Wallpaper (simple vertical gradient)
 * =================================================================== */

static bool dt_intersects_clip(int x, int y, int w, int h)
{
    return x < dt_clip_x1 && y < dt_clip_y1 &&
           (x + w) > dt_clip_x0 && (y + h) > dt_clip_y0;
}

static void dt_draw_wallpaper(int w, int h) {
    dt_bake_wallpaper(w, h);
    if (dt_wp_cache && dt_rt_ready) {
        r_blit(&dt_rt, 0, 0, dt_wp_cache, (int)dt_wp_cw, (int)dt_wp_ch, (int)dt_wp_cw);
        return;
    }
    dt_fill(DT_COL_WALLPAPER, 0, 0, w, h);
}

/* ===================================================================
 * Menubar + floating dock
 * =================================================================== */

static int dt_dock_spawned_pid[8] = {0};

static void dt_dock_spawn(int slot) {
    if (slot < 0 || slot >= DT_DOCK_COUNT) return;

    if (dt_dock_apps[slot].launch) {
        dt_dock_apps[slot].launch();
        return;
    }

    /* Prevent double-launching the same dock slot */
    if (dt_dock_spawned_pid[slot] > 0) {
        serial_printf("[desktop] dock: slot %d already running (pid %d)\n",
                      slot, dt_dock_spawned_pid[slot]);
        return;
    }

    const dt_dock_app_t *app = &dt_dock_apps[slot];

    int pid = exec_spawn(app->spawn_path, NULL);
    if (pid > 0) {
        dt_dock_spawned_pid[slot] = pid;
        serial_printf("[desktop] dock: spawned %s pid %d\n",
                      app->spawn_path, pid);
        return;
    }
    if (app->alt_paths) {
        for (int i = 0; app->alt_paths[i]; i++) {
            pid = exec_spawn(app->alt_paths[i], NULL);
            if (pid > 0) {
                dt_dock_spawned_pid[slot] = pid;
                serial_printf("[desktop] dock: spawned %s pid %d\n",
                              app->alt_paths[i], pid);
                return;
            }
        }
    }
    serial_printf("[desktop] dock: failed to spawn %s\n", app->spawn_path);
    /* Visible feedback: open a small kernel window so the user sees the
     * click was registered and which path was tried. */
    static const char *lines[5];
    static char path_line[160];
    int n = 0;
    const char *p = "Path: ";
    while (*p) path_line[n++] = *p++;
    p = dt_dock_apps[slot].spawn_path;
    while (*p && n < (int)sizeof(path_line) - 1) path_line[n++] = *p++;
    path_line[n] = '\0';

    lines[0] = "Could not launch app:";
    lines[1] = path_line;
    lines[2] = "";
    lines[3] = "Build user apps and copy them onto the FAT image.";
    lines[4] = NULL;
    dt_open_kernel_text_window("Launch failed", lines);
}

static void dt_fmt_clock(char *clock_buf) {
    rtc_time_t time = dt_panel_rtc_cache;
    int ci = 0;
    clock_buf[ci++] = '0' + (time.hour / 10);
    clock_buf[ci++] = '0' + (time.hour % 10);
    clock_buf[ci++] = ':';
    clock_buf[ci++] = '0' + (time.minute / 10);
    clock_buf[ci++] = '0' + (time.minute % 10);
    clock_buf[ci++] = ':';
    clock_buf[ci++] = '0' + (time.second / 10);
    clock_buf[ci++] = '0' + (time.second % 10);
    clock_buf[ci] = '\0';
}

static void dt_draw_menubar(int scr_w, int scr_h) {
    (void)scr_h;
    if (!dt_intersects_clip(0, 0, scr_w, DESKTOP_MENUBAR_H))
        return;
    if (dt_rt_ready) {
        r_fill(&dt_rt, 0, 0, scr_w, DESKTOP_MENUBAR_H, DT_COL_MENUBAR_BG);
        r_fill(&dt_rt, 0, DESKTOP_MENUBAR_H - 1, scr_w, 1, DT_COL_BORDER);
        r_rrect(&dt_rt, 8, 4, DT_LOGO_W, 24, 6,
                dt_start_open ? DT_COL_PANEL_ACTIVE : 0xFF1A2035);
        r_fill(&dt_rt, 8, 4, 3, 24, DT_COL_ACCENT);
        r_text(&dt_rt, 18, 8, "Start", DT_COL_TEXT_WHITE);
    } else {
        dt_fill(DT_COL_MENUBAR_BG, 0, 0, scr_w, DESKTOP_MENUBAR_H);
        dt_draw_text(18, 8, "Start", DT_COL_TEXT_WHITE, scr_w, scr_h);
    }

    int bx = 8 + DT_LOGO_W + 12;
    int tab_y = (DESKTOP_MENUBAR_H - DT_TAB_H) / 2;
    spinlock_lock(&desktop_lock);
    for (desktop_window_t *w = win_bottom; w; w = w->next) {
        if (!w->visible && !w->minimized) continue;
        if (bx + DT_TAB_W > scr_w - 100) break;
        uint32_t bg = w->focused ? 0xFF1E2740 : 0xFF141824;
        if (w->minimized) bg = 0xFF0C1018;
        if (dt_rt_ready) {
            r_fill(&dt_rt, bx, tab_y, DT_TAB_W, DT_TAB_H, bg);
            if (w->focused)
                r_fill(&dt_rt, bx + 8, tab_y + DT_TAB_H - 3, DT_TAB_W - 16, 2, DT_COL_ACCENT);
        } else {
            dt_fill(bg, bx, tab_y, DT_TAB_W, DT_TAB_H);
        }
        char label[13];
        int i;
        for (i = 0; i < 12 && w->title[i]; i++)
            label[i] = w->title[i];
        label[i] = '\0';
        dt_draw_text(bx + 8, tab_y + 3, label,
                     w->focused ? DT_COL_TEXT_WHITE : DT_COL_TEXT_GRAY, scr_w, scr_h);
        bx += DT_TAB_W + 6;
    }
    spinlock_unlock(&desktop_lock);

    char clock_buf[16];
    dt_fmt_clock(clock_buf);
    dt_draw_text(scr_w - 80, 8, clock_buf, DT_COL_TEXT_WHITE, scr_w, scr_h);
}

static const uint32_t dt_dock_tile_col[] = {
    0xFFFBBF24, 0xFF2DD4BF, 0xFF60A5FA, 0xFFA78BFA, 0xFFF87171
};

static void dt_draw_panel(int scr_w, int scr_h) {
    int dx, dy, dw, dh;
    dt_dock_geom(scr_w, scr_h, &dx, &dy, &dw, &dh);

    if (!dt_intersects_clip(dx - 4, dy, dw + 8, dh + 8))
        return;
    if (dt_rt_ready) {
        r_fill(&dt_rt, dx + 3, dy + 4, dw, dh, 0xFF07080C);
        r_rrect(&dt_rt, dx, dy, dw, dh, 16, DT_COL_SURFACE);
        r_rrect_outline(&dt_rt, dx, dy, dw, dh, 16, DT_COL_BORDER);
    } else {
        dt_fill(DT_COL_SURFACE, dx, dy, dw, dh);
    }

    int tx = dx + DT_DOCK_PAD;
    int ty = dy + (dh - DT_DOCK_TILE) / 2;
    for (int d = 0; d < DT_DOCK_COUNT; d++) {
        uint32_t col = dt_dock_tile_col[d % 5];
        bool running = dt_dock_spawned_pid[d] > 0;
        if (dt_dock_apps[d].launch == notepad_open)
            running = notepad_is_open();
        else if (dt_dock_apps[d].launch == dt_launch_files)
            running = files_is_open();
        else if (dt_dock_apps[d].launch == dt_launch_terminal)
            running = kterm_is_open();
        else if (dt_dock_apps[d].launch == dt_launch_settings)
            running = settings_app_is_open();
        if (dt_rt_ready) {
            r_rrect(&dt_rt, tx, ty, DT_DOCK_TILE, DT_DOCK_TILE, 10, DT_COL_DOCK_BTN);
            if (dt_dock_icon[d])
                dt_blit_icon(tx + 8, ty + 4, 28, 28, dt_dock_icon[d],
                             (int)dt_dock_icon_w[d], (int)dt_dock_icon_h[d]);
            else
                dt_draw_dock_glyph(tx + 8, ty + 4, d, col);
            r_text(&dt_rt, tx + 6, ty + 30, dt_dock_apps[d].label, DT_COL_TEXT_WHITE);
            if (running)
                r_circle(&dt_rt, tx + DT_DOCK_TILE / 2, ty + DT_DOCK_TILE - 5, 2, DT_COL_ACCENT);
        } else {
            dt_fill(col, tx, ty, DT_DOCK_TILE, DT_DOCK_TILE);
            dt_draw_text(tx + 4, ty + 14, dt_dock_apps[d].label, DT_COL_TEXT_WHITE, scr_w, scr_h);
        }
        tx += DT_DOCK_TILE + DESKTOP_DOCK_BTN_GAP;
    }
}

/* Open a small kernel-owned window with `title` and a few text lines.
 * pid=0 marks the window as kernel-owned so process-cleanup paths leave it
 * alone. Lines is NULL-terminated. */
static void dt_open_kernel_text_window(const char *title,
                                       const char *const *lines) {
    int scr_w = (int)fb_get_width();
    int scr_h = (int)fb_get_height();
    int nlines = 0;
    while (lines && lines[nlines]) nlines++;
    int w = 480;
    int h = 80 + nlines * 22;
    if (h < 220) h = 220;
    if (h > 460) h = 460;
    int x = (scr_w - w) / 2;
    int y = (scr_h - h) / 2;

    desktop_window_t *win = desktop_create_window(0, x, y, w, h, title);
    if (!win || !win->pixels) return;

    r_target_t wt;
    r_target_init(&wt, win->pixels, w, h, w);

    spinlock_lock(&win->content_lock);
    r_fill(&wt, 0, 0, w, h, 0xFF141824);
    r_fill(&wt, 0, 0, w, 3, DT_COL_ACCENT);
    r_fill(&wt, 24, 28, w - 48, 1, DT_COL_BORDER);

    int ty = 44;
    for (int i = 0; lines && lines[i]; i++) {
        r_text(&wt, 24, ty, lines[i], 0xFFE5E9F0);
        ty += 22;
        if (ty > h - 28) break;
    }
    spinlock_unlock(&win->content_lock);
    win->dirty = true;
    dt_dirty = true;
}

static void dt_launch_terminal(void) {
    /* Windowed Zeus session. Ctrl+F2 still switches to a full-screen tty. */
    kterm_open();
}

static void dt_launch_files(void) {
    files_open();
}

static void dt_launch_settings(void) {
    settings_app_open();
}

/* Try to launch a default graphical app from the Start menu.  If nothing on
 * disk works, open an "About inDevOS" kernel window instead so the user sees
 * the click did something. */
static void dt_launcher_spawn_default_app(void) {
    static const char *const candidates[] = {
        "/os/apps/sdl_user_demo",
        "/os/apps/sdl_user_demo.exe",
        "/os/apps/minimal_sdl_test",
        "/os/apps/minimal_sdl_test.exe",
        "/sdl_user_demo",
        "/sdl_user_demo.exe",
        "/minimal_sdl_test",
        "/minimal_sdl_test.exe",
        "sdl_user_demo",
        "sdl_user_demo.exe",
        "minimal_sdl_test",
        "minimal_sdl_test.exe",
        "/os/apps/hello",
        "/os/apps/hello.exe",
        "/hello",
        "/hello.exe",
        "hello",
        "hello.exe",
    };

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        int pid = exec_spawn(candidates[i], NULL);
        if (pid > 0) {
            serial_printf("[desktop] Start: spawned '%s' (pid %d)\n",
                          candidates[i], pid);
            return;
        }
    }
    serial_puts("[desktop] Start: no app found, opening About window\n");

    rtc_time_t t;
    rtc_read(&t);
    static char line_time[64];
    static char line_res[64];
    int n;

    n = 0;
    line_time[n++] = 'T'; line_time[n++] = 'i'; line_time[n++] = 'm';
    line_time[n++] = 'e'; line_time[n++] = ':'; line_time[n++] = ' ';
    line_time[n++] = '0' + (t.hour / 10);
    line_time[n++] = '0' + (t.hour % 10);
    line_time[n++] = ':';
    line_time[n++] = '0' + (t.minute / 10);
    line_time[n++] = '0' + (t.minute % 10);
    line_time[n++] = ':';
    line_time[n++] = '0' + (t.second / 10);
    line_time[n++] = '0' + (t.second % 10);
    line_time[n] = '\0';

    int sw = (int)fb_get_width();
    int sh = (int)fb_get_height();
    n = 0;
    const char *res_prefix = "Resolution: ";
    while (*res_prefix) line_res[n++] = *res_prefix++;
    /* itoa-ish, small ints */
    char tmp[8]; int ti = 0;
    int v = sw; if (v == 0) tmp[ti++] = '0';
    while (v) { tmp[ti++] = '0' + (v % 10); v /= 10; }
    while (ti--) line_res[n++] = tmp[ti];
    line_res[n++] = 'x';
    ti = 0; v = sh; if (v == 0) tmp[ti++] = '0';
    while (v) { tmp[ti++] = '0' + (v % 10); v /= 10; }
    while (ti--) line_res[n++] = tmp[ti];
    line_res[n] = '\0';

    static const char *lines[8];
    lines[0] = "Welcome to inDevOS";
    lines[1] = "";
    lines[2] = line_time;
    lines[3] = line_res;
    lines[4] = "";
    lines[5] = "Click dock buttons to launch demo apps.";
    lines[6] = "(spawn paths under /os/apps/)";
    lines[7] = NULL;
    dt_open_kernel_text_window("About inDevOS", lines);
}

static void dt_menubar_click(int mx, int my, int scr_w, int scr_h) {
    (void)scr_h;
    if (mx >= 8 && mx < 8 + DT_LOGO_W && my >= 5 && my < 27) {
        dt_start_open = !dt_start_open;
        dt_start_hover = -1;
        dt_dirty = true;
        return;
    }
    int bx = 8 + DT_LOGO_W + 12;
    int tab_y = (DESKTOP_MENUBAR_H - DT_TAB_H) / 2;
    spinlock_lock(&desktop_lock);
    for (desktop_window_t *w = win_bottom; w; w = w->next) {
        if (!w->visible && !w->minimized) continue;
        if (bx + DT_TAB_W > scr_w - 100) break;
        if (dt_point_in_rect(mx, my, bx, tab_y, DT_TAB_W, DT_TAB_H)) {
            spinlock_unlock(&desktop_lock);
            /* Unminimize first so a minimized fullscreen window comes
             * back. A visible fullscreen tab click leaves fullscreen. */
            if (w->minimized)
                dt_restore_window(w);
            else if (w->fullscreen)
                dt_toggle_fullscreen(w);
            desktop_focus_window(w);
            return;
        }
        bx += DT_TAB_W + 6;
    }
    spinlock_unlock(&desktop_lock);
}

static void dt_dock_click(int mx, int my, int scr_w, int scr_h) {
    int dx, dy, dw, dh;
    dt_dock_geom(scr_w, scr_h, &dx, &dy, &dw, &dh);
    if (!dt_point_in_rect(mx, my, dx, dy, dw, dh)) return;
    int tx = dx + DT_DOCK_PAD;
    int ty = dy + (dh - DT_DOCK_TILE) / 2;
    for (int d = 0; d < DT_DOCK_COUNT; d++) {
        if (dt_point_in_rect(mx, my, tx, ty, DT_DOCK_TILE, DT_DOCK_TILE)) {
            dt_dock_spawn(d);
            return;
        }
        tx += DT_DOCK_TILE + DESKTOP_DOCK_BTN_GAP;
    }
}

/* ===================================================================
 * Window decoration drawing
 * =================================================================== */

static void dt_draw_decoration(desktop_window_t *win, int scr_w, int scr_h) {
    if (!win->decorated) return;

    int fx, fy, fw, fh;
    dt_win_frame(win, &fx, &fy, &fw, &fh);

    uint32_t border_col = win->focused ? DT_COL_BORDER_FOCUS : DT_COL_BORDER;
    uint32_t titlebar_col = win->focused ? DT_COL_TITLEBAR_FOCUS : DT_COL_TITLEBAR_UNFOC;

    if (!dt_intersects_clip(fx - 2, fy, fw + 8, fh + 8))
        return;

    if (dt_rt_ready) {
        r_shadow(&dt_rt, fx, fy, fw, fh, DESKTOP_WIN_RAD, DESKTOP_SHADOW_PAD);
        r_rrect(&dt_rt, fx, fy, fw, DESKTOP_TITLEBAR_H + DESKTOP_WIN_RAD,
                DESKTOP_WIN_RAD, titlebar_col);
        r_fill(&dt_rt, fx, fy + DESKTOP_WIN_RAD, fw,
               DESKTOP_TITLEBAR_H - DESKTOP_WIN_RAD, titlebar_col);
        r_rrect_outline(&dt_rt, fx, fy, fw, fh, DESKTOP_WIN_RAD, border_col);
        r_fill(&dt_rt, fx + 1, fy + DESKTOP_TITLEBAR_H, fw - 2, 1,
               win->focused ? 0xFF243044 : DT_COL_BORDER);
    } else {
        dt_fill(titlebar_col, fx, fy, fw, DESKTOP_TITLEBAR_H);
        dt_fill(border_col, fx, fy, fw, 1);
    }

    static const uint32_t traffic_col[3] = {
        DT_COL_CLOSE_BTN, DT_COL_TRAFFIC_MIN, DT_COL_TRAFFIC_MAX
    };
    for (int i = 0; i < 3; i++) {
        int bx, by, bs;
        dt_traffic_rect(win, i, &bx, &by, &bs);
        int cy = by + bs / 2;
        int cr = bs / 2;
        uint32_t col = traffic_col[i];
        if (dt_hover_win_id == win->id && dt_hover_btn == i)
            col = 0xFFFFFFFF;
        if (dt_rt_ready)
            r_circle(&dt_rt, bx + cr, cy, cr, col);
        else
            dt_fill(col, bx, by, bs, bs);
    }

    int tbx, tby, tbs;
    dt_traffic_rect(win, 0, &tbx, &tby, &tbs);
    int title_x = tbx + 3 * (tbs + 8) + 4;
    dt_draw_text(title_x, fy + (DESKTOP_TITLEBAR_H - 16) / 2, win->title,
                 win->focused ? DT_COL_TEXT_WHITE : DT_COL_TEXT_GRAY, scr_w, scr_h);
}

/* Blit a window's client pixel buffer onto the framebuffer.
 * Opaque pixel runs are copied with memcpy (a full row in the common
 * fully-opaque case) instead of pixel-by-pixel stores. */
static void dt_blit_window(desktop_window_t *win, int scr_w, int scr_h) {
    if (!win->pixels) return;

    int cx = win->x + (win->decorated ? DESKTOP_BORDER_W : 0);
    int cy = win->y + (win->decorated ? DESKTOP_BORDER_W + DESKTOP_TITLEBAR_H : 0);
    uint32_t src_pitch_pix = win->pixel_pitch / 4;

    spinlock_lock(&win->content_lock);
    if (dt_rt_ready) {
        r_blit_keyed(&dt_rt, cx, cy, win->pixels,
                     win->content_w, win->content_h, (int)src_pitch_pix);
    } else if (dt_backbuffer) {
        int col0 = 0, col1 = win->content_w;
        int row0 = 0, row1 = win->content_h;
        if (cx + col0 < 0)           col0 = -cx;
        if (cx + col0 < dt_clip_x0)  col0 = dt_clip_x0 - cx;
        if (cx + col1 > scr_w)       col1 = scr_w - cx;
        if (cx + col1 > dt_clip_x1)  col1 = dt_clip_x1 - cx;
        if (cy + row0 < 0)           row0 = -cy;
        if (cy + row0 < dt_clip_y0)  row0 = dt_clip_y0 - cy;
        if (cy + row1 > scr_h)       row1 = scr_h - cy;
        if (cy + row1 > dt_clip_y1)  row1 = dt_clip_y1 - cy;
        if ((uint32_t)(cx + col1) > dt_bb_w) col1 = (int)dt_bb_w - cx;
        if ((uint32_t)(cy + row1) > dt_bb_h) row1 = (int)dt_bb_h - cy;

        for (int row = row0; row < row1; row++) {
            const uint32_t *src = win->pixels + (uint32_t)row * src_pitch_pix;
            uint32_t *dst_row = dt_backbuffer +
                                (uint32_t)(cy + row) * dt_bb_pitch_pix + cx;
            int col = col0;
            while (col < col1) {
                if ((src[col] & 0xFF000000) == 0) { col++; continue; }
                int run_start = col;
                while (col < col1 && (src[col] & 0xFF000000) != 0) col++;
                memcpy(dst_row + run_start, src + run_start,
                       (size_t)(col - run_start) * 4);
            }
        }
    } else {
        for (int row = 0; row < win->content_h; row++) {
            int sy = cy + row;
            if (sy < 0 || sy >= scr_h) continue;
            for (int col = 0; col < win->content_w; col++) {
                int sx = cx + col;
                if (sx < 0 || sx >= scr_w) continue;
                uint32_t pixel = win->pixels[row * src_pitch_pix + col];
                if ((pixel & 0xFF000000) == 0) continue;
                dt_putpixel(sx, sy, pixel);
            }
        }
    }
    spinlock_unlock(&win->content_lock);
}

/* ===================================================================
 * Compositor — called from the kernel main loop (~60 fps)
 * ===================================================================
 * Running in the main loop avoids all cooperative/preemptive thread
 * scheduling complexities.  The timer-driven preemptive scheduler
 * still handles Ring-3 user processes independently.
 */

/* ===================================================================
 * WM pointer pipeline — shared by poll_input and composite_frame
 * =================================================================== */

static desktop_window_t *dt_window_by_id(uint32_t id) {
    if (!id) return NULL;
    for (int i = 0; i < DESKTOP_MAX_WINDOWS; i++) {
        if (windows[i].id == id)
            return &windows[i];
    }
    return NULL;
}

static void dt_clear_pointer_grab(void) {
    spinlock_lock(&desktop_lock);
    for (desktop_window_t *w = win_bottom; w; w = w->next) {
        w->dragging = false;
        w->resize_edges = 0;
    }
    spinlock_unlock(&desktop_lock);
    dt_grab_id = 0;
}

static void dt_wm_apply_drag_resize(void) {
    if (!(dt_mouse_btn & 1))
        return;

    spinlock_lock(&desktop_lock);
    for (desktop_window_t *w = win_bottom; w; w = w->next) {
        if (w->dragging && !w->maximized) {
            dt_damage_window(w);
            w->x = dt_mouse_x - w->drag_off_x;
            w->y = dt_mouse_y - w->drag_off_y;
            if (w->y < DESKTOP_MENUBAR_H)
                w->y = DESKTOP_MENUBAR_H;
            dt_damage_window(w);
            dt_grab_id = w->id;
            dt_dirty = true;
            break;
        }
        if (w->resize_edges) {
            int dx = dt_mouse_x - w->resize_anchor_x;
            int dy = dt_mouse_y - w->resize_anchor_y;
            int nx = w->resize_orig_x;
            int ny = w->resize_orig_y;
            int nw = w->resize_orig_w;
            int nh = w->resize_orig_h;
            if (w->resize_edges & DT_EDGE_E) nw = w->resize_orig_w + dx;
            if (w->resize_edges & DT_EDGE_S) nh = w->resize_orig_h + dy;
            if (w->resize_edges & DT_EDGE_W) {
                nx = w->resize_orig_x + dx;
                nw = w->resize_orig_w - dx;
            }
            if (w->resize_edges & DT_EDGE_N) {
                ny = w->resize_orig_y + dy;
                nh = w->resize_orig_h - dy;
            }
            if (nw < DT_MIN_CONTENT_W) {
                if (w->resize_edges & DT_EDGE_W)
                    nx -= DT_MIN_CONTENT_W - nw;
                nw = DT_MIN_CONTENT_W;
            }
            if (nh < DT_MIN_CONTENT_H) {
                if (w->resize_edges & DT_EDGE_N)
                    ny -= DT_MIN_CONTENT_H - nh;
                nh = DT_MIN_CONTENT_H;
            }
            if (ny < DESKTOP_MENUBAR_H) {
                nh -= DESKTOP_MENUBAR_H - ny;
                ny = DESKTOP_MENUBAR_H;
            }
            dt_damage_window(w);
            w->x = nx;
            w->y = ny;
            dt_set_content_size(w, nw, nh);
            dt_damage_window(w);
            dt_grab_id = w->id;
            dt_dirty = true;
            break;
        }
    }
    spinlock_unlock(&desktop_lock);
}

/* Route pointer events to the focused window's client rect only.
 * Coordinates are client-relative. Chrome / grab swallows delivery. */
static void dt_deliver_client_pointer_events(bool btn1_down, bool btn1_up,
                                             bool btn2_down, bool btn2_up) {
    if (dt_grab_id)
        return; /* title-bar move / resize owns the pointer */

    desktop_window_t *fw = desktop_get_focused();
    if (!fw || !fw->pid)
        return;

    int cx, cy, cw, ch;
    dt_get_client_rect(fw, &cx, &cy, &cw, &ch);
    int rx = dt_mouse_x - cx;
    int ry = dt_mouse_y - cy;
    bool in_client = (dt_mouse_x >= cx && dt_mouse_x < cx + cw &&
                      dt_mouse_y >= cy && dt_mouse_y < cy + ch);

    if (fw->pid != dt_last_gui_pid) {
        dt_last_gui_rx = -1;
        dt_last_gui_ry = -1;
        dt_last_gui_pid = fw->pid;
    }

    if (!in_client) {
        dt_last_gui_rx = -1;
        dt_last_gui_ry = -1;
        return;
    }

    if (rx != dt_last_gui_rx || ry != dt_last_gui_ry) {
        dt_last_gui_rx = rx;
        dt_last_gui_ry = ry;
        gui_event_t ev = { GUI_EVENT_MOUSE_MOVE, rx, ry, 0 };
        process_gui_enqueue_by_pid(fw->pid, &ev);
    }
    if (btn1_down) {
        gui_event_t ev = { GUI_EVENT_MOUSE_DOWN, rx, ry, 1 };
        process_gui_enqueue_by_pid(fw->pid, &ev);
        gui_event_t click = { GUI_EVENT_CLICK, rx, ry, 1 };
        process_gui_enqueue_by_pid(fw->pid, &click);
    }
    if (btn1_up) {
        gui_event_t ev = { GUI_EVENT_MOUSE_UP, rx, ry, 1 };
        process_gui_enqueue_by_pid(fw->pid, &ev);
    }
    if (btn2_down) {
        gui_event_t ev = { GUI_EVENT_MOUSE_DOWN, rx, ry, 2 };
        process_gui_enqueue_by_pid(fw->pid, &ev);
        gui_event_t rc = { GUI_EVENT_RIGHT_CLICK, rx, ry, 2 };
        process_gui_enqueue_by_pid(fw->pid, &rc);
    }
    if (btn2_up) {
        gui_event_t ev = { GUI_EVENT_MOUSE_UP, rx, ry, 2 };
        process_gui_enqueue_by_pid(fw->pid, &ev);
    }
}

static void dt_wm_super_toggle(void) {
    static bool prev_super;
    bool now_super = keyboard_get_super();
    if (now_super && !prev_super) {
        dt_start_open = !dt_start_open;
        dt_start_hover = -1;
        dt_switcher_open = false;
        dt_dirty = true;
    }
    prev_super = now_super;
}

/** Sample mouse, run WM chrome/focus, drag/resize, and client routing. */
static void dt_wm_pointer_frame(void) {
    mouse_get_position(&dt_mouse_x, &dt_mouse_y);
    uint8_t new_btn = mouse_get_buttons();
    dt_prev_btn = dt_mouse_btn;
    dt_mouse_btn = new_btn;

    bool btn1_down = (dt_mouse_btn & 1) && !(dt_prev_btn & 1);
    bool btn1_up   = !(dt_mouse_btn & 1) && (dt_prev_btn & 1);
    bool btn2_down = (dt_mouse_btn & 2) && !(dt_prev_btn & 2);
    bool btn2_up   = !(dt_mouse_btn & 2) && (dt_prev_btn & 2);

    /* While grabbed, ignore raise/focus retargeting from further downs. */
    if (btn1_down && !dt_grab_id)
        desktop_handle_mouse(dt_mouse_x, dt_mouse_y, dt_mouse_btn);
    if (btn1_up)
        dt_clear_pointer_grab();

    dt_wm_apply_drag_resize();
    dt_deliver_client_pointer_events(btn1_down, btn1_up, btn2_down, btn2_up);
}

void desktop_poll_input(void) {
    if (!desktop_running) return;
    dt_wm_super_toggle();
    dt_wm_pointer_frame();
}

int desktop_count_visible_windows(void) {
    int n = 0;
    spinlock_lock(&desktop_lock);
    for (desktop_window_t *w = win_bottom; w; w = w->next) {
        if (w->visible && !w->minimized)
            n++;
    }
    spinlock_unlock(&desktop_lock);
    return n;
}

bool desktop_wm_needs_composite(void) {
    if (!desktop_running)
        return false;
    if (dt_dirty)
        return true;
    if (dt_start_open || dt_switcher_open)
        return true;
    if (dt_grab_id)
        return true;

    int nvis = 0;
    bool kernel_chrome = false;
    spinlock_lock(&desktop_lock);
    for (desktop_window_t *w = win_bottom; w; w = w->next) {
        if (!w->visible || w->minimized)
            continue;
        nvis++;
        if (w->pid == 0)
            kernel_chrome = true;
        if (w->dragging || w->resize_edges) {
            spinlock_unlock(&desktop_lock);
            return true;
        }
    }
    spinlock_unlock(&desktop_lock);

    /* Multi-window (or none): must stack wallpaper + chrome. */
    if (nvis != 1)
        return true;
    /* Kernel-owned windows have no fast-present path. */
    if (kernel_chrome)
        return true;

    desktop_window_t *fw = desktop_get_focused();
    if (!fw || fw->pid == 0)
        return true;

    /* Single focused Ring-3 client: allow client fast-present; WM still
     * runs via desktop_poll_input() for focus/chrome/input. */
    return false;
}

void desktop_composite_frame(void) {
    if (!desktop_running) return;

    /* Throttle to ~60 fps (every 16 ticks at 1 kHz HPET).
     *
     * Fallback: if HPET ticks aren't advancing, the simple "now - dt_last_frame"
     * check would skip every frame after the first because (now == dt_last_frame)
     * forever.  In that case, force a frame after ~150 calls so we still get a
     * usable refresh rate (compositor work itself is the real throttle). */
    uint64_t now = hpet_get_ticks();
    static uint64_t skipped_since_advance = 0;
    bool throttle_says_skip = (now - dt_last_frame) < 16;
    bool hpet_stalled       = (now == dt_last_frame) && skipped_since_advance > 150;
    if (throttle_says_skip && !hpet_stalled) {
        skipped_since_advance++;
        /* Still run WM input so click/focus/drag are not lost between
         * 60 Hz composite ticks. */
        dt_wm_super_toggle();
        dt_wm_pointer_frame();
        return;
    }
    if (hpet_stalled) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            serial_printf("[desktop] WARNING: HPET ticks not advancing "
                          "(stuck at %llu) — falling back to call-count throttle\n",
                          (unsigned long long)now);
        }
    }
    skipped_since_advance = 0;
    dt_last_frame = now;

    /* Lazy backbuffer alloc — first call after mem is initialised. */
    dt_ensure_backbuffer();

    int scr_w = (int)fb_get_width();
    int scr_h = (int)fb_get_height();

    /* WM pointer pipeline (shared with desktop_poll_input). */
    static int     dt_last_mouse_x = -1, dt_last_mouse_y = -1;
    static uint8_t dt_last_mouse_btn_seen = 0;
    dt_wm_super_toggle();
    dt_wm_pointer_frame();

    /* Skip the entire render pass if nothing has changed.  This is the main
     * cure for the low FPS / wasted bandwidth: a 1920x1080 redraw + memcpy
     * costs ~16 MB/frame; doing it only when needed keeps the desktop snappy
     * and stops idle flicker on tear-prone backends. */
    bool mouse_moved   = (dt_mouse_x != dt_last_mouse_x) ||
                         (dt_mouse_y != dt_last_mouse_y);
    if (dt_start_open && mouse_moved) {
        int hov = dt_start_hit(dt_mouse_x, dt_mouse_y);
        if (hov != dt_start_hover) {
            dt_start_hover = hov;
            dt_dirty = true;
        }
    }
    bool buttons_diff  = (dt_mouse_btn != dt_last_mouse_btn_seen);
    bool clock_changed = false;
    {
        uint64_t ms = hpet_get_ticks();
        if (ms - dt_last_rtc_poll_ms >= DT_RTC_POLL_MS) {
            dt_last_rtc_poll_ms = ms;
            rtc_read(&dt_panel_rtc_cache);
            uint64_t pack =
                ((uint64_t)dt_panel_rtc_cache.hour << 16) |
                ((uint64_t)dt_panel_rtc_cache.minute << 8) |
                (uint64_t)dt_panel_rtc_cache.second;
            if (pack != dt_last_clock) {
                dt_last_clock = pack;
                clock_changed = true;
            }
        }
    }
    /* Reset per-frame damage accumulation */
    dt_dmg_any = false;

    /* Caret blink / notepad pixels only dirty that window — never the
     * whole desktop (that used to force a full-screen recomposite). */
    notepad_tick();
    files_tick();
    kterm_tick();
    settings_app_tick();

    bool any_window_dirty = false;
    spinlock_lock(&desktop_lock);
    for (desktop_window_t *w = win_bottom; w; w = w->next) {
        if (w->dirty) {
            any_window_dirty = true;
            int fx, fy, fw, fh;
            dt_win_frame(w, &fx, &fy, &fw, &fh);
            dt_damage_add(fx - 4, fy, fw + DESKTOP_SHADOW_PAD, fh + DESKTOP_SHADOW_PAD);
        }
    }
    spinlock_unlock(&desktop_lock);

    bool need_render = dt_dirty || mouse_moved || buttons_diff ||
                       clock_changed || any_window_dirty;
    if (!need_render) {
        return;
    }

    /* Cursor damage: restore scene under old position, draw at new one.
     * (Cursor sprite is at most 10x16; pad slightly.) */
    if (mouse_moved || buttons_diff) {
        dt_damage_add(dt_last_mouse_x - 1, dt_last_mouse_y - 1, 14, 20);
        dt_damage_add(dt_mouse_x - 1, dt_mouse_y - 1, 14, 20);
    }
    if (clock_changed) {
        dt_damage_add(scr_w - 96, 0, 96, DESKTOP_MENUBAR_H);
    }
    /* Full-screen only when the scene graph changed (create/destroy/focus).
     * Clicks and caret blinks use the accumulated dirty rects. */
    bool full_damage = dt_dirty;

    dt_last_mouse_x        = dt_mouse_x;
    dt_last_mouse_y        = dt_mouse_y;
    dt_last_mouse_btn_seen = dt_mouse_btn;

    /* Hover highlight for traffic-light buttons. */
    {
        desktop_window_t *hw = desktop_window_at(dt_mouse_x, dt_mouse_y);
        int hb = (hw && hw->decorated && !hw->fullscreen) ? dt_hit_traffic(hw, dt_mouse_x, dt_mouse_y) : -1;
        uint32_t hid = hw ? hw->id : 0;
        if (hb != dt_hover_btn || hid != dt_hover_win_id) {
            dt_hover_btn = hb;
            dt_hover_win_id = hid;
            if (hw) dt_damage_window(hw);
        }
    }

    /* ---- Determine the frame's damage region and set the clip rect ---- */
    int dmg_x0, dmg_y0, dmg_x1, dmg_y1;
    if (full_damage || !dt_dmg_any || !dt_backbuffer) {
        /* Full-screen recomposite (window create/destroy/focus, first frame,
         * or direct-FB fallback where partial restore isn't possible). */
        dmg_x0 = 0; dmg_y0 = 0; dmg_x1 = scr_w; dmg_y1 = scr_h;
        dt_clip_clear();
    } else {
        dmg_x0 = dt_dmg_x0 < 0 ? 0 : dt_dmg_x0;
        dmg_y0 = dt_dmg_y0 < 0 ? 0 : dt_dmg_y0;
        dmg_x1 = dt_dmg_x1 > scr_w ? scr_w : dt_dmg_x1;
        dmg_y1 = dt_dmg_y1 > scr_h ? scr_h : dt_dmg_y1;
        if (dmg_x0 >= dmg_x1 || dmg_y0 >= dmg_y1) {
            dt_clip_clear();
            return;  /* damage entirely off-screen */
        }
        dt_clip_set(dmg_x0, dmg_y0, dmg_x1, dmg_y1);
    }

    /* ---- Render (into dt_backbuffer if available; otherwise direct FB) ----
     * Everything is drawn back-to-front as before, but the clip rect means
     * only pixels inside the damaged region are actually written. */
    dt_rt_bind();
    dt_draw_wallpaper(scr_w, scr_h);
    if (dt_intersects_clip(0, DESKTOP_MENUBAR_H, scr_w,
                           scr_h - DESKTOP_MENUBAR_H - DESKTOP_PANEL_H))
        dt_draw_desktop_icons(scr_w, scr_h);

    spinlock_lock(&desktop_lock);
    for (desktop_window_t *w = win_bottom; w; w = w->next) {
        if (!w->visible || w->minimized) continue;
        if (w->fullscreen && w->pixels && dt_backbuffer) {
            /* No window chrome. Client fills everything under the menubar;
             * the dock is omitted below so it cannot sit on top of the game. */
            dt_scale_client_to(w, dt_backbuffer, (int)dt_bb_pitch_pix,
                               0, DESKTOP_MENUBAR_H, scr_w,
                               scr_h - DESKTOP_MENUBAR_H,
                               dt_clip_x0, dt_clip_y0, dt_clip_x1, dt_clip_y1);
        } else {
            int fx, fy, fw, fh;
            dt_win_frame(w, &fx, &fy, &fw, &fh);
            if (dt_intersects_clip(fx - 2, fy, fw + 8, fh + 8)) {
                dt_draw_decoration(w, scr_w, scr_h);
                dt_blit_window(w, scr_w, scr_h);
            }
        }
        w->dirty = false;
    }
    bool any_fullscreen = false;
    for (desktop_window_t *w = win_bottom; w; w = w->next) {
        if (w->visible && !w->minimized && w->fullscreen)
            any_fullscreen = true;
    }
    spinlock_unlock(&desktop_lock);

    dt_draw_menubar(scr_w, scr_h);
    if (!any_fullscreen)
        dt_draw_panel(scr_w, scr_h);
    dt_draw_start_menu(scr_w, scr_h);
    dt_draw_switcher(scr_w, scr_h);
    dt_draw_cursor(dt_mouse_x, dt_mouse_y, scr_w, scr_h);

    dt_dirty = false;
    dt_clip_clear();

    /* Copy only the damaged rect backbuffer -> Limine FB so on-screen scanout
     * sees a complete, consistent frame without full-screen traffic. */
    dt_present_backbuffer(dmg_x0, dmg_y0, dmg_x1, dmg_y1);

    /* Forward to virtio-GPU (or other HAL) when its scanout is a separate
     * buffer.  Use the backbuffer if we have one (already complete & same
     * pitch as fb).  Falls back to fb_get_composite_buffer otherwise. */
    {
        gpu_surface_t *hal = gpu_hal_get_primary_surface();
        const void *src;
        uint32_t src_pitch;
        if (dt_backbuffer) {
            src = dt_backbuffer;
            src_pitch = dt_bb_pitch_pix * 4;
        } else {
            src = fb_get_composite_buffer();
            src_pitch = fb ? fb->pitch : 0;
        }
        if (hal && hal->pixels && fb && src && src_pitch &&
            hal->pixels != (uint32_t *)fb->address) {
            uint32_t fw = fb->width < hal->width  ? fb->width  : hal->width;
            uint32_t fh = fb->height < hal->height ? fb->height : hal->height;
            int hx0 = dmg_x0, hy0 = dmg_y0;
            int hx1 = dmg_x1 < (int)fw ? dmg_x1 : (int)fw;
            int hy1 = dmg_y1 < (int)fh ? dmg_y1 : (int)fh;
            const uint8_t *srcb = (const uint8_t *)src;
            uint8_t *dst = (uint8_t *)hal->pixels;
            for (int row = hy0; row < hy1; row++) {
                if (hx0 >= hx1) break;
                memcpy(dst + (uint32_t)row * hal->pitch + (uint32_t)hx0 * 4,
                       srcb + (uint32_t)row * src_pitch + (uint32_t)hx0 * 4,
                       (size_t)(hx1 - hx0) * 4);
            }
        }
    }
    gpu_hal_present_rect((uint32_t)dmg_x0, (uint32_t)dmg_y0,
                         (uint32_t)(dmg_x1 - dmg_x0),
                         (uint32_t)(dmg_y1 - dmg_y0));

    /* With a compositor backbuffer the frame is already on the real
     * framebuffer (dt_present_backbuffer above) and on the GPU scanout
     * (gpu_hal_present_rect above).
     *
     * Do NOT call fb_flush() in that case: fb_flush() copies from the *console*
     * backbuffer, which still holds the pre-desktop screen (boot log / shell
     * prompt), so flushing here painted the old console text straight back over
     * the freshly composited frame — starting the desktop looked like a no-op
     * except for the rows of the last damage rect.
     *
     * The direct-draw fallback (no dt_backbuffer) really does need it, because
     * there putpixel/fillrect write into the console backbuffer. */
    if (!dt_backbuffer) {
        fb_mark_dirty_rows((uint32_t)dmg_y0, (uint32_t)(dmg_y1 - dmg_y0));
        fb_flush();
    }
}

void desktop_present_now(void) {
    if (!desktop_running) return;
    dt_last_frame = 0;
    desktop_composite_frame();
}

/* Force the next frame to be a full-screen recomposite and run it now.
 * Needed when something outside the compositor painted the framebuffer —
 * a full-screen Ring-3 app, or the console — because the damage tracker has
 * no idea those pixels changed. */
void desktop_force_full_repaint(void) {
    if (!desktop_running) return;
    dt_dirty = true;
    dt_last_frame = 0;
    desktop_composite_frame();
}

bool desktop_present_window_now(uint32_t pid) {
    desktop_window_t *win = desktop_find_window_by_pid(pid);
    if (!win)
        return false;
    if (dt_present_window_fast(win))
        return true;
    desktop_present_now();
    return true;
}

/* ===================================================================
 * Window management
 * =================================================================== */

#define DT_START_W 268
#define DT_START_ITEM_H 32
#define DT_START_PAD 10

static void dt_theme_reload_inner(void) {
    uint32_t accent = 0x3DDC97;
    int light = 0;
    const display_settings_t *ds = settings_get_display();
    if (ds) {
        light = (ds->theme == THEME_LIGHT);
        if (ds->accent)
            accent = 0xFF000000u | (ds->accent & 0x00FFFFFFu);
    }
    dt_pal.accent = accent;
    if (light) {
        dt_pal.wallpaper      = 0xFFE8EEF4;
        dt_pal.wallpaper_bot  = 0xFFD5DEE8;
        dt_pal.titlebar_focus = 0xFFF4F6FA;
        dt_pal.titlebar_unfoc = 0xFFE4E8F0;
        dt_pal.border         = 0xFFC5CDD8;
        dt_pal.border_focus   = accent;
        dt_pal.panel_bg       = 0xE6F4F6FA;
        dt_pal.panel_active   = 0xFFE8EEF6;
        dt_pal.menubar_bg     = 0xFFF4F6FA;
        dt_pal.dock_btn       = 0xFFE8ECF2;
        dt_pal.text_white     = 0xFF1A2230;
        dt_pal.text_gray      = 0xFF5B677A;
        dt_pal.surface        = 0xFFF8FAFC;
    } else {
        dt_pal.wallpaper      = 0xFF0B1020;
        dt_pal.wallpaper_bot  = 0xFF0A2430;
        dt_pal.titlebar_focus = 0xFF1A2035;
        dt_pal.titlebar_unfoc = 0xFF141824;
        dt_pal.border         = 0xFF2A3148;
        dt_pal.border_focus   = accent;
        dt_pal.panel_bg       = 0xE612161E;
        dt_pal.panel_active   = 0xFF1E2740;
        dt_pal.menubar_bg     = 0xFF0E121A;
        dt_pal.dock_btn       = 0xFF1C2436;
        dt_pal.text_white     = 0xFFF4F4F5;
        dt_pal.text_gray      = 0xFF94A3B8;
        dt_pal.surface        = 0xFF12161E;
    }
}

void desktop_request_redraw(void) {
    dt_dirty = true;
    dt_damage_add(0, 0, (int)fb_get_width(), (int)fb_get_height());
}

void desktop_theme_reload(void) {
    dt_theme_reload_inner();
    if (desktop_running) {
        dt_dirty = true;
        dt_damage_add(0, 0, (int)fb_get_width(), (int)fb_get_height());
    }
}

static void dt_start_geom(int *x, int *y, int *w, int *h) {
    int items = DT_DOCK_COUNT + 2;
    *x = 8;
    *y = DESKTOP_MENUBAR_H;
    *w = DT_START_W;
    *h = 36 + items * DT_START_ITEM_H + DT_START_PAD;
}

static int dt_collect_windows(desktop_window_t **out, int max) {
    int n = 0;
    spinlock_lock(&desktop_lock);
    for (desktop_window_t *w = win_bottom; w; w = w->next) {
        if (w->id == 0) continue;
        if (n < max) out[n++] = w;
    }
    spinlock_unlock(&desktop_lock);
    return n;
}

extern bool sdl_destroy_window_sharing_pixels(uint32_t *pixels);
extern int  process_terminate(uint32_t pid, int exit_code);

static void dt_toggle_fullscreen(desktop_window_t *w) {
    if (!w || w->id == 0)
        return;
    if (w->minimized)
        dt_restore_window(w);
    /* Presentation mode only. Do not resize or reallocate win->pixels:
     * SDL shares that buffer, and Doom queries embed size once at startup.
     * Stretching at present time keeps restore a flag flip. */
    w->fullscreen = !w->fullscreen;
    w->maximized = false;
    w->dragging = false;
    w->resize_edges = 0;
    dt_dirty = true;
    dt_damage_add(0, 0, (int)fb_get_width(), (int)fb_get_height());
}

static void dt_close_window(desktop_window_t *w) {
    if (!w || w->id == 0)
        return;

    uint32_t pid = w->pid;
    if (pid) {
        gui_event_t ev;
        ev.type = GUI_EVENT_CLOSE;
        ev.arg1 = ev.arg2 = ev.arg3 = 0;
        process_gui_enqueue_by_pid(pid, &ev);
    }

    /* Stop presenting before freeing pixels. A client blocked in
     * SYS_DESKTOP_COMPOSITE re-checks these flags. */
    w->visible = false;
    w->minimized = true;
    w->fullscreen = false;
    w->dragging = false;
    w->resize_edges = 0;
    if (dt_grab_id == w->id)
        dt_grab_id = 0;

    for (int i = 0; i < 8; i++) {
        if (pid && (uint32_t)dt_dock_spawned_pid[i] == pid)
            dt_dock_spawned_pid[i] = 0;
    }

    uint32_t *pix = w->pixels;
    if (sdl_destroy_window_sharing_pixels(pix))
        w->pixels = NULL; /* surface free owned the buffer */
    desktop_destroy_window(w);
    dt_dirty = true;
    dt_damage_add(0, 0, (int)fb_get_width(), (int)fb_get_height());

    if (!pid)
        return;

    keyboard_restore_shell_input();

    process_t *sync = exec_user_as_process();
    process_t *cur = get_current_process();
    if (exec_current_is_sync_program() && sync && sync->pid == pid) {
        /* Blocking exec_file (shell `doom`): killing the thread would take
         * the shell with it. Ask the timer to unwind, and if this stack is
         * that program's syscall, leave now. */
        set_terminate_flag(true);
        if (cur && cur->pid == pid)
            exec_force_terminate(0);
        return;
    }
    /* Dock/spawned Doom handles the click inside its own present
     * syscall (int 0x80). process_terminate only marks the current
     * process a zombie and returns, so the game keeps running. */
    if (cur && cur->pid == pid && cur->sched_thread)
        exec_spawned_exit_int80(0);
    process_terminate(pid, 0);
}

static void dt_try_load_dock_icons(void) {
    static const char *const paths[] = {
        "/os/icons/note.png", "/os/icons/term.png", "/os/icons/files.png",
        "/os/icons/settings.png", "/os/icons/doom.png"
    };
    if (dt_dock_icons_tried) return;
    dt_dock_icons_tried = true;
    for (int i = 0; i < DT_DOCK_COUNT && i < 5; i++) {
        int fd = vfs_open(paths[i], VFS_O_READ);
        if (fd < 0) continue;
        uint8_t *buf = (uint8_t *)kalloc(64 * 1024);
        if (!buf) { vfs_close(fd); continue; }
        uint32_t len = 0;
        while (len < 64 * 1024) {
            int r = vfs_read(fd, buf + len, 64 * 1024 - len);
            if (r <= 0) break;
            len += (uint32_t)r;
        }
        vfs_close(fd);
        png_image_t *img = png_decode(buf, len);
        kfree(buf);
        if (!img || !img->valid || !img->pixels) {
            if (img) png_free(img);
            continue;
        }
        dt_dock_icon[i] = img->pixels;
        dt_dock_icon_w[i] = img->width;
        dt_dock_icon_h[i] = img->height;
        img->pixels = NULL;
        png_free(img);
    }
}

static void dt_draw_dock_glyph(int x, int y, int slot, uint32_t col) {
    if (dt_rt_ready)
        r_rrect(&dt_rt, x, y, 28, 28, 6, DT_COL_DOCK_BTN);
    else
        dt_fill(DT_COL_DOCK_BTN, x, y, 28, 28);
    switch (slot) {
    case 0: /* note */
        if (dt_rt_ready) {
            r_rrect(&dt_rt, x + 6, y + 4, 16, 20, 2, 0xFFF8FAFC);
            r_fill(&dt_rt, x + 8, y + 8, 12, 2, col);
            r_fill(&dt_rt, x + 8, y + 12, 12, 2, col);
            r_fill(&dt_rt, x + 8, y + 16, 8, 2, col);
        }
        break;
    case 1: /* term */
        if (dt_rt_ready) {
            r_rrect(&dt_rt, x + 4, y + 6, 20, 16, 3, 0xFF0B1020);
            r_text(&dt_rt, x + 6, y + 8, ">_", col);
        }
        break;
    case 2: /* files folder */
        if (dt_rt_ready) {
            r_rrect(&dt_rt, x + 5, y + 10, 18, 12, 2, col);
            r_fill(&dt_rt, x + 7, y + 8, 8, 4, col);
        }
        break;
    case 3: /* settings gear */
        if (dt_rt_ready) {
            r_circle(&dt_rt, x + 14, y + 14, 6, col);
            r_circle(&dt_rt, x + 14, y + 14, 2, DT_COL_DOCK_BTN);
        }
        break;
    default: /* doom-ish */
        if (dt_rt_ready)
            r_circle(&dt_rt, x + 14, y + 14, 8, col);
        break;
    }
}

static void dt_blit_icon(int dx, int dy, int dw, int dh, const uint32_t *src, int sw, int sh) {
    if (!src || sw <= 0 || sh <= 0 || !dt_rt_ready) return;
    for (int y = 0; y < dh; y++) {
        int sy = y * sh / dh;
        for (int x = 0; x < dw; x++) {
            int sx = x * sw / dw;
            uint32_t p = src[sy * sw + sx];
            if ((p >> 24) == 0) continue;
            r_blend(&dt_rt, dx + x, dy + y, p | 0xFF000000u);
        }
    }
}

static void dt_draw_start_menu(int scr_w, int scr_h) {
    (void)scr_w;
    if (!dt_start_open) return;
    int sx, sy, sw, sh;
    dt_start_geom(&sx, &sy, &sw, &sh);
    if (sy + sh > scr_h - DESKTOP_PANEL_H)
        sh = scr_h - DESKTOP_PANEL_H - sy;
    if (dt_rt_ready) {
        r_shadow(&dt_rt, sx, sy, sw, sh, 10, 8);
        r_rrect(&dt_rt, sx, sy, sw, sh, 10, DT_COL_SURFACE);
        r_rrect_outline(&dt_rt, sx, sy, sw, sh, 10, DT_COL_BORDER);
        r_fill(&dt_rt, sx + 10, sy + 10, 4, 16, DT_COL_ACCENT);
        r_text(&dt_rt, sx + 20, sy + 10, "Start", DT_COL_TEXT_WHITE);
    } else {
        dt_fill(DT_COL_SURFACE, sx, sy, sw, sh);
        dt_draw_text(sx + 16, sy + 10, "Start", DT_COL_TEXT_WHITE, scr_w, scr_h);
    }
    int iy = sy + 34;
    for (int i = 0; i < DT_DOCK_COUNT + 2; i++) {
        bool hover = (dt_start_hover == i);
        uint32_t bg = hover ? DT_COL_PANEL_ACTIVE : DT_COL_SURFACE;
        if (dt_rt_ready)
            r_rrect(&dt_rt, sx + 8, iy, sw - 16, DT_START_ITEM_H - 4, 6, bg);
        const char *label;
        if (i < DT_DOCK_COUNT)
            label = dt_dock_apps[i].label;
        else if (i == DT_DOCK_COUNT)
            label = (settings_get_display() && settings_get_display()->theme == THEME_LIGHT)
                    ? "Theme: Light" : "Theme: Dark";
        else
            label = "About inDevOS";
        if (i < DT_DOCK_COUNT) {
            dt_draw_dock_glyph(sx + 14, iy + 2, i, DT_COL_ACCENT);
            dt_draw_text(sx + 48, iy + 6, label, DT_COL_TEXT_WHITE, scr_w, scr_h);
        } else {
            dt_draw_text(sx + 18, iy + 6, label, DT_COL_TEXT_WHITE, scr_w, scr_h);
        }
        iy += DT_START_ITEM_H;
    }
}

static void dt_draw_switcher(int scr_w, int scr_h) {
    if (!dt_switcher_open) return;
    desktop_window_t *list[DESKTOP_MAX_WINDOWS];
    int n = dt_collect_windows(list, DESKTOP_MAX_WINDOWS);
    if (n <= 0) return;
    if (dt_switcher_sel < 0) dt_switcher_sel = 0;
    if (dt_switcher_sel >= n) dt_switcher_sel = n - 1;
    int sw = 420, sh = 28 + n * 26;
    int sx = (scr_w - sw) / 2, sy = (scr_h - sh) / 3;
    if (dt_rt_ready) {
        r_shadow(&dt_rt, sx, sy, sw, sh, 10, 10);
        r_rrect(&dt_rt, sx, sy, sw, sh, 10, DT_COL_SURFACE);
        r_rrect_outline(&dt_rt, sx, sy, sw, sh, 10, DT_COL_ACCENT);
        r_text(&dt_rt, sx + 16, sy + 8, "Windows", DT_COL_TEXT_GRAY);
    } else {
        dt_fill(DT_COL_SURFACE, sx, sy, sw, sh);
    }
    for (int i = 0; i < n; i++) {
        int iy = sy + 26 + i * 26;
        if (i == dt_switcher_sel) {
            if (dt_rt_ready)
                r_rrect(&dt_rt, sx + 10, iy, sw - 20, 22, 4, DT_COL_PANEL_ACTIVE);
            else
                dt_fill(DT_COL_PANEL_ACTIVE, sx + 10, iy, sw - 20, 22);
        }
        dt_draw_text(sx + 18, iy + 3, list[i]->title[0] ? list[i]->title : "Window",
                     DT_COL_TEXT_WHITE, scr_w, scr_h);
    }
}

static void dt_start_activate(int idx) {
    if (idx < 0) return;
    if (idx < DT_DOCK_COUNT) {
        dt_start_open = false;
        dt_dock_spawn(idx);
        return;
    }
    if (idx == DT_DOCK_COUNT) {
        char cur[16];
        if (settings_get("display.theme", cur, sizeof(cur)) != 0)
            cur[0] = 'd';
        settings_set("display.theme", (cur[0] == 'l') ? "dark" : "light");
        settings_apply();
        return;
    }
    dt_start_open = false;
    {
        static const char *lines[] = {
            "Welcome to inDevOS",
            "Alt-Tab   switch windows",
            "Super     start menu",
            "Alt-F4    close window",
            "Ctrl+F1-F4 / Ctrl+Q  ttys",
            NULL
        };
        dt_open_kernel_text_window("About inDevOS", lines);
    }
}

static int dt_start_hit(int mx, int my) {
    if (!dt_start_open) return -1;
    int sx, sy, sw, sh;
    dt_start_geom(&sx, &sy, &sw, &sh);
    if (!dt_point_in_rect(mx, my, sx, sy, sw, sh)) return -2;
    int iy = sy + 34;
    for (int i = 0; i < DT_DOCK_COUNT + 2; i++) {
        if (dt_point_in_rect(mx, my, sx + 8, iy, sw - 16, DT_START_ITEM_H - 4))
            return i;
        iy += DT_START_ITEM_H;
    }
    return -1;
}

void desktop_init(void) {
    memset(windows, 0, sizeof(windows));
    desktop_lock.lock = 0;
    win_bottom = win_top = NULL;
    desktop_running = false;
    next_win_id = 1;
    dt_theme_reload_inner();
    serial_puts("[desktop] initialised\n");
}

/* ===================================================================
 * Desktop compositor
 * ===================================================================
 * The timer scheduler can safely switch Ring-3 iret frames, but it does
 * not yet switch kernel interrupt stacks for Ring-0 threads. Running the
 * compositor from the main loop avoids resuming a kernel thread on the
 * wrong stack while still keeping the desktop responsive at idle.
 */
static thread_t *desktop_thread = NULL;

static void dt_apply_keyboard_focus(void) {
    keyboard_set_raw_mode(desktop_focus_eats_keys());
}

static void desktop_thread_main(void *arg) {
    (void)arg;
    while (desktop_running) {
        /* Drain SDL key events queued by the keyboard ISR. The fast
         * state-only update path (SDL_InjectKeyState_Fast) already
         * happens from the ISR for polling apps; this drains the full
         * event queue + desktop GUI notifications which require a
         * thread context (event_queue spinlock). */
        if (keyboard_has_sdl_pending())
            keyboard_process_sdl_events();

        desktop_composite_frame();

        /* Sleep ~16 ms between frames (~60 fps target). The compositor
         * itself also early-returns when nothing is dirty; the sleep
         * just ensures we don't spin between vsyncs. */
        thread_sleep(16);
    }
}

void desktop_start(void) {
    if (desktop_running) return;
    desktop_running = true;
    dt_theme_reload_inner();
    dt_try_load_dock_icons();
    dt_dirty = true;
    dt_last_frame = 0;
    rtc_read(&dt_panel_rtc_cache);
    dt_last_rtc_poll_ms = hpet_get_ticks();
    {
        uint64_t pack =
            ((uint64_t)dt_panel_rtc_cache.hour << 16) |
            ((uint64_t)dt_panel_rtc_cache.minute << 8) |
            (uint64_t)dt_panel_rtc_cache.second;
        dt_last_clock = pack;
    }
    dt_refresh_desktop_icons();
    mouse_sync_screen_bounds();

    desktop_thread = NULL;
    /* Taking over the display: damage the whole screen so the first frame
     * replaces the console text (dt_dirty alone has no damage rect). */
    desktop_request_redraw();
    serial_puts("[desktop] started — compositor running in kernel main loop\n");
}

void desktop_stop(void) {
    desktop_running = false;
    /* The compositor thread observes desktop_running on each iteration
     * and exits cleanly via thread_exit(). It will be reaped by the
     * scheduler. */
    desktop_thread = NULL;

    if (dt_wallpaper_pixels) {
        kfree(dt_wallpaper_pixels);
        dt_wallpaper_pixels = NULL;
        dt_wallpaper_w = dt_wallpaper_h = 0;
    }
    dt_wallpaper_tried = false;

    /* Destroy all windows */
    spinlock_lock(&desktop_lock);
    for (int i = 0; i < DESKTOP_MAX_WINDOWS; i++) {
        if (windows[i].id != 0) {
            if (windows[i].pixels) {
                kfree(windows[i].pixels);
                windows[i].pixels = NULL;
            }
            windows[i].id = 0;
        }
    }
    win_bottom = win_top = NULL;
    dt_focused_id = 0;
    dt_grab_id = 0;
    spinlock_unlock(&desktop_lock);

    /* Restore normal terminal output path */
    extern void term_set_output_hook(void (*hook)(const char *, size_t));
    term_set_output_hook(NULL);

    /* Redraw the console screen */
    extern void clear_screen(void);
    clear_screen();
}

bool desktop_has_backbuffer(void) {
    return desktop_running && dt_backbuffer != NULL;
}

bool desktop_is_running(void) {
    return desktop_running;
}

desktop_window_t *desktop_create_window(uint32_t pid, int x, int y,
                                        int w, int h, const char *title) {
    spinlock_lock(&desktop_lock);

    desktop_window_t *win = NULL;
    for (int i = 0; i < DESKTOP_MAX_WINDOWS; i++) {
        if (windows[i].id == 0) {
            win = &windows[i];
            break;
        }
    }
    if (!win) {
        spinlock_unlock(&desktop_lock);
        serial_puts("[desktop] no free window slots\n");
        return NULL;
    }

    memset(win, 0, sizeof(*win));
    win->content_lock.lock = 0;
    win->id = next_win_id++;
    if (y >= 0 && y < DESKTOP_MENUBAR_H + 8)
        y = DESKTOP_MENUBAR_H + 8;
    win->x = x;
    win->y = y;
    win->content_w = w;
    win->content_h = h;
    win->pixel_pitch = (uint32_t)w * 4;
    win->pid = pid;
    win->visible = true;
    win->decorated = true;
    win->resizable = true;
    win->dirty = true;

    if (title) {
        int i;
        for (i = 0; i < DESKTOP_TITLE_MAX - 1 && title[i]; i++)
            win->title[i] = title[i];
        win->title[i] = '\0';
    } else {
        strcpy(win->title, "Window");
    }

    /* Allocate pixel buffer — fail closed on OOM (never return NULL pixels) */
    size_t buf_sz = (size_t)w * (size_t)h * 4;
    win->pixels = (uint32_t *)kalloc(buf_sz);
    if (!win->pixels) {
        win->id = 0;  /* release slot (static windows[] entry) */
        spinlock_unlock(&desktop_lock);
        serial_puts("[desktop] OOM allocating window pixels\n");
        return NULL;
    }
    memset(win->pixels, 0, buf_sz);

    /* Append to Z-order list (top) */
    win->prev = win_top;
    win->next = NULL;
    if (win_top) win_top->next = win;
    win_top = win;
    if (!win_bottom) win_bottom = win;

    /* Focus the new window (single focused id). */
    for (desktop_window_t *w = win_bottom; w; w = w->next)
        w->focused = false;
    win->focused = true;
    dt_focused_id = win->id;

    spinlock_unlock(&desktop_lock);
    dt_apply_keyboard_focus();
    dt_dirty = true;

    serial_printf("[desktop] created window %u '%s' %dx%d at (%d,%d) pid=%u\n",
                  win->id, win->title, w, h, x, y, pid);
    return win;
}

void desktop_destroy_window(desktop_window_t *win) {
    if (!win || win->id == 0) return;

    notepad_on_window_destroyed(win);
    files_on_window_destroyed(win);
    kterm_on_window_destroyed(win);
    settings_app_on_window_destroyed(win);

    spinlock_lock(&desktop_lock);

    /* Unlink from Z-order list */
    if (win->prev) win->prev->next = win->next;
    if (win->next) win->next->prev = win->prev;
    if (win == win_bottom) win_bottom = win->next;
    if (win == win_top)    win_top    = win->prev;

    if (win->pixels) {
        kfree(win->pixels);
        win->pixels = NULL;
    }

    bool was_focused = win->focused;
    win->id = 0;

    if (dt_grab_id == win->id) {
        dt_grab_id = 0;
    }

    /* Transfer focus to the new top window */
    if (was_focused) {
        if (win_top) {
            win_top->focused = true;
            dt_focused_id = win_top->id;
        } else {
            dt_focused_id = 0;
        }
    } else if (dt_focused_id == win->id) {
        dt_focused_id = 0;
    }

    spinlock_unlock(&desktop_lock);
    if (was_focused)
        dt_apply_keyboard_focus();
    dt_dirty = true;
}

void desktop_destroy_windows_for_pid(uint32_t pid) {
    for (int i = 0; i < DESKTOP_MAX_WINDOWS; i++) {
        if (windows[i].id != 0 && windows[i].pid == pid)
            desktop_destroy_window(&windows[i]);
    }
}

void desktop_raise_window(desktop_window_t *win) {
    if (!win || win == win_top) return;

    spinlock_lock(&desktop_lock);

    /* Unlink */
    if (win->prev) win->prev->next = win->next;
    if (win->next) win->next->prev = win->prev;
    if (win == win_bottom) win_bottom = win->next;

    /* Append at top */
    win->prev = win_top;
    win->next = NULL;
    if (win_top) win_top->next = win;
    win_top = win;
    if (!win_bottom) win_bottom = win;

    spinlock_unlock(&desktop_lock);
    dt_dirty = true;
}

void desktop_focus_window(desktop_window_t *win) {
    if (win && win->minimized)
        dt_restore_window(win);
    spinlock_lock(&desktop_lock);
    for (desktop_window_t *w = win_bottom; w; w = w->next)
        w->focused = false;
    if (win) {
        win->focused = true;
        dt_focused_id = win->id;
    } else {
        dt_focused_id = 0;
    }
    spinlock_unlock(&desktop_lock);
    if (win)
        desktop_raise_window(win); /* raise-on-focus */
    dt_apply_keyboard_focus();
    dt_dirty = true;
}

void desktop_unfocus_all(void) {
    spinlock_lock(&desktop_lock);
    for (desktop_window_t *w = win_bottom; w; w = w->next)
        w->focused = false;
    spinlock_unlock(&desktop_lock);
    dt_focused_id = 0;
    SDL_ResetInputState();
    keyboard_set_raw_mode(false);
    dt_last_gui_pid = 0;
    dt_last_gui_rx = -1;
    dt_last_gui_ry = -1;
    dt_dirty = true;
}

bool desktop_pid_has_focus(uint32_t pid) {
    if (pid == 0)
        return false;
    desktop_window_t *fw = desktop_get_focused();
    return fw && fw->pid == pid;
}

bool desktop_focus_eats_keys(void) {
    desktop_window_t *fw = desktop_get_focused();
    if (!fw) return false;
    if (fw->pid != 0) return true;
    return notepad_is_window(fw) || files_is_window(fw) ||
           kterm_is_window(fw) || settings_app_is_window(fw);
}

void desktop_move_window(desktop_window_t *win, int x, int y) {
    if (!win) return;
    win->x = x;
    win->y = y;
    dt_dirty = true;
}

desktop_window_t *desktop_find_window_by_pid(uint32_t pid) {
    for (int i = 0; i < DESKTOP_MAX_WINDOWS; i++) {
        if (windows[i].id != 0 && windows[i].pid == pid)
            return &windows[i];
    }
    return NULL;
}

/* ===================================================================
 * Input routing
 * =================================================================== */

desktop_window_t *desktop_get_focused(void) {
    if (dt_focused_id) {
        desktop_window_t *w = dt_window_by_id(dt_focused_id);
        if (w && w->focused)
            return w;
    }
    for (int i = 0; i < DESKTOP_MAX_WINDOWS; i++) {
        if (windows[i].id != 0 && windows[i].focused) {
            dt_focused_id = windows[i].id;
            return &windows[i];
        }
    }
    dt_focused_id = 0;
    return NULL;
}

/* Returns topmost visible window whose frame contains (x, y). */
desktop_window_t *desktop_window_at(int x, int y) {
    desktop_window_t *hit = NULL;
    spinlock_lock(&desktop_lock);
    for (desktop_window_t *w = win_top; w; w = w->prev) {
        if (!w->visible || w->minimized) continue;
        if (w->fullscreen) {
            if (y >= DESKTOP_MENUBAR_H) {
                hit = w;
                break;
            }
            continue;
        }
        int fx, fy, fw, fh;
        dt_win_frame(w, &fx, &fy, &fw, &fh);
        int pad = (w->resizable && !w->maximized) ? DT_RESIZE_BORDER : 0;
        if (x >= fx - pad && x < fx + fw + pad &&
            y >= fy - pad && y < fy + fh + pad) {
            hit = w;
            break;
        }
    }
    spinlock_unlock(&desktop_lock);
    return hit;
}

/* Called on left-button-down */
void desktop_handle_mouse(int mx, int my, uint8_t buttons) {
    if (!(buttons & 1)) return;

    int scr_w = (int)fb_get_width();
    int scr_h = (int)fb_get_height();

    if (dt_start_open) {
        int hit = dt_start_hit(mx, my);
        if (hit >= 0) {
            dt_start_activate(hit);
            dt_dirty = true;
            return;
        }
        if (hit == -2 && !(mx >= 8 && mx < 8 + DT_LOGO_W && my >= 5 && my < 27)) {
            dt_start_open = false;
            dt_dirty = true;
            if (my >= 0 && my < DESKTOP_MENUBAR_H)
                return;
        }
    }

    if (my >= 0 && my < DESKTOP_MENUBAR_H) {
        dt_menubar_click(mx, my, scr_w, scr_h);
        return;
    }

    int dx, dy, dw, dh;
    dt_dock_geom(scr_w, scr_h, &dx, &dy, &dw, &dh);
    if (dt_point_in_rect(mx, my, dx, dy, dw, dh)) {
        desktop_unfocus_all();
        dt_dock_click(mx, my, scr_w, scr_h);
        return;
    }

    desktop_window_t *hit = desktop_window_at(mx, my);
    if (!hit) {
        desktop_unfocus_all();
        dt_handle_desktop_icon_click(mx, my, scr_w, scr_h);
        return;
    }

    desktop_focus_window(hit);

    /* Fullscreen has no chrome; the click belongs to the client. */
    if (hit->fullscreen)
        return;

    if (!hit->decorated) return;

    int traffic = dt_hit_traffic(hit, mx, my);
    if (traffic == 0) {
        dt_close_window(hit);
        return;
    }
    if (traffic == 1) {
        dt_minimize_window(hit);
        return;
    }
    if (traffic == 2) {
        dt_toggle_fullscreen(hit);
        desktop_focus_window(hit);
        return;
    }
    int edges = dt_resize_hit(hit, mx, my);
    if (edges) {
        hit->resize_edges = edges;
        hit->resize_orig_x = hit->x;
        hit->resize_orig_y = hit->y;
        hit->resize_orig_w = hit->content_w;
        hit->resize_orig_h = hit->content_h;
        hit->resize_anchor_x = mx;
        hit->resize_anchor_y = my;
        dt_grab_id = hit->id;
        return;
    }

    int tbx = hit->x + DESKTOP_BORDER_W;
    int tby = hit->y + DESKTOP_BORDER_W;
    int tbw = hit->content_w;
    if (mx >= tbx && mx < tbx + tbw &&
        my >= tby && my < tby + DESKTOP_TITLEBAR_H) {
        uint64_t now = hpet_get_ticks();
        if (hit->id == dt_title_last_id &&
            (now - dt_title_last_click) < DT_TITLE_DBLCLK_MS) {
            dt_title_last_id = 0;
            dt_toggle_fullscreen(hit);
            desktop_focus_window(hit);
            return;
        }
        dt_title_last_id = hit->id;
        dt_title_last_click = now;
        if (!hit->maximized) {
            hit->dragging = true;
            hit->drag_off_x = mx - hit->x;
            hit->drag_off_y = my - hit->y;
            dt_grab_id = hit->id;
        }
        return;
    }

    int cx, cy, cw, ch;
    dt_get_client_rect(hit, &cx, &cy, &cw, &ch);
    if (mx >= cx && mx < cx + cw && my >= cy && my < cy + ch) {
        int rx = mx - cx, ry = my - cy;
        if (notepad_is_window(hit))
            notepad_on_mouse(rx, ry, true);
        else if (files_is_window(hit))
            files_on_mouse(rx, ry, true);
        else if (kterm_is_window(hit))
            kterm_on_mouse(rx, ry, true);
        else if (settings_app_is_window(hit))
            settings_app_on_mouse(rx, ry, true);
    }
}

bool desktop_handle_key(uint8_t scancode, bool pressed) {
    if (!desktop_running) return false;

    if (scancode == 0x5B && pressed) {
        dt_start_open = !dt_start_open;
        dt_start_hover = -1;
        dt_switcher_open = false;
        dt_dirty = true;
        return true;
    }

    if (scancode == 0x38 && !pressed && dt_switcher_open) {
        desktop_window_t *list[DESKTOP_MAX_WINDOWS];
        int n = dt_collect_windows(list, DESKTOP_MAX_WINDOWS);
        dt_switcher_open = false;
        if (n > 0) {
            if (dt_switcher_sel < 0) dt_switcher_sel = 0;
            if (dt_switcher_sel >= n) dt_switcher_sel = n - 1;
            desktop_focus_window(list[dt_switcher_sel]);
        }
        dt_dirty = true;
        return true;
    }

    bool alt = keyboard_get_alt() || (scancode == 0x38 && pressed);
    if (alt && scancode == 0x0F && pressed) {
        desktop_window_t *list[DESKTOP_MAX_WINDOWS];
        int n = dt_collect_windows(list, DESKTOP_MAX_WINDOWS);
        if (n > 0) {
            if (!dt_switcher_open) {
                dt_switcher_open = true;
                dt_switcher_sel = 1 % n;
            } else {
                dt_switcher_sel = (dt_switcher_sel + 1) % n;
            }
            dt_start_open = false;
            dt_dirty = true;
        }
        return true;
    }

    if (alt && scancode == 0x3E && pressed) {
        dt_close_window(desktop_get_focused());
        return true;
    }

    /* Alt+Enter or F11 toggles fullscreen. Restore does not need chrome. */
    if (pressed && ((alt && scancode == 0x1C) || scancode == 0x57)) {
        desktop_window_t *fs = desktop_get_focused();
        if (fs) {
            dt_toggle_fullscreen(fs);
            return true;
        }
    }

    if (scancode == 0x01 && pressed && (dt_start_open || dt_switcher_open)) {
        dt_start_open = false;
        dt_switcher_open = false;
        dt_dirty = true;
        return true;
    }

    return false;
}

/* ===================================================================
 * Desktop syscall backends
 * =================================================================== */

int desktop_sys_embed_info(uint32_t pid, void *user_info) {
    if (!desktop_running) return -1;

    desktop_window_t *win = desktop_find_window_by_pid(pid);
    if (!win) return -1;

    /* Build the embed info struct that DOOM / clients expect */
    typedef struct {
        int32_t active;
        int32_t content_x;
        int32_t content_y;
        int32_t content_w;
        int32_t content_h;
    } embed_info_t;

    int cx = win->x + (win->decorated ? DESKTOP_BORDER_W : 0);
    int cy = win->y + (win->decorated ? DESKTOP_BORDER_W + DESKTOP_TITLEBAR_H : 0);

    embed_info_t info;
    info.active    = 1;
    info.content_x = cx;
    info.content_y = cy;
    info.content_w = win->content_w;
    info.content_h = win->content_h;

    extern int copy_to_user(void *dst, const void *src, size_t n);
    if (copy_to_user(user_info, &info, sizeof(info)) != 0)
        return -14; /* EFAULT */
    return 0;
}

int desktop_sys_composite(uint32_t pid, const void *user_pixels,
                          uint32_t w, uint32_t h) {
    if (!desktop_running) {
        return -1;
    }

    /* Blocking exec_file never returns to kmain, so chrome clicks are
     * sampled on the present syscall. This may destroy the window. */
    desktop_poll_input();

    desktop_window_t *win = desktop_find_window_by_pid(pid);
    if (!win || !win->pixels || !user_pixels || w == 0 || h == 0 ||
        !win->visible || win->minimized) {
        /* Repaint so a minimized or closed client stops covering the desktop
         * even when this thread is the only one running the compositor. */
        if (desktop_running)
            desktop_present_now();
        return win ? 0 : -1;
    }

    uint32_t copy_w = (w < (uint32_t)win->content_w) ? w : (uint32_t)win->content_w;
    uint32_t copy_h = (h < (uint32_t)win->content_h) ? h : (uint32_t)win->content_h;
    if (copy_w == 0 || copy_h == 0)
        return -1;

    /*
     * Native-resolution present: if the client sent a smaller buffer than
     * the window (DOOM 320x200 into a 640x400 slot), copy once then
     * nearest-neighbor scale in-kernel.  That avoids a 2 MB userspace
     * upscale + copy_from_user every frame (~2 FPS on TCG).
     */
    int scale_x = win->content_w / (int)w;
    int scale_y = win->content_h / (int)h;
    int iscale = (scale_x < scale_y) ? scale_x : scale_y;
    bool scaled = (iscale >= 2) && (w * h * 4u <= 512u * 1024u);

    spinlock_lock(&win->content_lock);
    if (scaled) {
        static uint32_t *scratch;
        static size_t scratch_bytes;
        size_t src_bytes = (size_t)w * (size_t)h * 4u;
        if (!scratch || scratch_bytes < src_bytes) {
            if (scratch) kfree(scratch);
            scratch = (uint32_t *)kalloc(src_bytes);
            scratch_bytes = scratch ? src_bytes : 0;
        }
        if (!scratch || copy_from_user(scratch, user_pixels, src_bytes) != 0) {
            spinlock_unlock(&win->content_lock);
            return -14;
        }
        int vw = (int)w * iscale;
        int vh = (int)h * iscale;
        int ox = (win->content_w - vw) / 2;
        int oy = (win->content_h - vh) / 2;
        if (ox < 0) ox = 0;
        if (oy < 0) oy = 0;
        uint32_t dst_pitch_px = win->pixel_pitch / 4u;
        for (uint32_t y = 0; y < h; y++) {
            const uint32_t *srow = scratch + (size_t)y * w;
            for (int sy = 0; sy < iscale; sy++) {
                int dy = oy + (int)y * iscale + sy;
                if (dy < 0 || dy >= win->content_h) continue;
                uint32_t *drow = win->pixels + (size_t)dy * dst_pitch_px + (size_t)ox;
                if (iscale == 2) {
                    for (uint32_t x = 0; x < w; x++) {
                        uint32_t c = srow[x];
                        drow[x * 2u] = c;
                        drow[x * 2u + 1u] = c;
                    }
                } else {
                    for (uint32_t x = 0; x < w; x++) {
                        uint32_t c = srow[x];
                        for (int sx = 0; sx < iscale; sx++)
                            drow[x * (uint32_t)iscale + (uint32_t)sx] = c;
                    }
                }
            }
        }
    } else if (win->pixel_pitch == w * 4 && copy_w == w) {
        size_t nbytes = (size_t)copy_h * (size_t)w * 4;
        if (copy_from_user(win->pixels, user_pixels, nbytes) != 0) {
            spinlock_unlock(&win->content_lock);
            return -14; /* EFAULT */
        }
    } else {
        size_t row_bytes = (size_t)copy_w * 4;
        for (uint32_t y = 0; y < copy_h; y++) {
            void *dst = (uint8_t *)win->pixels + (size_t)y * win->pixel_pitch;
            const void *src = (const uint8_t *)user_pixels + (size_t)y * (size_t)w * 4;
            if (copy_from_user(dst, src, row_bytes) != 0) {
                spinlock_unlock(&win->content_lock);
                return -14; /* EFAULT */
            }
        }
    }
    spinlock_unlock(&win->content_lock);

    /* Doom presents a 320x200 buffer. Cap that path at 35 Hz on the PM timer
     * (HLT pacing does not hold when HPET is absent). */
    if ((w == 320 && h == 200) ||
        (win->title[0] == 'D' && win->title[1] == 'O' && win->title[2] == 'O'))
        ktime_pace_hz(35);

    if (dt_present_window_fast(win))
        return 0;
    win->dirty = true;
    desktop_present_now();
    return 0;
}
