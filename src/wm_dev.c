/*
 * wm_dev.c - /dev/wm: window-server device for Linux-ABI (.exe) clients.
 *
 * Phase 2 of docs/DESKTOP_PLAN.md.  The kernel desktop (desktop.c) stays the
 * shell and compositor; a client opens /dev/wm, creates windows with ioctls,
 * draws into a shared double-buffered surface (mmap), presents damage and
 * reads fixed-size input events.  ABI: wm_abi.h.
 *
 * Surfaces: one kalloc() block per window (2 buffers, page aligned).  Blocks
 * larger than a slab come from the page allocator and are physically
 * contiguous in the HHDM, so they are mapped with sys_mmap_device (VMM_DEVICE
 * PTEs: munmap / exit never free the frames; we do, on destroy).
 * WM_PRESENT copies the damaged rect of the chosen buffer into the desktop
 * window's own pixel buffer under content_lock, so the compositor never reads
 * client memory and never waits for a client.
 *
 * Lifetime: windows belong to the connection.  close(), exit and crash all
 * end in wm_dev_release(), which destroys the remaining desktop windows and
 * frees the surfaces.  A surface that may still be mapped in a live address
 * space is unmapped first (or, if that is impossible, leaked rather than
 * freed - never a use-after-free).
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "limine.h"
#include "mem.h"
#include "serial.h"
#include "string.h"
#include "syscall.h"
#include "uaccess.h"
#include "thread.h"
#include "ktime.h"
#include "atomic.h"
#include "desktop.h"
#include "wm_abi.h"
#include "wm_dev.h"

#define WM_EPERM   1
#define WM_ENOENT  2
#define WM_EINTR   4
#define WM_EAGAIN 11
#define WM_ENOMEM 12
#define WM_EFAULT 14
#define WM_EBUSY  16
#define WM_EINVAL 22
#define WM_ENOTTY 25
#define WM_ENOSPC 28

#define WM_QLEN       128
/* mmap_off = (slot+1) << 28 | gen << 25 | byte offset (surface <= 32 MiB) */
#define WM_SLOT_SHIFT 28
#define WM_GEN_SHIFT  25
#define WM_GEN_MASK   7u

extern struct limine_framebuffer *fb;
extern bool process_pid_alive(uint32_t pid);
extern long sys_mmap_device(uint64_t addr, uint64_t len, int prot, int fixed,
                            uint64_t phys, uint64_t pte_extra);
void desktop_damage_client_rect(desktop_window_t *w, int x, int y, int ww, int hh);
extern bool exec_current_is_sync_program(void);

typedef struct {
    uint8_t  *mem;                          /* kalloc, 2 * buf_size          */
    uint64_t  len;
    uint32_t  w, h, stride, buf_size;
    /* mapping (for unmap before free) */
    bool      mapped;
    uint64_t  map_va, map_pages;
    uint64_t *map_pml4;
} wm_surf_t;

typedef struct {
    bool      used;
    uint32_t  desk_id;
    uint32_t  slot;
    uint32_t  gen;                          /* bumped by every RESIZE_ACK    */
    wm_surf_t cur;                          /* presented / mappable          */
    wm_surf_t old;                          /* previous gen, freed next ack  */
} wm_win_t;

typedef struct wm_conn {
    struct wm_conn *next;
    uint32_t  pid;
    uint32_t  serial;
    wm_win_t  win[WM_MAX_WINDOWS_PER_FD];
    struct wm_event q[WM_QLEN];
    uint32_t  qh, qt;                       /* head (read), tail (write)     */
    uint32_t  dropped;
} wm_conn_t;

static spinlock_t  wm_lock;                 /* conn list + queues + win[]    */
static wm_conn_t  *wm_conns;
static uint32_t    wm_focus_last;
static uint32_t    wm_conn_serial;

/* ---- helpers (call with wm_lock held) ---- */
static wm_win_t *find_win_locked(uint32_t desk_id, wm_conn_t **out) {
    if (!desk_id) return NULL;
    for (wm_conn_t *c = wm_conns; c; c = c->next)
        for (int i = 0; i < WM_MAX_WINDOWS_PER_FD; i++)
            if (c->win[i].used && c->win[i].desk_id == desk_id) {
                if (out) *out = c;
                return &c->win[i];
            }
    return NULL;
}

static void q_push_locked(wm_conn_t *c, const struct wm_event *ev) {
    uint32_t n = c->qt - c->qh;
    if ((ev->type == WM_EV_MOTION || ev->type == WM_EV_RESIZE) && n > 0) {
        struct wm_event *last = &c->q[(c->qt - 1) % WM_QLEN];
        if (last->type == ev->type && last->win == ev->win) { *last = *ev; return; }
    }
    if (n >= WM_QLEN) { c->dropped++; return; }
    c->q[c->qt % WM_QLEN] = *ev;
    c->qt++;
}

static void post(uint32_t desk_id, struct wm_event *ev) {
    spinlock_lock(&wm_lock);
    wm_conn_t *c = NULL;
    if (find_win_locked(desk_id, &c)) {
        ev->win = desk_id;
        ev->time_ms = (uint32_t)ktime_now_ms();
        q_push_locked(c, ev);
    }
    spinlock_unlock(&wm_lock);
}

/* Remove a surface's pages from the address space that mapped them, if that
 * address space is the current one.  Returns true when nothing can still
 * reference the surface (never mapped, or unmapped now). */
static bool unmap_surface(wm_surf_t *sf) {
    if (!sf->mapped) return true;
    process_t *cur = get_current_process();
    if (!cur || !cur->pml4 || cur->pml4 != sf->map_pml4)
        return false;               /* another (or a destroyed) address space */
    uint64_t base = virt_to_phys(sf->mem);
    for (uint64_t i = 0; i < sf->map_pages; i++) {
        uint64_t va = sf->map_va + i * PAGE_SIZE;
        uint64_t *pte = vmm_get_pte_in(sf->map_pml4, va);
        if (!pte || !(*pte & VMM_PRESENT)) continue;      /* client munmapped */
        uint64_t phys = *pte & PTE_ADDR_MASK;
        if (phys < base || phys >= base + sf->len) continue;  /* remapped */
        *pte = 0;
        __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
    }
    sf->mapped = false;
    return true;
}

/* Free a surface unless it may still be mapped in a live address space
 * (then it is leaked and logged: never a use-after-free).  owner_gone: the
 * owner's address space has been destroyed (exit), so nothing maps it. */
static void surf_free(wm_surf_t *sf, uint32_t id, bool owner_gone) {
    if (!sf->mem) return;
    bool can_free = unmap_surface(sf) || owner_gone;
    if (can_free) kfree(sf->mem);
    else serial_printf("[wm] window %u surface still mapped elsewhere, leaked %llu bytes\n",
                       id, (unsigned long long)sf->len);
    memset(sf, 0, sizeof(*sf));
}

static bool surf_alloc(wm_surf_t *sf, uint32_t w, uint32_t h) {
    memset(sf, 0, sizeof(*sf));
    uint32_t stride = w * 4;
    uint64_t bsz = ((uint64_t)stride * h + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t total = bsz * 2;
    uint8_t *m = (uint8_t *)kalloc((size_t)total);
    if (!m || ((uint64_t)m & (PAGE_SIZE - 1))) { if (m) kfree(m); return false; }
    memset(m, 0, (size_t)total);
    sf->mem = m; sf->len = total;
    sf->w = w; sf->h = h; sf->stride = stride; sf->buf_size = (uint32_t)bsz;
    return true;
}

static uint64_t win_mmap_off(const wm_win_t *w) {
    return ((uint64_t)(w->slot + 1) << WM_SLOT_SHIFT) |
           ((uint64_t)(w->gen & WM_GEN_MASK) << WM_GEN_SHIFT);
}

/* Destroy one window: desktop window (if it still exists) and surfaces. */
static void win_teardown(wm_win_t *w, bool owner_gone) {
    desktop_window_t *dw = desktop_find_window_by_id(w->desk_id);
    if (dw) desktop_destroy_window(dw);
    surf_free(&w->old, w->desk_id, owner_gone);
    surf_free(&w->cur, w->desk_id, owner_gone);
    memset(w, 0, sizeof(*w));
}

/* ---- file operations ---- */
long wm_dev_open(void **handle) {
    wm_conn_t *c = (wm_conn_t *)kalloc(sizeof(wm_conn_t));
    if (!c) return -WM_ENOMEM;
    memset(c, 0, sizeof(*c));
    process_t *p = get_current_process();
    c->pid = p ? p->pid : 0;
    spinlock_lock(&wm_lock);
    c->serial = ++wm_conn_serial;
    c->next = wm_conns;
    wm_conns = c;
    spinlock_unlock(&wm_lock);
    serial_printf("[wm] connect %u pid %u\n", c->serial, c->pid);
    *handle = c;
    return 0;
}

void wm_dev_release(void *handle) {
    wm_conn_t *c = (wm_conn_t *)handle;
    if (!c) return;
    spinlock_lock(&wm_lock);
    for (wm_conn_t **pp = &wm_conns; *pp; pp = &(*pp)->next)
        if (*pp == c) { *pp = c->next; break; }
    spinlock_unlock(&wm_lock);
    process_t *cur = get_current_process();
    bool owner_gone = !process_pid_alive(c->pid) ||
                      (cur && cur->pid == c->pid && !cur->pml4);
    int n = 0;
    for (int i = 0; i < WM_MAX_WINDOWS_PER_FD; i++)
        if (c->win[i].used) { win_teardown(&c->win[i], owner_gone); n++; }
    serial_printf("[wm] client %u gone (pid %u), %d window(s) destroyed, %u events dropped\n",
                  c->serial, c->pid, n, c->dropped);
    kfree(c);
}

static long do_create(wm_conn_t *c, void *arg) {
    struct wm_create cr;
    if (copy_from_user(&cr, arg, sizeof(cr))) return -WM_EFAULT;
    if (cr.w == 0 || cr.h == 0 || cr.w > WM_MAX_W || cr.h > WM_MAX_H) return -WM_EINVAL;
    if (cr.type > WM_TYPE_POPUP) return -WM_EINVAL;
    cr.title[WM_TITLE_MAX - 1] = 0;

    int slot = -1;
    spinlock_lock(&wm_lock);
    for (int i = 0; i < WM_MAX_WINDOWS_PER_FD; i++)
        if (!c->win[i].used) { slot = i; c->win[i].used = true; c->win[i].desk_id = 0; break; }
    spinlock_unlock(&wm_lock);
    if (slot < 0) return -WM_ENOSPC;
    wm_win_t *w = &c->win[slot];

    wm_surf_t sf;
    if (!surf_alloc(&sf, cr.w, cr.h)) {
        spinlock_lock(&wm_lock); w->used = false; spinlock_unlock(&wm_lock);
        return -WM_ENOMEM;
    }

    int x = cr.x, y = cr.y;
    if (cr.x == WM_POS_DEFAULT || cr.y == WM_POS_DEFAULT) {
        static int cascade;
        x = 160 + 32 * (cascade % 8);
        y = 80 + 32 * (cascade % 8);
        cascade++;
    }
    desktop_window_t *dw = desktop_create_window_ex(c->pid, x, y, (int)cr.w, (int)cr.h,
                                                    cr.title[0] ? cr.title : "Window",
                                                    (int)cr.type, cr.parent, DESKTOP_FMT_ARGB);
    if (!dw) {
        kfree(sf.mem);
        spinlock_lock(&wm_lock); w->used = false; spinlock_unlock(&wm_lock);
        return -WM_ENOMEM;
    }
    if (cr.type == WM_TYPE_NORMAL) dw->resizable = (cr.flags & WM_FLAG_RESIZABLE) != 0;

    spinlock_lock(&wm_lock);
    w->desk_id = dw->id;
    w->slot = (uint32_t)slot;
    w->gen = 0;
    w->cur = sf;
    spinlock_unlock(&wm_lock);

    cr.id = dw->id;
    cr.stride = sf.stride;
    cr.buf_size = sf.buf_size;
    cr.nbuf = 2;
    cr.mmap_off = win_mmap_off(w);
    cr.mmap_len = sf.len;
    serial_printf("[wm] create id %u pid %u %ux%u type %u '%s'\n",
                  dw->id, c->pid, cr.w, cr.h, cr.type, dw->title);
    if (copy_to_user(arg, &cr, sizeof(cr))) return -WM_EFAULT;   /* window stays; fd close frees */
    return 0;
}

static wm_win_t *conn_win(wm_conn_t *c, uint32_t id) {
    if (!id) return NULL;
    for (int i = 0; i < WM_MAX_WINDOWS_PER_FD; i++)
        if (c->win[i].used && c->win[i].desk_id == id) return &c->win[i];
    return NULL;
}

static long do_present(wm_conn_t *c, void *arg) {
    struct wm_present pr;
    if (copy_from_user(&pr, arg, sizeof(pr))) return -WM_EFAULT;
    wm_win_t *w = conn_win(c, pr.id);
    if (!w || pr.buffer > 1) return -WM_EINVAL;
    desktop_window_t *dw = desktop_find_window_by_id(w->desk_id);
    if (!dw || !dw->pixels) return -WM_ENOENT;        /* destroyed by the WM */
    int x0 = pr.x, y0 = pr.y, x1, y1;
    if (pr.w <= 0 || pr.h <= 0) { x0 = 0; y0 = 0; x1 = (int)w->cur.w; y1 = (int)w->cur.h; }
    else { x1 = x0 + pr.w; y1 = y0 + pr.h; }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    wm_surf_t *sf = &w->cur;
    if (x1 > (int)sf->w) x1 = (int)sf->w;
    if (y1 > (int)sf->h) y1 = (int)sf->h;
    const uint8_t *src = sf->mem + (size_t)pr.buffer * sf->buf_size;
    spinlock_lock(&dw->content_lock);
    if (x1 > dw->content_w) x1 = dw->content_w;
    if (y1 > dw->content_h) y1 = dw->content_h;
    if (x1 > x0 && y1 > y0 && dw->pixels) {
        for (int yy = y0; yy < y1; yy++)
            memcpy((uint8_t *)dw->pixels + (size_t)yy * dw->pixel_pitch + (size_t)x0 * 4,
                   src + (size_t)yy * sf->stride + (size_t)x0 * 4, (size_t)(x1 - x0) * 4);
    }
    spinlock_unlock(&dw->content_lock);
    if (x1 > x0 && y1 > y0) desktop_damage_client_rect(dw, x0, y0, x1 - x0, y1 - y0);
    if (exec_current_is_sync_program()) desktop_sync_pump();   /* kmain is blocked */
    return 0;
}

/* WM_RESIZE_ACK: new double buffers at the acknowledged size.  The previous
 * surface is kept (still mapped, still valid) until the next ack or close;
 * the one before it is unmapped from the caller and freed now. */
static long do_resize_ack(wm_conn_t *c, void *arg) {
    struct wm_resize_ack ra;
    if (copy_from_user(&ra, arg, sizeof(ra))) return -WM_EFAULT;
    if (ra.w == 0 || ra.h == 0 || ra.w > WM_MAX_W || ra.h > WM_MAX_H) return -WM_EINVAL;
    wm_win_t *w = conn_win(c, ra.id);
    if (!w) return -WM_EINVAL;
    if (!desktop_find_window_by_id(w->desk_id)) return -WM_ENOENT;
    wm_surf_t nsf;
    if (!surf_alloc(&nsf, ra.w, ra.h)) return -WM_ENOMEM;
    wm_surf_t older;
    spinlock_lock(&wm_lock);
    older = w->old;
    w->old = w->cur;
    w->cur = nsf;
    w->gen++;
    spinlock_unlock(&wm_lock);
    surf_free(&older, w->desk_id, false);
    ra.stride = nsf.stride;
    ra.buf_size = nsf.buf_size;
    ra.nbuf = 2;
    ra.mmap_off = win_mmap_off(w);
    ra.mmap_len = nsf.len;
    serial_printf("[wm] resize ack id %u -> %ux%u gen %u\n", w->desk_id, ra.w, ra.h, w->gen);
    return copy_to_user(arg, &ra, sizeof(ra)) ? -WM_EFAULT : 0;
}

static long do_title(wm_conn_t *c, void *arg) {
    struct wm_title t;
    if (copy_from_user(&t, arg, sizeof(t))) return -WM_EFAULT;
    t.title[WM_TITLE_MAX - 1] = 0;
    wm_win_t *w = conn_win(c, t.id);
    if (!w) return -WM_EINVAL;
    desktop_window_t *dw = desktop_find_window_by_id(w->desk_id);
    if (!dw) return -WM_ENOENT;
    int i = 0;
    for (; i < DESKTOP_TITLE_MAX - 1 && t.title[i]; i++) dw->title[i] = t.title[i];
    dw->title[i] = 0;
    desktop_request_redraw();
    return 0;
}

static long do_destroy(wm_conn_t *c, uint32_t id) {
    spinlock_lock(&wm_lock);
    wm_win_t *w = conn_win(c, id);
    wm_win_t copy;
    if (w) { copy = *w; memset(w, 0, sizeof(*w)); }
    spinlock_unlock(&wm_lock);
    if (!w) return -WM_EINVAL;
    serial_printf("[wm] destroy id %u pid %u\n", id, c->pid);
    win_teardown(&copy, false);
    return 0;
}

long wm_dev_ioctl(void *handle, unsigned long req, void *arg) {
    wm_conn_t *c = (wm_conn_t *)handle;
    if (!c) return -WM_EINVAL;
    req = (uint32_t)req;    /* musl passes int: drop the sign extension, like Linux */
    switch (req) {
    case WM_CREATE_WINDOW: return do_create(c, arg);
    case WM_PRESENT:       return do_present(c, arg);
    case WM_SET_TITLE:     return do_title(c, arg);
    case WM_DESTROY:       return do_destroy(c, (uint32_t)(unsigned long)arg);
    case WM_RESIZE_ACK:    return do_resize_ack(c, arg);
    case WM_GET_INFO: {
        struct wm_info in;
        memset(&in, 0, sizeof(in));
        in.version = WM_ABI_VERSION;
        in.screen_w = fb ? (uint32_t)fb->width : 0;
        in.screen_h = fb ? (uint32_t)fb->height : 0;
        in.max_windows = WM_MAX_WINDOWS_PER_FD;
        in.event_size = sizeof(struct wm_event);
        in.minor = WM_ABI_MINOR;
        return copy_to_user(arg, &in, sizeof(in)) ? -WM_EFAULT : 0;
    }
    default:
        return -WM_ENOTTY;
    }
}

bool wm_dev_poll_ready(void *handle) {
    wm_conn_t *c = (wm_conn_t *)handle;
    return c && c->qt != c->qh;
}

long wm_dev_read(void *handle, void *ubuf, size_t n, bool nonblock) {
    wm_conn_t *c = (wm_conn_t *)handle;
    if (!c) return -WM_EINVAL;
    size_t want = n / sizeof(struct wm_event);
    if (want == 0) return -WM_EINVAL;
    for (;;) {
        if (c->qt != c->qh) break;
        if (nonblock) return -WM_EAGAIN;
        process_t *p = get_current_process();
        if (p && proc_kill_pending(p)) return -WM_EINTR;
        if (exec_current_is_sync_program()) desktop_sync_pump();
        thread_sleep(1);
    }
    size_t done = 0;
    while (done < want) {
        struct wm_event ev;
        spinlock_lock(&wm_lock);
        if (c->qt == c->qh) { spinlock_unlock(&wm_lock); break; }
        ev = c->q[c->qh % WM_QLEN];
        c->qh++;
        spinlock_unlock(&wm_lock);
        if (copy_to_user((uint8_t *)ubuf + done * sizeof(ev), &ev, sizeof(ev)))
            return done ? (long)(done * sizeof(ev)) : -WM_EFAULT;
        done++;
    }
    return (long)(done * sizeof(struct wm_event));
}

long wm_dev_mmap(void *handle, uint64_t addr, uint64_t len, long prot, long flags, int64_t off) {
    wm_conn_t *c = (wm_conn_t *)handle;
    if (!c || off <= 0 || (off & (PAGE_SIZE - 1)) || len == 0) return -WM_EINVAL;
    uint64_t slot = ((uint64_t)off >> WM_SLOT_SHIFT) - 1;
    uint32_t gen = (uint32_t)(((uint64_t)off >> WM_GEN_SHIFT) & WM_GEN_MASK);
    uint64_t sub = (uint64_t)off & (((uint64_t)1 << WM_GEN_SHIFT) - 1);
    if (slot >= WM_MAX_WINDOWS_PER_FD) return -WM_EINVAL;
    wm_win_t *w = &c->win[slot];
    if (!w->used || !w->cur.mem) return -WM_EINVAL;
    if (gen != (w->gen & WM_GEN_MASK)) return -WM_EINVAL;   /* stale offset */
    wm_surf_t *sf = &w->cur;
    if (sub >= sf->len || len > sf->len - sub) return -WM_EINVAL;
    if (sf->mapped) return -WM_EBUSY;                 /* one mapping per surface */
    long va = sys_mmap_device(addr, len, (int)prot, (flags & 0x10) != 0 /* MAP_FIXED */,
                              virt_to_phys(sf->mem) + sub, 0 /* write-back */);
    if (va < 0) return va;
    process_t *p = get_current_process();
    sf->mapped = true;
    sf->map_va = (uint64_t)va;
    sf->map_pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
    sf->map_pml4 = p ? p->pml4 : NULL;
    serial_printf("[wm] map id %u gen %u pid %u %llu bytes at %lx\n", w->desk_id, w->gen, c->pid,
                  (unsigned long long)len, va);
    return va;
}

bool wm_dev_pid_connected(uint32_t pid) {
    if (!wm_conns || !pid) return false;
    spinlock_lock(&wm_lock);
    bool r = false;
    for (wm_conn_t *c = wm_conns; c && !r; c = c->next) r = c->pid == pid;
    spinlock_unlock(&wm_lock);
    return r;
}

/* ---- desktop.c hooks ---- */
bool wm_dev_owns_window(uint32_t desk_id) {
    if (!wm_conns) return false;
    spinlock_lock(&wm_lock);
    bool r = find_win_locked(desk_id, NULL) != NULL;
    spinlock_unlock(&wm_lock);
    return r;
}

void wm_dev_pointer(uint32_t desk_id, const desktop_ptr_event_t *p) {
    struct wm_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = (uint32_t)p->type;           /* DESKTOP_PTR_* == WM_EV_* 1..4 */
    ev.x = p->x; ev.y = p->y;
    ev.button = p->button; ev.clicks = p->clicks; ev.wheel = p->wheel;
    ev.mods = p->mods;
    post(desk_id, &ev);
}

bool wm_dev_key(uint32_t desk_id, int32_t keycode, bool pressed) {
    if (!wm_dev_owns_window(desk_id)) return false;
    struct wm_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = pressed ? WM_EV_KEY_DOWN : WM_EV_KEY_UP;
    ev.key = keycode;
    {
        /* Modifier state tracked from the key events themselves, in queue
         * order (the PS/2 ISR flags run ahead of the deferred SDL queue, so
         * reading them here gave stale mods on fast typing). SDL keycodes:
         * 0x200/0x201 shift, 0x202/0x203 ctrl, 0x204/0x205 alt, 0x206 gui. */
        static uint32_t held;      /* bit per modifier key 0x200..0x206 */
        if (keycode >= 0x200 && keycode <= 0x206) {
            uint32_t bit = 1u << (keycode - 0x200);
            if (pressed) held |= bit; else held &= ~bit;
        }
        uint32_t m = 0;
        if (held & 0x03) m |= WM_MOD_SHIFT;
        if (held & 0x0c) m |= WM_MOD_CTRL;
        if (held & 0x30) m |= WM_MOD_ALT;
        if (held & 0x40) m |= WM_MOD_SUPER;
        ev.mods = m;
    }
    post(desk_id, &ev);
    return true;
}

bool wm_dev_close_request(uint32_t desk_id) {
    if (!wm_dev_owns_window(desk_id)) return false;
    struct wm_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = WM_EV_CLOSE_REQUEST;
    post(desk_id, &ev);
    serial_printf("[wm] close request id %u\n", desk_id);
    return true;
}

void wm_dev_resized(uint32_t desk_id, int w, int h) {
    if (!wm_conns) return;
    struct wm_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = WM_EV_RESIZE;
    ev.w = (uint32_t)w; ev.h = (uint32_t)h;
    post(desk_id, &ev);
}

void wm_dev_focus_update(uint32_t focused) {
    if (focused == wm_focus_last) return;
    uint32_t old = wm_focus_last;
    wm_focus_last = focused;
    if (!wm_conns) return;
    struct wm_event ev;
    memset(&ev, 0, sizeof(ev));
    if (old) { ev.type = WM_EV_FOCUS_OUT; post(old, &ev); }
    if (focused) { memset(&ev, 0, sizeof(ev)); ev.type = WM_EV_FOCUS_IN; post(focused, &ev); }
}
