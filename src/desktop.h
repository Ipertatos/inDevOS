/**
 * @file desktop.h
 * @brief Desktop environment compositor and window manager for inDevOS
 *
 * Architecture (Phase 1 WM):
 *   - WM owns focus, Z-order, decorations, hit-testing, pointer grab
 *     (title-bar move / edge resize), and routes input to the focused
 *     (or capturing) window's client area only.
 *   - Compositor stacks wallpaper -> windows (bottom..top) -> panel/cursor.
 *   - Clients (SDL / SYS_DESKTOP_COMPOSITE / LuaDE apps) draw into the
 *     window pixel buffer (client rect) only; chrome is WM-drawn.
 *
 * Syscall backends: SYS_DESKTOP_EMBED_INFO, SYS_DESKTOP_COMPOSITE,
 * SYS_DESKTOP_POLL_EVENT.
 *
 * Phase 2 (not here): multi-monitor, damage-aware client clip, server-side
 * decorations themes, pointer warping, full compositing effects.
 */
#ifndef DESKTOP_H
#define DESKTOP_H

#include "types.h"
#include "atomic.h"
#include "../include/indevos_gui.h"

#define DESKTOP_MAX_WINDOWS   32
#define DESKTOP_TITLE_MAX     64
#define DESKTOP_TITLEBAR_H    32
#define DESKTOP_BORDER_W       1
#define DESKTOP_PANEL_H       72
#define DESKTOP_CLOSE_BTN_SZ  12
/* Reserved top area (menubar / avoids icons under a future bar). */
#define DESKTOP_MENUBAR_H     32
#define DESKTOP_TOP_DEADSPACE DESKTOP_MENUBAR_H
#define DESKTOP_ICON_COL_W    88
#define DESKTOP_ICON_ROW_H    80
#define DESKTOP_ICON_GRID     16
#define DESKTOP_DOCK_BTN_W    44
#define DESKTOP_DOCK_BTN_GAP  10
#define DESKTOP_WALLPAPER_PATHS 3
#define DESKTOP_WIN_RAD        8
#define DESKTOP_SHADOW_PAD    10

/* ---- Colour palette (midnight / teal) ---- */
#define DT_COL_WALLPAPER       0xFF0B1020
#define DT_COL_WALLPAPER_BOT   0xFF0A2430
#define DT_COL_TITLEBAR_FOCUS  0xFF1A2035
#define DT_COL_TITLEBAR_UNFOC  0xFF141824
#define DT_COL_BORDER          0xFF2A3148
#define DT_COL_BORDER_FOCUS    0xFF3DDC97
#define DT_COL_PANEL_BG        0xE612161E
#define DT_COL_PANEL_ACTIVE    0xFF1E2740
#define DT_COL_MENUBAR_BG      0xFF0E121A
#define DT_COL_DOCK_BTN        0xFF1C2436
#define DT_COL_CLOSE_BTN       0xFFE85D5D
#define DT_COL_TRAFFIC_MIN     0xFFFEBC2E
#define DT_COL_TRAFFIC_MAX     0xFF28C840
#define DT_COL_TEXT_WHITE      0xFFF4F4F5
#define DT_COL_TEXT_GRAY       0xFF94A3B8
#define DT_COL_CURSOR_FILL     0xFFF8FAFC
#define DT_COL_CURSOR_BORDER   0xFF0B1020
#define DT_COL_ACCENT          0xFF3DDC97
#define DT_COL_SURFACE         0xFF12161E

/* ---- Window ---- */
typedef struct desktop_window {
    uint32_t  id;
    int32_t   x, y;              /* top-left of decorated frame             */
    int32_t   content_w, content_h;
    uint32_t *pixels;            /* kernel-side XRGB8888 buffer             */
    uint32_t  pixel_pitch;       /* bytes per row                           */
    uint32_t  pid;               /* owning process (0 = kernel/panel)       */
    char      title[DESKTOP_TITLE_MAX];
    bool      visible;
    bool      focused;
    bool      dirty;
    bool      decorated;
    bool      resizable;
    bool      minimized;
    bool      maximized;
    bool      fullscreen;        /* client fills the screen; no window chrome */
    bool      dragging;
    int32_t   drag_off_x, drag_off_y;
    int32_t   restore_x, restore_y, restore_w, restore_h;
    int       resize_edges;
    int32_t   resize_orig_x, resize_orig_y, resize_orig_w, resize_orig_h;
    int32_t   resize_anchor_x, resize_anchor_y;
    spinlock_t content_lock;     /* SMP: user composite vs blit             */
    struct desktop_window *prev; /* lower Z                                */
    struct desktop_window *next; /* higher Z                               */
} desktop_window_t;

/* ---- Public API (kernel-side) ---- */
void desktop_init(void);
void desktop_start(void);         /* start compositor                      */
void desktop_stop(void);
bool desktop_is_running(void);
void desktop_composite_frame(void);
/** Mouse/Super handling without a full composite — used while a Ring-3
 *  app owns the framebuffer so clicks still focus/close windows. */
void desktop_poll_input(void);
/** True when the WM needs a full composite (stacking/chrome/drag/menus),
 *  not just a client fast-present. Kernel uses this instead of treating
 *  any focused Ring-3 window as "owns the framebuffer". */
bool desktop_wm_needs_composite(void);
/** Count of visible, non-minimized windows (for stacking policy). */
int  desktop_count_visible_windows(void);
void desktop_present_now(void);
/** Force a full-screen recomposite now (damage tracking reset).  Use after an
 *  app that drew straight to the framebuffer exits, so its residue is replaced
 *  by the desktop instead of the console being redrawn over it. */
void desktop_force_full_repaint(void);
bool desktop_present_window_now(uint32_t pid);
bool desktop_pid_has_focus(uint32_t pid);
bool desktop_focus_eats_keys(void);
void desktop_unfocus_all(void);

/* Window management */
desktop_window_t *desktop_create_window(uint32_t pid, int x, int y,
                                        int w, int h, const char *title);
void  desktop_destroy_window(desktop_window_t *win);
void  desktop_destroy_windows_for_pid(uint32_t pid);
void  desktop_raise_window(desktop_window_t *win);
void  desktop_focus_window(desktop_window_t *win);
void  desktop_move_window(desktop_window_t *win, int x, int y);

/* Desktop syscall backends */
int  desktop_sys_embed_info(uint32_t pid, void *user_info);
int  desktop_sys_composite(uint32_t pid, const void *pixels,
                           uint32_t w, uint32_t h);

/* Input routing */
desktop_window_t *desktop_window_at(int x, int y);
desktop_window_t *desktop_get_focused(void);
void desktop_handle_mouse(int mx, int my, uint8_t buttons);
/** Returns true if the compositor consumed the key (do not forward to apps). */
bool desktop_handle_key(uint8_t scancode, bool pressed);
void desktop_theme_reload(void);
void desktop_request_redraw(void);

/* SDL key path: enqueue GUI_EVENT_KEY / KEYUP for focused window */
void desktop_gui_enqueue_key_sdl(int32_t keycode, bool pressed);

/* Find window by PID (first match) */
desktop_window_t *desktop_find_window_by_pid(uint32_t pid);

#endif /* DESKTOP_H */
