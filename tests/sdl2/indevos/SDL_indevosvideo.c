/*
  inDevOS video driver for SDL2.

  Each SDL_Window is one window of the inDevOS window server (/dev/wm, see
  wm_abi.h).  Rendering goes through SDL's window framebuffer: the surface
  SDL draws into is buffer 0 of the window's shared mapping (XRGB8888), and
  UpdateWindowFramebuffer forces alpha to 0xFF on the damaged area (the WM
  composites premultiplied ARGB) and WM_PRESENTs it.  WM_PRESENT is a
  synchronous copy, so one buffer is enough.  The software renderer
  (SDL_CreateRenderer) works on top of that.  The WM draws the window frame
  and the mouse pointer.

  Copyright (C) 2026 Georgios Markakis.  zlib license (same as SDL).
*/
#include "../../SDL_internal.h"

#ifdef SDL_VIDEO_DRIVER_INDEVOS

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "SDL_video.h"
#include "SDL_mouse.h"
#include "SDL_hints.h"
#include "../SDL_sysvideo.h"
#include "../SDL_pixels_c.h"
#include "../../events/SDL_events_c.h"
#include "../../events/SDL_mouse_c.h"
#include "../../events/SDL_keyboard_c.h"

#include "SDL_indevosvideo.h"
#include "wm_abi.h"

#define INDEVOS_DRIVER_NAME "indevos"
#define INDEVOS_FORMAT      SDL_PIXELFORMAT_RGB888   /* XRGB8888 */

/* ---------------------------------------------------------------- windows */

static void UnmapSurface(SDL_WindowData *data)
{
    if (data->map) {
        munmap(data->map, data->map_len);
        data->map = NULL;
        data->map_len = 0;
    }
}

static int MapSurface(SDL_WindowData *data, int fd, Uint64 off, Uint64 len, Uint32 stride, int w, int h)
{
    void *m = mmap(NULL, (size_t)len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)off);
    if (m == MAP_FAILED) {
        return SDL_SetError("indevos: mmap window surface: %s", strerror(errno));
    }
    UnmapSurface(data);
    data->map = m;
    data->map_len = (size_t)len;
    data->stride = (int)stride;
    data->w = w;
    data->h = h;
    return 0;
}

static int ClampDim(int v, int max)
{
    return v < 1 ? 1 : (v > max ? max : v);
}

/* Create the WM window for window->w x window->h (window->driverdata set). */
static int CreateWMWindow(_THIS, SDL_Window *window)
{
    SDL_VideoData *vd = (SDL_VideoData *)_this->driverdata;
    SDL_WindowData *data = (SDL_WindowData *)window->driverdata;
    struct wm_create c;
    int w = ClampDim(window->w, WM_MAX_W), h = ClampDim(window->h, WM_MAX_H);

    SDL_zero(c);
    c.w = (uint32_t)w;
    c.h = (uint32_t)h;
    c.type = WM_TYPE_NORMAL;
    if (window->flags & SDL_WINDOW_RESIZABLE) {
        c.flags |= WM_FLAG_RESIZABLE;
    }
    if (SDL_WINDOWPOS_ISUNDEFINED(window->x) || SDL_WINDOWPOS_ISCENTERED(window->x) ||
        SDL_WINDOWPOS_ISUNDEFINED(window->y) || SDL_WINDOWPOS_ISCENTERED(window->y)) {
        c.x = c.y = WM_POS_DEFAULT;
    } else {
        c.x = window->x;
        c.y = window->y;
    }
    SDL_strlcpy(c.title, (window->title && *window->title) ? window->title : "SDL", WM_TITLE_MAX);
    if (ioctl(vd->fd, WM_CREATE_WINDOW, &c) < 0) {
        return SDL_SetError("indevos: WM_CREATE_WINDOW %dx%d: %s", w, h, strerror(errno));
    }
    data->id = c.id;
    if (MapSurface(data, vd->fd, c.mmap_off, c.mmap_len, c.stride, w, h) < 0) {
        ioctl(vd->fd, WM_DESTROY, (unsigned long)data->id);
        data->id = 0;
        return -1;
    }
    data->pending_w = data->pending_h = 0;
    window->w = w;
    window->h = h;
    return 0;
}

static void DestroyWMWindow(_THIS, SDL_WindowData *data)
{
    SDL_VideoData *vd = (SDL_VideoData *)_this->driverdata;
    UnmapSurface(data);
    if (data->id) {
        ioctl(vd->fd, WM_DESTROY, (unsigned long)data->id);
        data->id = 0;
    }
}

static int INDEVOS_CreateWindow(_THIS, SDL_Window *window)
{
    SDL_WindowData *data = (SDL_WindowData *)SDL_calloc(1, sizeof(*data));
    if (!data) {
        return SDL_OutOfMemory();
    }
    window->driverdata = data;
    if (CreateWMWindow(_this, window) < 0) {
        SDL_free(data);
        window->driverdata = NULL;
        return -1;
    }
    /* The WM gives a new window the focus; its FOCUS_IN event may already
       be queued.  Make SDL agree right away so key events are routed. */
    SDL_SetKeyboardFocus(window);
    return 0;
}

static void INDEVOS_DestroyWindow(_THIS, SDL_Window *window)
{
    SDL_WindowData *data = (SDL_WindowData *)window->driverdata;
    if (data) {
        DestroyWMWindow(_this, data);
        SDL_free(data);
        window->driverdata = NULL;
    }
}

static void INDEVOS_SetWindowTitle(_THIS, SDL_Window *window)
{
    SDL_VideoData *vd = (SDL_VideoData *)_this->driverdata;
    SDL_WindowData *data = (SDL_WindowData *)window->driverdata;
    struct wm_title t;
    if (!data || !data->id) {
        return;
    }
    SDL_zero(t);
    t.id = data->id;
    SDL_strlcpy(t.title, window->title ? window->title : "", WM_TITLE_MAX);
    ioctl(vd->fd, WM_SET_TITLE, &t);
}

/* The WM resizes frames only on user drags; for a program-requested size
   the window is recreated (it keeps its SDL identity, gets a new WM id). */
static void INDEVOS_SetWindowSize(_THIS, SDL_Window *window)
{
    SDL_WindowData *data = (SDL_WindowData *)window->driverdata;
    if (!data || (window->w == data->w && window->h == data->h)) {
        return;
    }
    DestroyWMWindow(_this, data);
    if (CreateWMWindow(_this, window) == 0) {
        SDL_SetKeyboardFocus(window);
    }
}

SDL_Window *INDEVOS_FindWindow(_THIS, Uint32 id)
{
    SDL_Window *w;
    for (w = _this->windows; w; w = w->next) {
        SDL_WindowData *data = (SDL_WindowData *)w->driverdata;
        if (data && data->id == id) {
            return w;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------ framebuffer */

static int INDEVOS_CreateWindowFramebuffer(_THIS, SDL_Window *window, Uint32 *format, void **pixels, int *pitch)
{
    SDL_VideoData *vd = (SDL_VideoData *)_this->driverdata;
    SDL_WindowData *data = (SDL_WindowData *)window->driverdata;
    int w, h;

    if (!data || !data->id) {
        return SDL_SetError("indevos: window has no WM surface");
    }
    SDL_GetWindowSizeInPixels(window, &w, &h);
    w = ClampDim(w, WM_MAX_W);
    h = ClampDim(h, WM_MAX_H);
    if ((w != data->w || h != data->h) && (w != data->pending_w || h != data->pending_h)) {
        /* Size changed by the program (or fullscreen), not by a user drag:
           the WM cannot resize a frame from the client, so recreate it. */
        DestroyWMWindow(_this, data);
        if (CreateWMWindow(_this, window) < 0) {
            return -1;
        }
        SDL_SetKeyboardFocus(window);
    } else if (w != data->w || h != data->h) {
        /* Acknowledge the user's resize now that SDL has dropped the old
           surface: new buffers, map them, unmap the old ones. */
        struct wm_resize_ack a;
        SDL_zero(a);
        a.id = data->id;
        a.w = (uint32_t)w;
        a.h = (uint32_t)h;
        if (ioctl(vd->fd, WM_RESIZE_ACK, &a) < 0) {
            return SDL_SetError("indevos: WM_RESIZE_ACK %dx%d: %s", w, h, strerror(errno));
        }
        if (MapSurface(data, vd->fd, a.mmap_off, a.mmap_len, a.stride, w, h) < 0) {
            return -1;
        }
    }
    data->pending_w = data->pending_h = 0;
    *format = INDEVOS_FORMAT;
    *pixels = data->map;
    *pitch = data->stride;
    return 0;
}

static int INDEVOS_UpdateWindowFramebuffer(_THIS, SDL_Window *window, const SDL_Rect *rects, int numrects)
{
    SDL_VideoData *vd = (SDL_VideoData *)_this->driverdata;
    SDL_WindowData *data = (SDL_WindowData *)window->driverdata;
    struct wm_present p;
    int x0 = data ? data->w : 0, y0 = data ? data->h : 0, x1 = 0, y1 = 0, i, x, y;

    if (!data || !data->id || !data->map) {
        return SDL_SetError("indevos: window has no WM surface");
    }
    for (i = 0; i < numrects; i++) {
        int rx0 = SDL_max(rects[i].x, 0), ry0 = SDL_max(rects[i].y, 0);
        int rx1 = SDL_min(rects[i].x + rects[i].w, data->w), ry1 = SDL_min(rects[i].y + rects[i].h, data->h);
        if (rx1 <= rx0 || ry1 <= ry0) {
            continue;
        }
        /* XRGB -> opaque premultiplied ARGB */
        for (y = ry0; y < ry1; y++) {
            Uint32 *row = (Uint32 *)((Uint8 *)data->map + (size_t)y * data->stride);
            for (x = rx0; x < rx1; x++) {
                row[x] |= 0xFF000000u;
            }
        }
        x0 = SDL_min(x0, rx0);
        y0 = SDL_min(y0, ry0);
        x1 = SDL_max(x1, rx1);
        y1 = SDL_max(y1, ry1);
    }
    if (x1 <= x0 || y1 <= y0) {
        return 0;
    }
    SDL_zero(p);
    p.id = data->id;
    p.buffer = 0;
    p.x = x0;
    p.y = y0;
    p.w = x1 - x0;
    p.h = y1 - y0;
    if (ioctl(vd->fd, WM_PRESENT, &p) < 0) {
        return SDL_SetError("indevos: WM_PRESENT: %s", strerror(errno));
    }
    return 0;
}

static void INDEVOS_DestroyWindowFramebuffer(_THIS, SDL_Window *window)
{
    /* The surface belongs to the WM window; it goes with DestroyWindow. */
    (void)_this;
    (void)window;
}

/* ------------------------------------------------------------------ mouse */

/* The WM draws the pointer.  Relative mode: SDL derives deltas from the
   absolute positions we report (the pointer is not confined). */
static int INDEVOS_SetRelativeMouseMode(SDL_bool enabled)
{
    (void)enabled;
    return 0;
}

/* ----------------------------------------------------------------- device */

static int INDEVOS_VideoInit(_THIS)
{
    SDL_VideoData *vd = (SDL_VideoData *)_this->driverdata;
    struct wm_info info;
    SDL_DisplayMode mode;

    SDL_zero(info);
    if (ioctl(vd->fd, WM_GET_INFO, &info) < 0 || info.version != WM_ABI_VERSION) {
        return SDL_SetError("indevos: /dev/wm ABI mismatch (version %u, want %u)",
                            (unsigned)info.version, (unsigned)WM_ABI_VERSION);
    }
    if (info.event_size != sizeof(struct wm_event)) {
        return SDL_SetError("indevos: /dev/wm event size %u, want %u",
                            (unsigned)info.event_size, (unsigned)sizeof(struct wm_event));
    }
    vd->screen_w = info.screen_w ? (int)info.screen_w : 1024;
    vd->screen_h = info.screen_h ? (int)info.screen_h : 768;

    SDL_zero(mode);
    mode.format = INDEVOS_FORMAT;
    mode.w = vd->screen_w;
    mode.h = vd->screen_h;
    mode.refresh_rate = 60;
    if (SDL_AddBasicVideoDisplay(&mode) < 0) {
        return -1;
    }
    SDL_AddDisplayMode(&_this->displays[0], &mode);

    SDL_GetMouse()->SetRelativeMouseMode = INDEVOS_SetRelativeMouseMode;
    INDEVOS_InitKeyboard();
    return 0;
}

static void INDEVOS_VideoQuit(_THIS)
{
    (void)_this;
}

static void INDEVOS_DeleteDevice(SDL_VideoDevice *device)
{
    SDL_VideoData *vd = (SDL_VideoData *)device->driverdata;
    if (vd) {
        if (vd->fd >= 0) {
            close(vd->fd);   /* destroys any window still open */
        }
        if (vd->wake[0] >= 0) {
            close(vd->wake[0]);
            close(vd->wake[1]);
        }
        SDL_free(vd);
    }
    SDL_free(device);
}

static SDL_VideoDevice *INDEVOS_CreateDevice(void)
{
    SDL_VideoDevice *device;
    SDL_VideoData *vd;
    int fd = open(WM_DEVICE_PATH, O_RDWR | O_CLOEXEC | O_NONBLOCK);

    if (fd < 0) {
        /* No desktop (or not inDevOS): let SDL try the next driver. */
        return NULL;
    }
    device = (SDL_VideoDevice *)SDL_calloc(1, sizeof(SDL_VideoDevice));
    vd = (SDL_VideoData *)SDL_calloc(1, sizeof(SDL_VideoData));
    if (!device || !vd) {
        SDL_free(device);
        SDL_free(vd);
        close(fd);
        SDL_OutOfMemory();
        return NULL;
    }
    vd->fd = fd;
    vd->wake[0] = vd->wake[1] = -1;
    if (pipe(vd->wake) == 0) {
        fcntl(vd->wake[0], F_SETFL, O_NONBLOCK);
        fcntl(vd->wake[1], F_SETFL, O_NONBLOCK);
        fcntl(vd->wake[0], F_SETFD, FD_CLOEXEC);
        fcntl(vd->wake[1], F_SETFD, FD_CLOEXEC);
    } else {
        vd->wake[0] = vd->wake[1] = -1;
    }
    device->driverdata = vd;

    device->VideoInit = INDEVOS_VideoInit;
    device->VideoQuit = INDEVOS_VideoQuit;
    device->PumpEvents = INDEVOS_PumpEvents;
    if (vd->wake[0] >= 0) {
        device->WaitEventTimeout = INDEVOS_WaitEventTimeout;
        device->SendWakeupEvent = INDEVOS_SendWakeupEvent;
    }
    device->CreateSDLWindow = INDEVOS_CreateWindow;
    device->SetWindowTitle = INDEVOS_SetWindowTitle;
    device->SetWindowSize = INDEVOS_SetWindowSize;
    device->DestroyWindow = INDEVOS_DestroyWindow;
    device->CreateWindowFramebuffer = INDEVOS_CreateWindowFramebuffer;
    device->UpdateWindowFramebuffer = INDEVOS_UpdateWindowFramebuffer;
    device->DestroyWindowFramebuffer = INDEVOS_DestroyWindowFramebuffer;
    device->free = INDEVOS_DeleteDevice;
    return device;
}

VideoBootStrap INDEVOS_bootstrap = {
    INDEVOS_DRIVER_NAME, "inDevOS window server (/dev/wm)",
    INDEVOS_CreateDevice,
    NULL /* no ShowMessageBox implementation */
};

#endif /* SDL_VIDEO_DRIVER_INDEVOS */

/* vi: set ts=4 sw=4 expandtab: */
