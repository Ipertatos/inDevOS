/*
 * wm_abi.h - inDevOS window-server device (/dev/wm) user ABI, version 1.
 *
 * Shared verbatim by the kernel (src/wm_abi.h) and user space
 * (userlib/libwm/wm_abi.h).  Freestanding: only <stdint.h>.
 *
 * Model
 *   fd = open("/dev/wm", O_RDWR)          one connection (usually per process)
 *   ioctl(fd, WM_CREATE_WINDOW, &c)       -> c.id, c.stride, c.mmap_off, c.mmap_len
 *   mmap(NULL, c.mmap_len, PROT_READ|PROT_WRITE, MAP_SHARED, fd, c.mmap_off)
 *       two surfaces of c.buf_size bytes each: buffer 0 at +0, buffer 1 at
 *       +c.buf_size.  Pixels are 32-bit premultiplied ARGB (0xAARRGGBB in a
 *       little-endian uint32_t), row pitch c.stride bytes, c.w x c.h.
 *   ioctl(fd, WM_PRESENT, &p)             copy p.buffer's damage rect into the
 *       compositor (synchronous copy: both buffers are free again on return).
 *   read(fd, wm_event[], n*sizeof)        whole events only; O_NONBLOCK -> EAGAIN
 *   poll(POLLIN)                          an event is queued
 *   close(fd) / exit / crash              destroys every window of the fd
 *
 * Resize (minor 1): when the user resizes a WM_FLAG_RESIZABLE window the
 * client gets WM_EV_RESIZE {w,h}.  It answers with WM_RESIZE_ACK {id,w,h},
 * which allocates new double buffers and returns a new stride / buf_size /
 * mmap_off / mmap_len; the client maps the new range and munmaps the old one.
 * The previous surface stays valid until the next ack or close.  Until the
 * first present at the new size the desktop shows the old pixels, clipped,
 * padded with the window background colour.
 *
 * The WM draws the chrome (title bar, border, shadow); the surface is the
 * client area only.  Closing via the title-bar button does NOT destroy the
 * window: the client receives WM_EV_CLOSE_REQUEST and decides.
 */
#ifndef INDEVOS_WM_ABI_H
#define INDEVOS_WM_ABI_H
#include <stdint.h>

#define WM_ABI_VERSION   1   /* major: incompatible changes only */
#define WM_ABI_MINOR     1   /* 1: WM_RESIZE_ACK                 */
#define WM_DEVICE_PATH   "/dev/wm"
#define WM_TITLE_MAX     64
#define WM_MAX_W         2048
#define WM_MAX_H         2048
#define WM_MAX_WINDOWS_PER_FD 8

/* Linux _IOC encoding (dir:2 | size:14 | type:8 | nr:8). */
#define WM_IOC_NONE  0u
#define WM_IOC_WRITE 1u
#define WM_IOC_READ  2u
#define WM_IOC(dir, nr, size) \
    ((unsigned long)(((dir) << 30) | ((uint32_t)(size) << 16) | ((uint32_t)'W' << 8) | (nr)))

/* window types (match DESKTOP_WIN_*) */
#define WM_TYPE_NORMAL 0
#define WM_TYPE_DIALOG 1
#define WM_TYPE_POPUP  2

/* create flags */
#define WM_FLAG_RESIZABLE 0x1   /* NORMAL only: user may resize the frame */
#define WM_FLAG_NO_FOCUS  0x2   /* reserved (not implemented yet)          */

#define WM_POS_DEFAULT  ((int32_t)0x80000000)  /* let the WM place it */

struct wm_create {
    /* in */
    uint32_t w, h;              /* client size, 1..WM_MAX_W/H              */
    uint32_t type;              /* WM_TYPE_*                               */
    uint32_t flags;             /* WM_FLAG_*                               */
    uint32_t parent;            /* owner window id (dialog/popup), 0 = none */
    int32_t  x, y;              /* frame position or WM_POS_DEFAULT        */
    char     title[WM_TITLE_MAX];
    /* out */
    uint32_t id;                /* window id (stable, never reused)        */
    uint32_t stride;            /* bytes per row (w*4)                     */
    uint32_t buf_size;          /* bytes per buffer (page aligned)         */
    uint32_t nbuf;              /* 2                                       */
    uint64_t mmap_off;          /* offset to pass to mmap                  */
    uint64_t mmap_len;          /* nbuf * buf_size                         */
};

struct wm_present {
    uint32_t id;
    uint32_t buffer;            /* 0 or 1                                  */
    int32_t  x, y, w, h;        /* damage in client coords; w<=0||h<=0 = all */
};

struct wm_title {
    uint32_t id;
    char     title[WM_TITLE_MAX];
};

struct wm_info {
    uint32_t version;           /* WM_ABI_VERSION                          */
    uint32_t screen_w, screen_h;
    uint32_t max_windows;       /* per fd                                  */
    uint32_t event_size;        /* sizeof(struct wm_event)                 */
    uint32_t minor;             /* WM_ABI_MINOR (0 on minor-0 kernels)     */
    uint32_t reserved[2];
};

struct wm_resize_ack {
    /* in */
    uint32_t id;
    uint32_t w, h;              /* new client size (normally the RESIZE event's) */
    /* out */
    uint32_t stride, buf_size, nbuf;
    uint64_t mmap_off, mmap_len;
};

#define WM_CREATE_WINDOW WM_IOC(WM_IOC_READ | WM_IOC_WRITE, 1, sizeof(struct wm_create))
#define WM_PRESENT       WM_IOC(WM_IOC_WRITE, 2, sizeof(struct wm_present))
#define WM_SET_TITLE     WM_IOC(WM_IOC_WRITE, 3, sizeof(struct wm_title))
#define WM_DESTROY       WM_IOC(WM_IOC_NONE,  4, 0)   /* arg = window id (by value) */
#define WM_GET_INFO      WM_IOC(WM_IOC_READ,  5, sizeof(struct wm_info))
#define WM_RESIZE_ACK    WM_IOC(WM_IOC_READ | WM_IOC_WRITE, 6, sizeof(struct wm_resize_ack))  /* minor 1 */

/* event types; pointer types equal DESKTOP_PTR_* */
#define WM_EV_MOTION        1
#define WM_EV_BUTTON_DOWN   2
#define WM_EV_BUTTON_UP     3
#define WM_EV_WHEEL         4
#define WM_EV_KEY_DOWN      5
#define WM_EV_KEY_UP        6
#define WM_EV_FOCUS_IN      7
#define WM_EV_FOCUS_OUT     8
#define WM_EV_CLOSE_REQUEST 9
#define WM_EV_RESIZE       10   /* w,h = new client size; answer with WM_RESIZE_ACK */

/* modifier bits (match DESKTOP_MOD_*) */
#define WM_MOD_SHIFT 0x1
#define WM_MOD_CTRL  0x2
#define WM_MOD_ALT   0x4
#define WM_MOD_SUPER 0x8

/* Fixed 48-byte event.  Pointer fields mirror desktop_ptr_event_t. */
struct wm_event {
    uint32_t type;              /* WM_EV_*                                 */
    uint32_t win;               /* window id                               */
    int32_t  x, y;              /* pointer: client coords                  */
    int32_t  button;            /* 1 left, 2 right, 3 middle               */
    int32_t  clicks;            /* 1 single, 2 double, ...                 */
    int32_t  wheel;             /* steps, positive = up                    */
    uint32_t mods;              /* WM_MOD_*                                */
    int32_t  key;               /* key events: desktop/SDL keycode         */
    uint32_t w, h;              /* RESIZE                                  */
    uint32_t time_ms;           /* kernel monotonic ms                     */
};

#ifdef __cplusplus
static_assert(sizeof(struct wm_event) == 48, "wm_event");
#else
_Static_assert(sizeof(struct wm_event) == 48, "wm_event");
_Static_assert(sizeof(struct wm_create) == 128, "wm_create");
_Static_assert(sizeof(struct wm_resize_ack) == 40, "wm_resize_ack");
_Static_assert(sizeof(struct wm_info) == 32, "wm_info");
#endif

#endif /* INDEVOS_WM_ABI_H */
