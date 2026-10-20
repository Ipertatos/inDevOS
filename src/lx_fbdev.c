/*
 * lx_fbdev.c - Linux fbdev (/dev/fb0) on the firmware framebuffer.
 *
 * Linux ABI only (linux_syscall.c dispatches here).  Struct layouts and
 * ioctl numbers follow <linux/fb.h>, <linux/kd.h> and <linux/vt.h>.
 *
 * Display ownership: the first open of /dev/fb0 (or KDSETMODE KD_GRAPHICS on
 * a tty) makes the calling process the display owner for the VT that is on
 * screen at that moment.  While the owner is alive, still holds an fb fd,
 * has the framebuffer mapped, or is in KD_GRAPHICS, and that VT is shown,
 * the desktop compositor, LuaDE and the text console stop painting
 * (fbdev_display_owned()).  When the owner closes everything or exits (also
 * by signal - liveness is checked lazily, no exit hook needed) the desktop
 * and the console are repainted.  Switching VTs away still works; switching
 * back gives the screen back to the app, which (like on Linux without
 * VT_SETMODE) repaints on its next frame.
 *
 * Presentation: the mapping writes straight to scanout memory on GOP/VESA.
 * Back ends with a separate host scanout (virtio-gpu) need a transfer, so
 * fbdev_tick() presents the whole frame ~30 times a second while owned.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "limine.h"
#include "mem.h"
#include "serial.h"
#include "syscall.h"
#include "uaccess.h"
#include "fb_wc.h"
#include "kernel_config.h"
#include "lx_fbdev.h"

extern struct limine_framebuffer *fb;
extern int vconsole_get_active(void);
extern void desktop_request_redraw(void);
extern void fb_mark_dirty_all(void);
extern void fb_flush(void);
extern void gpu_hal_present_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
#include "ktime.h"

#ifndef EINVAL
#define EINVAL 22
#endif
#define LX_EFAULT 14
#define LX_EINVAL 22
#define LX_ENODEV 19
#define LX_ENOTTY 25
#define LX_ENOMEM 12

/* ---- <linux/fb.h> ---- */
struct lx_fb_bitfield { uint32_t offset, length, msb_right; };
struct lx_fb_var_screeninfo {
    uint32_t xres, yres, xres_virtual, yres_virtual, xoffset, yoffset;
    uint32_t bits_per_pixel, grayscale;
    struct lx_fb_bitfield red, green, blue, transp;
    uint32_t nonstd, activate, height, width, accel_flags;
    uint32_t pixclock, left_margin, right_margin, upper_margin, lower_margin;
    uint32_t hsync_len, vsync_len, sync, vmode, rotate, colorspace;
    uint32_t reserved[4];
};
struct lx_fb_fix_screeninfo {
    char id[16];
    unsigned long smem_start;
    uint32_t smem_len, type, type_aux, visual;
    uint16_t xpanstep, ypanstep, ywrapstep;
    uint32_t line_length;
    unsigned long mmio_start;
    uint32_t mmio_len, accel;
    uint16_t capabilities;
    uint16_t reserved[2];
};
struct lx_fb_cmap { uint32_t start, len; uint64_t red, green, blue, transp; };
_Static_assert(sizeof(struct lx_fb_var_screeninfo) == 160, "fb_var_screeninfo layout");
_Static_assert(sizeof(struct lx_fb_fix_screeninfo) == 80, "fb_fix_screeninfo layout");
_Static_assert(offsetof(struct lx_fb_fix_screeninfo, line_length) == 48, "line_length offset");
_Static_assert(offsetof(struct lx_fb_fix_screeninfo, mmio_start) == 56, "mmio_start offset");

#define FBIOGET_VSCREENINFO 0x4600
#define FBIOPUT_VSCREENINFO 0x4601
#define FBIOGET_FSCREENINFO 0x4602
#define FBIOGETCMAP         0x4604
#define FBIOPUTCMAP         0x4605
#define FBIOPAN_DISPLAY     0x4606
#define FBIOBLANK           0x4611
#define FBIO_WAITFORVSYNC   0x40044620
#define FB_TYPE_PACKED_PIXELS 0
#define FB_VISUAL_TRUECOLOR   2
#define FB_ACTIVATE_TEST      2
#define FB_ACTIVATE_MASK      15
#define FB_VMODE_NONINTERLACED 0

/* ---- <linux/kd.h>, <linux/vt.h> ---- */
#define KDGKBTYPE   0x4B33
#define KDSETMODE   0x4B3A
#define KDGETMODE   0x4B3B
#define KDGKBMODE   0x4B44
#define KDSKBMODE   0x4B45
#define KDGKBMETA   0x4B62
#define KDSKBMETA   0x4B63
#define KDGKBLED    0x4B64
#define KDSKBLED    0x4B65
#define KDGETLED    0x4B31
#define KDSETLED    0x4B32
#define KD_TEXT     0
#define KD_GRAPHICS 1
#define KB_101      2
#define K_XLATE     1
#define VT_OPENQRY    0x5600
#define VT_GETMODE    0x5601
#define VT_SETMODE    0x5602
#define VT_GETSTATE   0x5603
#define VT_RELDISP    0x5605
#define VT_ACTIVATE   0x5606
#define VT_WAITACTIVE 0x5607
#define VT_DISALLOCATE 0x5608
struct lx_vt_mode { char mode, waitv; short relsig, acqsig, frsig; };
struct lx_vt_stat { uint16_t v_active, v_signal, v_state; };

/* ---- ownership state ---- */
static volatile uint32_t own_pid = 0;
static int      own_vt = 0;
static int      own_fds = 0;
static bool     own_mapped = false;
static bool     own_kdgfx = false;
static uint64_t own_present_last = 0;
static uint32_t kd_mode_global = KD_TEXT;
static uint32_t kb_mode_global = K_XLATE;
static struct lx_vt_mode vt_mode_global = { 0, 0, 0, 0, 0 };

static uint64_t fb_len(void) { return fb ? (uint64_t)fb->pitch * fb->height : 0; }
static uint64_t fb_phys(void) { return fb ? (uint64_t)fb->address - vmm_higher_half_offset : 0; }

extern bool process_pid_alive(uint32_t pid);   /* syscall.c */
static bool pid_proc(uint32_t pid) { return pid && process_pid_alive(pid); }

static void release_display(const char *why) {
    if (!own_pid) return;
    serial_printf("[fbdev] display released by pid %u (%s)\n", own_pid, why);
    own_pid = 0; own_fds = 0; own_mapped = false; own_kdgfx = false;
    kd_mode_global = KD_TEXT;
    desktop_request_redraw();
#ifdef ENABLE_LUA
    { extern void luade_notify_vt_return(void); luade_notify_vt_return(); }
#endif
    fb_mark_dirty_all();
    fb_flush();
}

/* Drop ownership when the owner is gone or holds nothing any more. */
static void check_owner(void) {
    if (!own_pid) return;
    if (!pid_proc(own_pid)) { release_display("owner exited"); return; }
    if (own_fds <= 0 && !own_mapped && !own_kdgfx) release_display("closed");
}

static void take_display(process_t *p) {
    check_owner();
    if (own_pid && own_pid != p->pid) return;   /* first owner keeps it */
    if (!own_pid) {
        own_pid = p->pid;
        own_vt = vconsole_get_active();
        own_fds = 0; own_mapped = false; own_kdgfx = false;
        own_present_last = 0;
        serial_printf("[fbdev] display owned by pid %u on VT %d\n", p->pid, own_vt);
    }
}

bool fbdev_display_owned(void) {
    if (!own_pid) return false;
    check_owner();
    return own_pid && vconsole_get_active() == own_vt;
}

void fbdev_tick(void) {
    if (!own_pid) return;
    check_owner();
    if (!own_pid || vconsole_get_active() != own_vt || !fb) return;
    uint64_t now = ktime_now_ms();
    if (now - own_present_last < 33) return;
    own_present_last = now;
    gpu_hal_present_rect(0, 0, (uint32_t)fb->width, (uint32_t)fb->height);
}

/* ---- device entry points ---- */
int fbdev_open(process_t *p) {
    if (!fb || !fb->address || fb->bpp != 32) return -LX_ENODEV;
    take_display(p);
    if (own_pid == p->pid) own_fds++;
    return 0;
}

void fbdev_close(process_t *p) {
    if (own_pid == p->pid && own_fds > 0) own_fds--;
    check_owner();
}

static void fill_var(struct lx_fb_var_screeninfo *v) {
    for (size_t i = 0; i < sizeof(*v); i++) ((uint8_t *)v)[i] = 0;
    v->xres = v->xres_virtual = (uint32_t)fb->width;
    v->yres = v->yres_virtual = (uint32_t)fb->height;
    v->bits_per_pixel = fb->bpp;
    v->red.offset   = fb->red_mask_shift;   v->red.length   = fb->red_mask_size;
    v->green.offset = fb->green_mask_shift; v->green.length = fb->green_mask_size;
    v->blue.offset  = fb->blue_mask_shift;  v->blue.length  = fb->blue_mask_size;
    v->transp.offset = 0; v->transp.length = 0;          /* like efifb (xRGB) */
    v->height = v->width = 0xFFFFFFFFu;                   /* size in mm unknown */
    v->pixclock = 10000000 / (uint32_t)(fb->width ? fb->width : 1); /* nominal */
    v->vmode = FB_VMODE_NONINTERLACED;
}

static void fill_fix(struct lx_fb_fix_screeninfo *f) {
    for (size_t i = 0; i < sizeof(*f); i++) ((uint8_t *)f)[i] = 0;
    const char *id = "inDevOS GOP";
    for (int i = 0; id[i] && i < 15; i++) f->id[i] = id[i];
    f->smem_start = fb_phys();
    f->smem_len = (uint32_t)fb_len();
    f->type = FB_TYPE_PACKED_PIXELS;
    f->visual = FB_VISUAL_TRUECOLOR;
    f->line_length = (uint32_t)fb->pitch;
}

long fbdev_ioctl(process_t *p, unsigned long req, void *arg) {
    switch (req) {
    case FBIOGET_VSCREENINFO: {
        struct lx_fb_var_screeninfo v; fill_var(&v);
        return copy_to_user(arg, &v, sizeof(v)) ? -LX_EFAULT : 0;
    }
    case FBIOPUT_VSCREENINFO: {
        struct lx_fb_var_screeninfo in, cur;
        if (copy_from_user(&in, arg, sizeof(in))) return -LX_EFAULT;
        fill_var(&cur);
        bool ok = in.xres == cur.xres && in.yres == cur.yres &&
                  (in.bits_per_pixel == cur.bits_per_pixel || in.bits_per_pixel == 0) &&
                  (in.xres_virtual == 0 || in.xres_virtual == cur.xres) &&
                  (in.yres_virtual == 0 || in.yres_virtual == cur.yres) &&
                  in.xoffset == 0 && in.yoffset == 0;
        if (!ok) {
            serial_printf("[fbdev] FBIOPUT_VSCREENINFO %ux%u@%u (virt %ux%u) rejected, mode is %ux%u@%u\n",
                          in.xres, in.yres, in.bits_per_pixel, in.xres_virtual, in.yres_virtual,
                          cur.xres, cur.yres, cur.bits_per_pixel);
            return -LX_EINVAL;
        }
        cur.activate = in.activate;
        return copy_to_user(arg, &cur, sizeof(cur)) ? -LX_EFAULT : 0;
    }
    case FBIOGET_FSCREENINFO: {
        struct lx_fb_fix_screeninfo f; fill_fix(&f);
        return copy_to_user(arg, &f, sizeof(f)) ? -LX_EFAULT : 0;
    }
    case FBIOPAN_DISPLAY: {
        struct lx_fb_var_screeninfo in;
        if (copy_from_user(&in, arg, sizeof(in))) return -LX_EFAULT;
        if (in.xoffset || in.yoffset) return -LX_EINVAL;
        if (fb) gpu_hal_present_rect(0, 0, (uint32_t)fb->width, (uint32_t)fb->height);
        return 0;
    }
    case FBIOBLANK:
        return 0;                       /* no DPMS on a firmware framebuffer */
    case FBIO_WAITFORVSYNC:
        if (fb) gpu_hal_present_rect(0, 0, (uint32_t)fb->width, (uint32_t)fb->height);
        return 0;
    case FBIOGETCMAP: case FBIOPUTCMAP:
        return -LX_EINVAL;              /* truecolor: no palette */
    default:
        return -LX_ENOTTY;
    }
}

long fbdev_read(process_t *p, uint32_t *pos, void *ubuf, size_t n) {
    uint64_t len = fb_len();
    if (*pos >= len) return 0;
    if (n > len - *pos) n = (size_t)(len - *pos);
    if (copy_to_user(ubuf, (uint8_t *)fb->address + *pos, n)) return -LX_EFAULT;
    *pos += (uint32_t)n;
    return (long)n;
}

long fbdev_write(process_t *p, uint32_t *pos, const void *ubuf, size_t n) {
    uint64_t len = fb_len();
    if (*pos >= len) return n ? -28 /* ENOSPC */ : 0;
    if (n > len - *pos) n = (size_t)(len - *pos);
    if (copy_from_user((uint8_t *)fb->address + *pos, ubuf, n)) return -LX_EFAULT;
    *pos += (uint32_t)n;
    return (long)n;
}

long fbdev_lseek(uint32_t *pos, int64_t off, long whence) {
    int64_t base = whence == 0 ? 0 : whence == 1 ? (int64_t)*pos : whence == 2 ? (int64_t)fb_len() : -1;
    if (base < 0) return -LX_EINVAL;
    int64_t np = base + off;
    if (np < 0 || np > (int64_t)fb_len()) return -LX_EINVAL;
    *pos = (uint32_t)np;
    return np;
}

long fbdev_mmap(process_t *p, uint64_t addr, uint64_t len, long prot, long flags, int64_t off) {
    extern long sys_mmap_device(uint64_t addr, uint64_t len, int prot, int fixed,
                                uint64_t phys, uint64_t pte_extra);
    uint64_t total = (fb_len() + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    if (off < 0 || (off & (PAGE_SIZE - 1)) || (uint64_t)off >= total || len == 0) return -LX_EINVAL;
    if (len > total - (uint64_t)off) return -LX_EINVAL;
    long va = sys_mmap_device(addr, len, (int)prot, (flags & 0x10) != 0 /* MAP_FIXED */,
                              fb_phys() + (uint64_t)off, fb_wc_pte_flags());
    if (va >= 0) {
        take_display(p);
        if (own_pid == p->pid) own_mapped = true;
        serial_printf("[fbdev] pid %u mapped %llu bytes at %lx (%s)\n", p->pid,
                      (unsigned long long)len, va, fb_wc_pat_ready() ? "WC" : "UC");
    }
    return va;
}

/* KD / VT ioctls on a tty fd; *handled=false when the request is not ours. */
long fbdev_tty_ioctl(process_t *p, unsigned long req, void *arg, bool *handled) {
    *handled = true;
    switch (req) {
    case KDSETMODE: {
        unsigned long m = (unsigned long)arg;
        if (m == KD_GRAPHICS) {
            take_display(p);
            if (own_pid == p->pid) { own_kdgfx = true; kd_mode_global = KD_GRAPHICS; }
        } else if (m == KD_TEXT) {
            if (own_pid == p->pid) { own_kdgfx = false; kd_mode_global = KD_TEXT; check_owner(); }
            else if (!own_pid) kd_mode_global = KD_TEXT;
        } else return -LX_EINVAL;
        return 0;
    }
    case KDGETMODE: { int m = (int)kd_mode_global; return copy_to_user(arg, &m, sizeof(m)) ? -LX_EFAULT : 0; }
    case KDGKBTYPE: { char t = KB_101; return copy_to_user(arg, &t, 1) ? -LX_EFAULT : 0; }
    case KDGKBMODE: { int m = (int)kb_mode_global; return copy_to_user(arg, &m, sizeof(m)) ? -LX_EFAULT : 0; }
    case KDSKBMODE:
        /* Accepted and remembered; input stays translated (termios raw mode
         * is what the keyboard path honours). */
        kb_mode_global = (uint32_t)(unsigned long)arg;
        return 0;
    case KDGKBMETA: { int m = 3 /* K_ESCPREFIX */; return copy_to_user(arg, &m, sizeof(m)) ? -LX_EFAULT : 0; }
    case KDSKBMETA: case KDSKBLED: case KDSETLED: return 0;
    case KDGKBLED: case KDGETLED: { char z = 0; return copy_to_user(arg, &z, 1) ? -LX_EFAULT : 0; }
    case VT_OPENQRY: { int n = vconsole_get_active() + 1; return copy_to_user(arg, &n, sizeof(n)) ? -LX_EFAULT : 0; }
    case VT_GETMODE: return copy_to_user(arg, &vt_mode_global, sizeof(vt_mode_global)) ? -LX_EFAULT : 0;
    case VT_SETMODE:
        if (copy_from_user(&vt_mode_global, arg, sizeof(vt_mode_global))) return -LX_EFAULT;
        return 0;                       /* VT_PROCESS signals not delivered */
    case VT_GETSTATE: {
        struct lx_vt_stat s = { (uint16_t)(vconsole_get_active() + 1), 0, 0x1F };
        return copy_to_user(arg, &s, sizeof(s)) ? -LX_EFAULT : 0;
    }
    case VT_RELDISP: case VT_WAITACTIVE: case VT_DISALLOCATE:
        return 0;
    case VT_ACTIVATE: {
        extern void vconsole_switch(int id);
        long n = (long)(unsigned long)arg;
        if (n >= 1 && n <= 4) vconsole_switch((int)n - 1);
        return 0;
    }
    default:
        *handled = false;
        return 0;
    }
}
