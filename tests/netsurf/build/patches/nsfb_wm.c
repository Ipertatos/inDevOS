/*
 * libnsfb surface for the inDevOS window server (/dev/wm, ABI v1 minor 1).
 * Select with "nsfb -f wm".  Pixels: libnsfb draws XRGB8888 straight into
 * buffer 0 of the window's shared mapping; update() forces alpha=0xFF on the
 * damage rect (WM wants premultiplied ARGB) and WM_PRESENTs it.  WM_PRESENT
 * is a synchronous copy, so a single buffer is enough.  The WM draws the
 * mouse pointer, so .cursor reports a "hardware" cursor and does nothing.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "libnsfb.h"
#include "libnsfb_event.h"
#include "libnsfb_plot.h"
#include "nsfb.h"
#include "surface.h"
#include "plot.h"
#include "cursor.h"
#include "wm_abi.h"

struct wm_priv {
	int fd;
	uint32_t id;
	void *map;
	size_t map_len;
	uint32_t stride;
	nsfb_event_t pending;   /* second half of a wheel click */
	bool have_pending;
};

static int wm_defaults(nsfb_t *nsfb)
{
	nsfb->width = 800;
	nsfb->height = 600;
	nsfb->format = NSFB_FMT_XRGB8888;
	select_plotters(nsfb);
	return 0;
}

static void wm_use_map(nsfb_t *nsfb, struct wm_priv *p)
{
	nsfb->ptr = p->map;              /* buffer 0 */
	nsfb->linelen = p->stride;
}

static int wm_initialise(nsfb_t *nsfb)
{
	struct wm_priv *p;
	struct wm_create c;

	if (nsfb->surface_priv != NULL)
		return -1;
	if (nsfb->format != NSFB_FMT_XRGB8888 && nsfb->format != NSFB_FMT_ARGB8888)
		nsfb->format = NSFB_FMT_XRGB8888;
	select_plotters(nsfb);

	p = calloc(1, sizeof(*p));
	if (p == NULL)
		return -1;
	p->fd = open(WM_DEVICE_PATH, O_RDWR | O_CLOEXEC);
	if (p->fd < 0) {
		free(p);
		return -1;
	}
	memset(&c, 0, sizeof(c));
	c.w = nsfb->width;
	c.h = nsfb->height;
	c.type = WM_TYPE_NORMAL;
	c.flags = WM_FLAG_RESIZABLE;
	c.x = c.y = WM_POS_DEFAULT;
	strncpy(c.title, "NetSurf", WM_TITLE_MAX - 1);
	if (ioctl(p->fd, WM_CREATE_WINDOW, &c) < 0)
		goto fail;
	p->id = c.id;
	p->stride = c.stride;
	p->map_len = c.mmap_len;
	p->map = mmap(NULL, c.mmap_len, PROT_READ | PROT_WRITE, MAP_SHARED,
		      p->fd, (off_t)c.mmap_off);
	if (p->map == MAP_FAILED)
		goto fail;
	nsfb->surface_priv = p;
	wm_use_map(nsfb, p);
	return 0;
fail:
	close(p->fd);
	free(p);
	return -1;
}

static int wm_finalise(nsfb_t *nsfb)
{
	struct wm_priv *p = nsfb->surface_priv;
	if (p != NULL) {
		munmap(p->map, p->map_len);
		close(p->fd);          /* destroys the window */
		free(p);
		nsfb->surface_priv = NULL;
		nsfb->ptr = NULL;
	}
	return 0;
}

/* Called by the frontend after an NSFB_EVENT_RESIZE (framebuffer_resize). */
static int wm_set_geometry(nsfb_t *nsfb, int width, int height,
			   enum nsfb_format_e format)
{
	struct wm_priv *p = nsfb->surface_priv;
	struct wm_resize_ack a;
	void *m;

	if (format != NSFB_FMT_ANY && format != NSFB_FMT_XRGB8888 &&
	    format != NSFB_FMT_ARGB8888)
		return -1;
	if (p == NULL) {             /* before initialise: just record it */
		if (width > 0) nsfb->width = width;
		if (height > 0) nsfb->height = height;
		if (format != NSFB_FMT_ANY) nsfb->format = format;
		select_plotters(nsfb);
		return 0;
	}
	if (width <= 0) width = nsfb->width;
	if (height <= 0) height = nsfb->height;
	memset(&a, 0, sizeof(a));
	a.id = p->id;
	a.w = width;
	a.h = height;
	if (ioctl(p->fd, WM_RESIZE_ACK, &a) < 0)
		return -1;
	m = mmap(NULL, a.mmap_len, PROT_READ | PROT_WRITE, MAP_SHARED,
		 p->fd, (off_t)a.mmap_off);
	if (m == MAP_FAILED)
		return -1;
	munmap(p->map, p->map_len);
	p->map = m;
	p->map_len = a.mmap_len;
	p->stride = a.stride;
	nsfb->width = width;
	nsfb->height = height;
	wm_use_map(nsfb, p);
	select_plotters(nsfb);
	return 0;
}

/* NSFB_WM_DEBUG=1 (or anything non-empty, not "0") logs every raw /dev/wm
 * key event and its libnsfb translation to stderr. */
static int wm_debug_keys(void)
{
	static int v = -1;
	if (v < 0) {
		const char *e = getenv("NSFB_WM_DEBUG");
		v = (e && *e && strcmp(e, "0") != 0) ? 1 : 0;
	}
	return v;
}

static enum nsfb_key_code_e wm_map_key(int k)
{
	if (k == '\n')
		return NSFB_KEY_RETURN;           /* some sources send LF */
	if (k > 0 && k < 0x100)
		return (enum nsfb_key_code_e)k;   /* ASCII / SDL1 low range */
	switch (k) {
	case 0x100: return NSFB_KEY_UP;
	case 0x101: return NSFB_KEY_DOWN;
	case 0x102: return NSFB_KEY_LEFT;
	case 0x103: return NSFB_KEY_RIGHT;
	case 0x104: return NSFB_KEY_PAGEUP;
	case 0x105: return NSFB_KEY_PAGEDOWN;
	case 0x106: return NSFB_KEY_HOME;
	case 0x107: return NSFB_KEY_END;
	case 0x108: return NSFB_KEY_F4;
	case 0x200: return NSFB_KEY_LSHIFT;
	case 0x201: return NSFB_KEY_RSHIFT;
	case 0x202: return NSFB_KEY_LCTRL;
	case 0x203: return NSFB_KEY_RCTRL;
	case 0x204: return NSFB_KEY_LALT;
	case 0x205: return NSFB_KEY_RALT;
	case 0x300: return NSFB_KEY_KP_ENTER;
	default:
		if (k >= 0x301 && k <= 0x30a)
			return (enum nsfb_key_code_e)(NSFB_KEY_KP0 + (k - 0x301));
		return NSFB_KEY_UNKNOWN;
	}
}

static bool wm_translate(struct wm_priv *p, const struct wm_event *ev,
			 nsfb_event_t *event)
{
	switch (ev->type) {
	case WM_EV_MOTION:
		event->type = NSFB_EVENT_MOVE_ABSOLUTE;
		event->value.vector.x = ev->x;
		event->value.vector.y = ev->y;
		event->value.vector.z = 0;
		return true;
	case WM_EV_BUTTON_DOWN:
	case WM_EV_BUTTON_UP:
		event->type = (ev->type == WM_EV_BUTTON_DOWN) ?
			NSFB_EVENT_KEY_DOWN : NSFB_EVENT_KEY_UP;
		switch (ev->button) {         /* WM: 1 left, 2 right, 3 middle */
		case 2:  event->value.keycode = NSFB_KEY_MOUSE_3; break;
		case 3:  event->value.keycode = NSFB_KEY_MOUSE_2; break;
		default: event->value.keycode = NSFB_KEY_MOUSE_1; break;
		}
		return true;
	case WM_EV_WHEEL:                     /* positive = up = MOUSE_4 */
		if (ev->wheel == 0)
			return false;
		event->type = NSFB_EVENT_KEY_DOWN;
		event->value.keycode = ev->wheel > 0 ? NSFB_KEY_MOUSE_4 : NSFB_KEY_MOUSE_5;
		p->pending = *event;
		p->pending.type = NSFB_EVENT_KEY_UP;
		p->have_pending = true;
		return true;
	case WM_EV_KEY_DOWN:
	case WM_EV_KEY_UP:
		event->type = (ev->type == WM_EV_KEY_DOWN) ?
			NSFB_EVENT_KEY_DOWN : NSFB_EVENT_KEY_UP;
		event->value.keycode = wm_map_key(ev->key);
		if (wm_debug_keys())
			fprintf(stderr, "[nsfb-wm] key %s raw=0x%x mods=0x%x -> nsfb=%d\n",
				ev->type == WM_EV_KEY_DOWN ? "down" : "up",
				(unsigned)ev->key, (unsigned)ev->mods,
				(int)event->value.keycode);
		return event->value.keycode != NSFB_KEY_UNKNOWN;
	case WM_EV_CLOSE_REQUEST:
		event->type = NSFB_EVENT_CONTROL;
		event->value.controlcode = NSFB_CONTROL_QUIT;
		return true;
	case WM_EV_RESIZE:
		event->type = NSFB_EVENT_RESIZE;
		event->value.resize.w = ev->w;
		event->value.resize.h = ev->h;
		return true;
	default:                              /* focus in/out: ignored */
		return false;
	}
}

/* timeout: ms, <0 = block forever, 0 = poll. */
static bool wm_input(nsfb_t *nsfb, nsfb_event_t *event, int timeout)
{
	struct wm_priv *p = nsfb->surface_priv;
	struct wm_event ev;
	struct pollfd pfd;
	ssize_t n;

	if (p == NULL)
		return false;
	if (p->have_pending) {
		*event = p->pending;
		p->have_pending = false;
		return true;
	}
	for (;;) {
		pfd.fd = p->fd;
		pfd.events = POLLIN;
		pfd.revents = 0;
		if (poll(&pfd, 1, timeout) <= 0) {
			event->type = NSFB_EVENT_CONTROL;
			event->value.controlcode = NSFB_CONTROL_TIMEOUT;
			return true;
		}
		n = read(p->fd, &ev, sizeof(ev));
		if (n != (ssize_t)sizeof(ev)) {
			if (n < 0 && (errno == EAGAIN || errno == EINTR))
				continue;
			event->type = NSFB_EVENT_CONTROL;
			event->value.controlcode = NSFB_CONTROL_QUIT;
			return true;
		}
		if (wm_translate(p, &ev, event))
			return true;
		timeout = 0;      /* swallowed an event: don't block again */
	}
}

static int wm_claim(nsfb_t *nsfb, nsfb_bbox_t *box)
{
	(void)nsfb; (void)box;
	return 0;
}

static int wm_update(nsfb_t *nsfb, nsfb_bbox_t *box)
{
	struct wm_priv *p = nsfb->surface_priv;
	struct wm_present pr;
	int x0, y0, x1, y1, x, y;

	if (p == NULL)
		return -1;
	x0 = box->x0 < 0 ? 0 : box->x0;
	y0 = box->y0 < 0 ? 0 : box->y0;
	x1 = box->x1 > nsfb->width ? nsfb->width : box->x1;
	y1 = box->y1 > nsfb->height ? nsfb->height : box->y1;
	if (x1 <= x0 || y1 <= y0)
		return 0;
	for (y = y0; y < y1; y++) {   /* XRGB -> opaque premultiplied ARGB */
		uint32_t *row = (uint32_t *)((uint8_t *)p->map + (size_t)y * p->stride);
		for (x = x0; x < x1; x++)
			row[x] |= 0xFF000000u;
	}
	pr.id = p->id;
	pr.buffer = 0;
	pr.x = x0;
	pr.y = y0;
	pr.w = x1 - x0;
	pr.h = y1 - y0;
	return ioctl(p->fd, WM_PRESENT, &pr) < 0 ? -1 : 0;
}

/* The compositor draws the system pointer. */
static int wm_cursor(nsfb_t *nsfb, struct nsfb_cursor_s *cursor)
{
	(void)nsfb; (void)cursor;
	return true;
}

const nsfb_surface_rtns_t wm_rtns = {
	.defaults = wm_defaults,
	.initialise = wm_initialise,
	.finalise = wm_finalise,
	.input = wm_input,
	.claim = wm_claim,
	.update = wm_update,
	.cursor = wm_cursor,
	.geometry = wm_set_geometry,
};

NSFB_SURFACE_DEF(wm, NSFB_SURFACE_WM, &wm_rtns)
