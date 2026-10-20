/*
 * libwm - tiny static client library for the inDevOS window server (/dev/wm).
 * Build: musl-gcc -static -O2 -c wm.c ; link wm.o (or libwm.a) into the app.
 */
#ifndef LIBWM_H
#define LIBWM_H
#include <stdint.h>
#include <stddef.h>
#include "wm_abi.h"

typedef struct wm_conn { int fd; struct wm_info info; } wm_conn_t;

typedef struct wm_window {
    wm_conn_t *conn;
    uint32_t   id;
    int        w, h;
    int        stride_px;           /* pixels per row                       */
    uint32_t  *buf[2];              /* two surfaces (premultiplied ARGB)     */
    int        back;                /* buffer to draw into next             */
    void      *map;
    size_t     map_len;
} wm_window_t;

int  wm_connect(wm_conn_t *c);                  /* 0 or -errno */
void wm_disconnect(wm_conn_t *c);               /* destroys all windows */

int  wm_window_create(wm_conn_t *c, wm_window_t *win, int w, int h, const char *title,
                      uint32_t type, uint32_t flags);
/* Back buffer to draw into. */
static inline uint32_t *wm_window_pixels(wm_window_t *win) { return win->buf[win->back]; }
/* Present the back buffer (damage rect; w<=0 = whole) and flip.
 * With copy_back the new back buffer receives the presented contents, so
 * partial redraws stay consistent. */
int  wm_window_present(wm_window_t *win, int x, int y, int w, int h, int copy_back);
int  wm_window_set_title(wm_window_t *win, const char *title);
int  wm_window_destroy(wm_window_t *win);
/* Answer a WM_EV_RESIZE: new surfaces at w x h (WM_RESIZE_ACK), remap,
 * unmap the old surfaces.  Contents are undefined: redraw and present.
 * Returns 0 or -errno (-ENOTTY on a minor-0 kernel: keep the old size). */
int  wm_window_resize(wm_window_t *win, int w, int h);

/* Wait up to timeout_ms (-1 = forever) for one event.  1 = got one, 0 = timeout, <0 = -errno. */
int  wm_next_event(wm_conn_t *c, struct wm_event *ev, int timeout_ms);

/* Drawing helpers (premultiplied ARGB). */
static inline uint32_t wm_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}
uint32_t wm_premul(uint32_t argb);              /* straight -> premultiplied */
void wm_fill_rect(wm_window_t *win, int x, int y, int w, int h, uint32_t color);
#endif
