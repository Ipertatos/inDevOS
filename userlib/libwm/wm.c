/* libwm - see wm.h.  Plain Linux syscalls through musl; no other deps. */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "wm.h"

int wm_connect(wm_conn_t *c) {
    memset(c, 0, sizeof(*c));
    c->fd = open(WM_DEVICE_PATH, O_RDWR | O_CLOEXEC);
    if (c->fd < 0) return -errno;
    if (ioctl(c->fd, WM_GET_INFO, &c->info) < 0) {
        int e = errno; close(c->fd); c->fd = -1; return -e;
    }
    if (c->info.version != WM_ABI_VERSION || c->info.event_size != sizeof(struct wm_event)) {
        close(c->fd); c->fd = -1; return -EPROTO;
    }
    return 0;
}

void wm_disconnect(wm_conn_t *c) {
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
}

int wm_window_create(wm_conn_t *c, wm_window_t *win, int w, int h, const char *title,
                     uint32_t type, uint32_t flags) {
    struct wm_create cr;
    memset(&cr, 0, sizeof(cr));
    memset(win, 0, sizeof(*win));
    cr.w = (uint32_t)w; cr.h = (uint32_t)h; cr.type = type; cr.flags = flags;
    cr.x = WM_POS_DEFAULT; cr.y = WM_POS_DEFAULT;
    if (title) strncpy(cr.title, title, WM_TITLE_MAX - 1);
    if (ioctl(c->fd, WM_CREATE_WINDOW, &cr) < 0) return -errno;
    void *m = mmap(NULL, (size_t)cr.mmap_len, PROT_READ | PROT_WRITE, MAP_SHARED, c->fd,
                   (off_t)cr.mmap_off);
    if (m == MAP_FAILED) {
        int e = errno;
        ioctl(c->fd, WM_DESTROY, (unsigned long)cr.id);
        return -e;
    }
    win->conn = c; win->id = cr.id; win->w = w; win->h = h;
    win->stride_px = (int)(cr.stride / 4);
    win->buf[0] = (uint32_t *)m;
    win->buf[1] = (uint32_t *)((uint8_t *)m + cr.buf_size);
    win->back = 0;
    win->map = m; win->map_len = (size_t)cr.mmap_len;
    return 0;
}

int wm_window_present(wm_window_t *win, int x, int y, int w, int h, int copy_back) {
    struct wm_present p = { win->id, (uint32_t)win->back, x, y, w, h };
    if (ioctl(win->conn->fd, WM_PRESENT, &p) < 0) return -errno;
    int front = win->back;
    win->back ^= 1;
    if (copy_back) {
        int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
        if (w <= 0 || h <= 0) { x0 = 0; y0 = 0; x1 = win->w; y1 = win->h; }
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > win->w) x1 = win->w;
        if (y1 > win->h) y1 = win->h;
        for (int yy = y0; yy < y1 && x1 > x0; yy++)
            memcpy(win->buf[win->back] + (size_t)yy * win->stride_px + x0,
                   win->buf[front] + (size_t)yy * win->stride_px + x0, (size_t)(x1 - x0) * 4);
    }
    return 0;
}

int wm_window_resize(wm_window_t *win, int w, int h) {
    if (win->conn->info.minor < 1) return -ENOTTY;
    struct wm_resize_ack ra;
    memset(&ra, 0, sizeof(ra));
    ra.id = win->id; ra.w = (uint32_t)w; ra.h = (uint32_t)h;
    if (ioctl(win->conn->fd, WM_RESIZE_ACK, &ra) < 0) return -errno;
    void *m = mmap(NULL, (size_t)ra.mmap_len, PROT_READ | PROT_WRITE, MAP_SHARED,
                   win->conn->fd, (off_t)ra.mmap_off);
    if (m == MAP_FAILED) return -errno;     /* old mapping stays usable until next ack */
    if (win->map) munmap(win->map, win->map_len);
    win->w = w; win->h = h;
    win->stride_px = (int)(ra.stride / 4);
    win->buf[0] = (uint32_t *)m;
    win->buf[1] = (uint32_t *)((uint8_t *)m + ra.buf_size);
    win->back = 0;
    win->map = m; win->map_len = (size_t)ra.mmap_len;
    return 0;
}

int wm_window_set_title(wm_window_t *win, const char *title) {
    struct wm_title t;
    memset(&t, 0, sizeof(t));
    t.id = win->id;
    strncpy(t.title, title, WM_TITLE_MAX - 1);
    return ioctl(win->conn->fd, WM_SET_TITLE, &t) < 0 ? -errno : 0;
}

int wm_window_destroy(wm_window_t *win) {
    if (win->map) munmap(win->map, win->map_len);
    win->map = NULL;
    int r = ioctl(win->conn->fd, WM_DESTROY, (unsigned long)win->id) < 0 ? -errno : 0;
    win->id = 0;
    return r;
}

int wm_next_event(wm_conn_t *c, struct wm_event *ev, int timeout_ms) {
    struct pollfd pf = { c->fd, POLLIN, 0 };
    int r = poll(&pf, 1, timeout_ms);
    if (r < 0) return errno == EINTR ? 0 : -errno;
    if (r == 0) return 0;
    ssize_t n = read(c->fd, ev, sizeof(*ev));
    if (n == (ssize_t)sizeof(*ev)) return 1;
    if (n < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
    return n < 0 ? -errno : -EIO;
}

uint32_t wm_premul(uint32_t c) {
    uint32_t a = c >> 24;
    if (a == 255) return c;
    uint32_t r = ((c >> 16) & 255) * a / 255, g = ((c >> 8) & 255) * a / 255, b = (c & 255) * a / 255;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

void wm_fill_rect(wm_window_t *win, int x, int y, int w, int h, uint32_t color) {
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > win->w ? win->w : x + w, y1 = y + h > win->h ? win->h : y + h;
    uint32_t *px = wm_window_pixels(win);
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++)
            px[(size_t)yy * win->stride_px + xx] = color;
}
