/*
 * input.c - kernel input core + Linux evdev character devices.
 *
 * See input.h.  Locking: one spinlock (input_lock) taken with interrupts
 * disabled protects device state and every client ring; sources may call
 * input_event() from IRQ handlers, the main loop or syscalls.  Kernel sync
 * handlers run after the lock is dropped.
 */
#include "input.h"
#include "atomic.h"
#include "mem.h"
#include "string.h"
#include "serial.h"
#include "ktime.h"
#include "uaccess.h"
#include "syscall.h"

extern uint64_t rtc_unix_epoch(void);
extern void thread_sleep(uint64_t ms);
extern process_t *get_current_process(void);
extern void usb_poll(void);

/* Polled input sources (xHCI HID) are normally drained by the kernel main
 * loop, which does not run while a foreground program is blocked in an
 * evdev read/poll. Pump them from the waiter too; usb_poll() is rate-limited
 * and uses a trylock, so concurrent callers are harmless. */
static void input_pump(void) {
    usb_poll();
}

#define LX_EINVAL 22
#define LX_EFAULT 14
#define LX_EAGAIN 11
#define LX_ENODEV 19
#define LX_EINTR  4
#define LX_EBUSY  16
#define LX_ENOENT 2
#define LX_ENOTTY 25
#define LX_ENOMEM 12

struct lx_input_event { uint64_t sec, usec; uint16_t type, code; int32_t value; };
_Static_assert(sizeof(struct lx_input_event) == 24, "input_event is 24 bytes on x86-64");

#define EVDEV_RING 256
typedef struct evdev_client {
    input_dev_t *dev;
    struct { uint64_t us; uint16_t type, code; int32_t value; } ring[EVDEV_RING];
    uint32_t head, tail;               /* head: next write, tail: next read */
    bool     revoked;
    bool     monotonic;
    struct evdev_client *next;
} evdev_client_t;

static spinlock_t input_lock = {0};
static input_dev_t *devs[INPUT_MAX_DEVICES];
static int dev_clients[INPUT_MAX_DEVICES];
static evdev_client_t *clients;
static input_sync_fn sync_handlers[4];
static int n_sync_handlers;
input_dev_t *input_ps2_kbd, *input_ps2_mouse;

static inline uint64_t ilock(void) { uint64_t f = irq_save(); spinlock_lock(&input_lock); return f; }
static inline void iunlock(uint64_t f) { spinlock_unlock(&input_lock); irq_restore(f); }

void input_init(void) {}

static bool kstrcmp_input(const char *a, const char *b) {   /* equal, ignoring one trailing '/' */
    while (*a && *a == *b) { a++; b++; }
    return *b == 0 && (*a == 0 || (a[0] == '/' && a[1] == 0));
}

static void kcopy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    if (src) for (; src[i] && i + 1 < n; i++) dst[i] = src[i];
    dst[i] = 0;
}

input_dev_t *input_allocate(const char *name, uint16_t bus, uint16_t vendor, uint16_t product) {
    static bool handlers_added;
    if (!handlers_added) {              /* built-in handler: the desktop pointer (mouse.c) */
        extern void mouse_input_sync(input_dev_t *d);
        handlers_added = true;
        input_add_sync_handler(mouse_input_sync);
    }
    input_dev_t *d = (input_dev_t *)kalloc(sizeof(input_dev_t));
    if (!d) return NULL;
    memset(d, 0, sizeof(*d));
    d->minor = -1;
    kcopy(d->name, name, sizeof(d->name));
    d->bustype = bus; d->vendor = vendor; d->product = product; d->version = 1;
    input_set_bit(d->evbit, EV_SYN);
    return d;
}

void input_set_capability(input_dev_t *d, unsigned type, unsigned code) {
    if (!d || type > EV_MAX) return;
    input_set_bit(d->evbit, type);
    switch (type) {
    case EV_KEY: if (code <= KEY_MAX) input_set_bit(d->keybit, code); break;
    case EV_REL: if (code <= REL_MAX) input_set_bit(d->relbit, code); break;
    case EV_ABS: if (code <= ABS_MAX) input_set_bit(d->absbit, code); break;
    case EV_MSC: if (code <= MSC_MAX) input_set_bit(d->mscbit, code); break;
    case EV_LED: if (code <= LED_MAX) input_set_bit(d->ledbit, code); break;
    default: break;
    }
}

int input_register(input_dev_t *d) {
    if (!d) return -1;
    uint64_t f = ilock();
    int slot = -1;
    for (int i = 0; i < INPUT_MAX_DEVICES; i++) {
        if (!devs[i] || (!devs[i]->present && dev_clients[i] == 0)) { slot = i; break; }
    }
    if (slot >= 0) {
        if (devs[slot] && devs[slot] != d && !devs[slot]->owner) kfree(devs[slot]);
        devs[slot] = d;
        d->minor = slot;
        d->present = true;
        if (!d->phys[0]) {
            char *p = d->phys;
            const char *pre = d->bustype == BUS_USB ? "usb-" : d->bustype == BUS_I8042 ? "isa0060/serio" : "virtual/input";
            kcopy(p, pre, sizeof(d->phys));
            size_t l = 0; while (p[l]) l++;
            if (l + 3 < sizeof(d->phys)) { p[l] = (char)('0' + slot / 10); p[l + 1] = (char)('0' + slot % 10); p[l + 2] = 0; }
        }
    }
    iunlock(f);
    if (slot < 0) { serial_printf("input: no free slot for '%s'\n", d->name); return -1; }
    serial_printf("input: %s as /dev/input/event%d\n", d->name, slot);
    return 0;
}

void input_unregister(input_dev_t *d) {
    if (!d || !d->present) return;
    /* release everything still held so nobody keeps a stuck key/button */
    bool any = false;
    for (unsigned k = 0; k <= KEY_MAX; k++)
        if (input_test_bit(d->key, k)) { input_event(d, EV_KEY, k, 0); any = true; }
    if (any) input_sync(d);
    uint64_t f = ilock();
    d->present = false;
    d->grab = NULL;
    iunlock(f);
    serial_printf("input: %s (event%d) removed\n", d->name, d->minor);
}

void input_add_sync_handler(input_sync_fn fn) {
    if (n_sync_handlers < 4) sync_handlers[n_sync_handlers++] = fn;
}

bool input_is_pointer(const input_dev_t *d) {
    return (input_test_bit(d->relbit, REL_X) && input_test_bit(d->relbit, REL_Y)) ||
           input_test_bit(d->keybit, BTN_LEFT);
}

static void client_push(evdev_client_t *c, uint64_t us, uint16_t type, uint16_t code, int32_t value) {
    uint32_t next = (c->head + 1) % EVDEV_RING;
    if (next == c->tail) {
        /* overflow: drop the queue and tell the reader (Linux SYN_DROPPED) */
        c->tail = c->head;
        c->ring[c->head].us = us; c->ring[c->head].type = EV_SYN;
        c->ring[c->head].code = SYN_DROPPED; c->ring[c->head].value = 0;
        c->head = (c->head + 1) % EVDEV_RING;
        next = (c->head + 1) % EVDEV_RING;
    }
    c->ring[c->head].us = us; c->ring[c->head].type = type;
    c->ring[c->head].code = code; c->ring[c->head].value = value;
    c->head = next;
}

void input_event(input_dev_t *d, unsigned type, unsigned code, int32_t value) {
    if (!d || !d->present || type > EV_MAX) return;
    if (!input_test_bit(d->evbit, type)) return;
    uint64_t us = ktime_now_us();
    bool run_handlers = false;
    uint64_t f = ilock();
    switch (type) {
    case EV_KEY:
        if (code > KEY_MAX || !input_test_bit(d->keybit, code)) goto out;
        if (value) {
            if (input_test_bit(d->key, code)) value = 2;      /* autorepeat */
            else { value = 1; input_set_bit(d->key, code); }
        } else {
            if (!input_test_bit(d->key, code)) goto out;
            d->key[code / 64] &= ~(1ull << (code % 64));
        }
        break;
    case EV_REL:
        if (code > REL_MAX || !value || !input_test_bit(d->relbit, code)) goto out;
        d->rel_accum[code] += value;
        break;
    case EV_ABS:
        if (code > ABS_MAX || !input_test_bit(d->absbit, code)) goto out;
        if (d->abs[code].value == value) goto out;
        d->abs[code].value = value;
        if (code < 64) d->abs_changed |= 1ull << code;
        break;
    case EV_LED:
        if (code > LED_MAX) goto out;
        if (value) d->led[0] |= 1ull << code; else d->led[0] &= ~(1ull << code);
        break;
    case EV_SYN:
        if (code == SYN_REPORT) {
            if (!d->frame_dirty) goto out;
            d->frame_dirty = false;
            run_handlers = d->grab == NULL;
        }
        break;
    default: break;
    }
    if (type != EV_SYN) d->frame_dirty = true;
    for (evdev_client_t *c = clients; c; c = c->next) {
        if (c->dev != d || c->revoked) continue;
        if (d->grab && d->grab != c) continue;
        client_push(c, us, (uint16_t)type, (uint16_t)code, value);
    }
out:
    iunlock(f);
    if (run_handlers) {
        for (int i = 0; i < n_sync_handlers; i++) sync_handlers[i](d);
    }
    if (type == EV_SYN && code == SYN_REPORT) {
        for (int i = 0; i <= REL_MAX; i++) d->rel_accum[i] = 0;
        d->abs_changed = 0;
    }
}

/* ---- PS/2 scan code set 1 -> Linux KEY_* ---- */
static const uint16_t set1_e0_map[128] = {
    [0x10] = 165, [0x19] = 163, [0x1C] = 96,  [0x1D] = 97,  [0x20] = 113,
    [0x21] = 140, [0x22] = 164, [0x24] = 166, [0x2E] = 114, [0x30] = 115,
    [0x32] = 172, [0x35] = 98,  [0x37] = 99,  [0x38] = 100, [0x46] = 119,
    [0x47] = 102, [0x48] = 103, [0x49] = 104, [0x4B] = 105, [0x4D] = 106,
    [0x4F] = 107, [0x50] = 108, [0x51] = 109, [0x52] = 110, [0x53] = 111,
    [0x5B] = 125, [0x5C] = 126, [0x5D] = 127, [0x5E] = 116, [0x5F] = 142,
    [0x63] = 143, [0x65] = 217, [0x66] = 156, [0x67] = 173, [0x68] = 128,
    [0x69] = 159, [0x6A] = 158, [0x6B] = 157, [0x6C] = 155, [0x6D] = 226,
};

void input_kbd_set1_key(input_dev_t *d, bool e0, uint8_t code, bool down) {
    if (!d) return;
    code &= 0x7F;
    unsigned key;
    if (e0) {
        if (code == 0x2A || code == 0x36) return;   /* fake shifts around nav keys */
        key = set1_e0_map[code];
    } else {
        key = code;                                 /* 0x01..0x58 == KEY_ESC..KEY_F12 */
        if (code == 0x00 || code > 0x58) {
            if (code == 0x70) key = 93;             /* KATAKANAHIRAGANA */
            else if (code == 0x73) key = 89;        /* RO */
            else if (code == 0x79) key = 92;        /* HENKAN */
            else if (code == 0x7B) key = 94;        /* MUHENKAN */
            else if (code == 0x7D) key = 124;       /* YEN */
            else key = 0;
        }
    }
    if (!key) return;
    input_event(d, EV_KEY, key, down ? 1 : 0);
    input_sync(d);
}

void input_kbd_set1_byte(input_dev_t *d, uint8_t b) {
    if (!d) return;
    if (d->set1_skip) {                 /* rest of the E1 1D 45 E1 9D C5 Pause sequence */
        d->set1_skip--;
        if (d->set1_skip == 0) {
            input_event(d, EV_KEY, 119, 1); input_sync(d);
            input_event(d, EV_KEY, 119, 0); input_sync(d);
        }
        return;
    }
    if (b == 0xE0) { d->set1_e0 = 1; return; }
    if (b == 0xE1) { d->set1_skip = 5; d->set1_e0 = 0; return; }
    if (b == 0x00 || b == 0xFA || b == 0xFE || b == 0xEE || b == 0xFF) {   /* acks, errors */
        d->set1_e0 = 0;
        return;
    }
    bool e0 = d->set1_e0;
    d->set1_e0 = 0;
    input_kbd_set1_key(d, e0, b & 0x7F, (b & 0x80) == 0);
}

void input_setup_keyboard(input_dev_t *d) {
    for (unsigned k = 1; k <= 88; k++) input_set_capability(d, EV_KEY, k);
    for (unsigned i = 0; i < 128; i++) if (set1_e0_map[i]) input_set_capability(d, EV_KEY, set1_e0_map[i]);
    static const uint16_t extra[] = { 89, 92, 93, 94, 119, 124 };
    for (unsigned i = 0; i < sizeof(extra) / sizeof(extra[0]); i++) input_set_capability(d, EV_KEY, extra[i]);
    input_set_capability(d, EV_LED, 0); input_set_capability(d, EV_LED, 1); input_set_capability(d, EV_LED, 2);
    input_set_bit(d->evbit, EV_REP);
}

/* ---- evdev ---- */
int evdev_parse_path(const char *path) {
    const char *pre = "/dev/input/event";
    int i = 0;
    while (pre[i] && path[i] == pre[i]) i++;
    if (pre[i]) return -1;
    if (path[i] < '0' || path[i] > '9') return -1;
    int n = 0;
    for (; path[i] >= '0' && path[i] <= '9'; i++) n = n * 10 + (path[i] - '0');
    if (path[i] || n >= INPUT_MAX_DEVICES) return -1;
    return (devs[n] && devs[n]->present) ? n : -1;
}

int input_list_minors(int *out, int max) {
    int n = 0;
    for (int i = 0; i < INPUT_MAX_DEVICES && n < max; i++)
        if (devs[i] && devs[i]->present) out[n++] = i;
    return n;
}

long evdev_open(int minor, void **handle) {
    if (minor < 0 || minor >= INPUT_MAX_DEVICES) return -LX_ENODEV;
    evdev_client_t *c = (evdev_client_t *)kalloc(sizeof(evdev_client_t));
    if (!c) return -LX_ENOMEM;
    memset(c, 0, sizeof(*c));
    uint64_t f = ilock();
    input_dev_t *d = devs[minor];
    if (!d || !d->present) { iunlock(f); kfree(c); return -LX_ENODEV; }
    c->dev = d;
    c->next = clients;
    clients = c;
    dev_clients[minor]++;
    iunlock(f);
    *handle = c;
    return 0;
}

void evdev_release(void *handle) {
    evdev_client_t *c = (evdev_client_t *)handle;
    if (!c) return;
    uint64_t f = ilock();
    for (evdev_client_t **pp = &clients; *pp; pp = &(*pp)->next)
        if (*pp == c) { *pp = c->next; break; }
    if (c->dev) {
        if (c->dev->grab == c) c->dev->grab = NULL;
        if (c->dev->minor >= 0 && c->dev->minor < INPUT_MAX_DEVICES && devs[c->dev->minor] == c->dev)
            dev_clients[c->dev->minor]--;
    }
    iunlock(f);
    kfree(c);
}

static uint64_t real_offset_us(void) {
    static uint64_t off; static bool valid;
    if (!valid) { off = rtc_unix_epoch() * 1000000ULL - ktime_now_us(); valid = true; }
    return off;
}

long evdev_read(void *handle, void *ubuf, size_t n, bool nonblock) {
    evdev_client_t *c = (evdev_client_t *)handle;
    if (!c) return -LX_EINVAL;
    if (n < sizeof(struct lx_input_event)) return -LX_EINVAL;
    size_t want = n / sizeof(struct lx_input_event);
    uint64_t off = c->monotonic ? 0 : real_offset_us();
    for (;;) {
        struct lx_input_event kev[32];
        size_t got = 0;
        input_pump();
        uint64_t f = ilock();
        while (got < want && got < 32 && c->tail != c->head) {
            kev[got].sec = (c->ring[c->tail].us + off) / 1000000ULL;
            kev[got].usec = (c->ring[c->tail].us + off) % 1000000ULL;
            kev[got].type = c->ring[c->tail].type;
            kev[got].code = c->ring[c->tail].code;
            kev[got].value = c->ring[c->tail].value;
            c->tail = (c->tail + 1) % EVDEV_RING;
            got++;
        }
        bool gone = c->revoked || !c->dev->present;
        iunlock(f);
        if (got) {
            if (copy_to_user(ubuf, kev, got * sizeof(kev[0]))) return -LX_EFAULT;
            return (long)(got * sizeof(kev[0]));
        }
        if (gone) return -LX_ENODEV;
        if (nonblock) return -LX_EAGAIN;
        process_t *p = get_current_process();
        if (p && proc_kill_pending(p)) return -LX_EINTR;
        thread_sleep(1);
    }
}

long evdev_write(void *handle, const void *ubuf, size_t n) {
    evdev_client_t *c = (evdev_client_t *)handle;
    if (!c || c->revoked || !c->dev->present) return -LX_ENODEV;
    if (n < sizeof(struct lx_input_event)) return -LX_EINVAL;
    size_t cnt = n / sizeof(struct lx_input_event);
    for (size_t i = 0; i < cnt; i++) {
        struct lx_input_event ev;
        if (copy_from_user(&ev, (const uint8_t *)ubuf + i * sizeof(ev), sizeof(ev))) return -LX_EFAULT;
        /* like Linux: only LED/SND/REP style feedback events from userspace;
         * key/rel injection belongs to uinput */
        if (ev.type == EV_LED || ev.type == EV_SYN) input_event(c->dev, ev.type, ev.code, ev.value);
    }
    return (long)(cnt * sizeof(struct lx_input_event));
}

bool evdev_poll_ready(void *handle, bool *hup) {
    evdev_client_t *c = (evdev_client_t *)handle;
    if (!c) return false;
    input_pump();
    uint64_t f = ilock();
    bool ready = c->tail != c->head;
    bool gone = c->revoked || !c->dev->present;
    iunlock(f);
    if (hup) *hup = gone;
    return ready || gone;
}

static long put_bits(void *arg, size_t size, const void *bits, size_t bytes) {
    uint8_t buf[128];
    memset(buf, 0, sizeof(buf));
    if (bytes > sizeof(buf)) bytes = sizeof(buf);
    if (bits) memcpy(buf, bits, bytes);
    size_t n = size < bytes ? size : bytes;
    if (n && copy_to_user(arg, buf, n)) return -LX_EFAULT;
    return (long)n;
}

static long put_str(void *arg, size_t size, const char *s) {
    size_t l = 0;
    while (s[l]) l++;
    l++;                                        /* include NUL */
    size_t n = size < l ? size : l;
    if (n && copy_to_user(arg, s, n)) return -LX_EFAULT;
    return (long)n;
}

long evdev_ioctl(void *handle, unsigned long req, void *arg) {
    evdev_client_t *c = (evdev_client_t *)handle;
    if (!c) return -LX_EINVAL;
    input_dev_t *d = c->dev;
    unsigned type = (req >> 8) & 0xFF, nr = req & 0xFF;
    size_t size = (req >> 16) & 0x3FFF;
    unsigned dir = (unsigned)(req >> 30) & 3;   /* 2 = read (to user), 1 = write */
    if (type != 'E') return -LX_ENOTTY;
    if (c->revoked || !d->present) return -LX_ENODEV;
    switch (nr) {
    case 0x01: { int v = 0x010001; return copy_to_user(arg, &v, sizeof(v)) ? -LX_EFAULT : 0; }
    case 0x02: {
        uint16_t id[4] = { d->bustype, d->vendor, d->product, d->version };
        return copy_to_user(arg, id, sizeof(id)) ? -LX_EFAULT : 0;
    }
    case 0x03:
        if (dir == 2) { uint32_t rep[2] = { 250, 33 }; return copy_to_user(arg, rep, sizeof(rep)) ? -LX_EFAULT : 0; }
        return 0;                               /* EVIOCSREP: accepted */
    case 0x06: return put_str(arg, size, d->name);
    case 0x07: return d->phys[0] ? put_str(arg, size, d->phys) : -LX_ENOENT;
    case 0x08: return -LX_ENOENT;               /* no unique id */
    case 0x09: return put_bits(arg, size, d->propbit, sizeof(d->propbit));
    case 0x18: { uint64_t k[(KEY_MAX + 64) / 64]; uint64_t f = ilock(); memcpy(k, d->key, sizeof(k)); iunlock(f);
                 return put_bits(arg, size, k, sizeof(k)); }
    case 0x19: return put_bits(arg, size, d->led, sizeof(d->led));
    case 0x1a: case 0x1b: return put_bits(arg, size, NULL, 8);
    case 0x84: { int v = 0; return copy_to_user(arg, &v, sizeof(v)) ? -LX_EFAULT : 0; }
    case 0x90: {                                 /* EVIOCGRAB: arg is the value */
        uint64_t f = ilock();
        long r = 0;
        if (arg) { if (d->grab && d->grab != c) r = -LX_EBUSY; else d->grab = c; }
        else { if (d->grab == c) d->grab = NULL; else r = -LX_EINVAL; }
        iunlock(f);
        return r;
    }
    case 0x91: c->revoked = true; return 0;     /* EVIOCREVOKE */
    case 0xa0: {                                 /* EVIOCSCLOCKID */
        int id;
        if (copy_from_user(&id, arg, sizeof(id))) return -LX_EFAULT;
        if (id == 0) c->monotonic = false;
        else if (id == 1 || id == 7) c->monotonic = true;
        else return -LX_EINVAL;
        return 0;
    }
    default: break;
    }
    if (nr >= 0x20 && nr < 0x40 && dir == 2) {   /* EVIOCGBIT(ev, len) */
        switch (nr - 0x20) {
        case 0:      return put_bits(arg, size, d->evbit, sizeof(d->evbit));
        case EV_KEY: return put_bits(arg, size, d->keybit, sizeof(d->keybit));
        case EV_REL: return put_bits(arg, size, d->relbit, sizeof(d->relbit));
        case EV_ABS: return put_bits(arg, size, d->absbit, sizeof(d->absbit));
        case EV_MSC: return put_bits(arg, size, d->mscbit, sizeof(d->mscbit));
        case EV_LED: return put_bits(arg, size, d->ledbit, sizeof(d->ledbit));
        case 0x15:   return put_bits(arg, size, NULL, 16);      /* EV_FF */
        default:     return put_bits(arg, size, NULL, 8);
        }
    }
    if (nr >= 0x40 && nr < 0x80 && dir == 2) {   /* EVIOCGABS(abs) */
        struct input_absinfo a;
        uint64_t f = ilock(); a = d->abs[nr - 0x40]; iunlock(f);
        size_t n = size < sizeof(a) ? size : sizeof(a);
        return copy_to_user(arg, &a, n) ? -LX_EFAULT : 0;
    }
    if (nr >= 0xc0 && nr < 0x100 && dir == 1) {  /* EVIOCSABS(abs) */
        struct input_absinfo a;
        if (copy_from_user(&a, arg, sizeof(a))) return -LX_EFAULT;
        uint64_t f = ilock(); d->abs[nr - 0xc0] = a; iunlock(f);
        return 0;
    }
    return -LX_EINVAL;
}

int evdev_minor(void *handle) {
    evdev_client_t *c = (evdev_client_t *)handle;
    return (c && c->dev) ? c->dev->minor : 0;
}

/* ======================================================================
 * /dev/uinput - Linux uinput ABI (linux/uinput.h, version 5).
 *
 * One uinput_t per open file.  Capabilities are collected with UI_SET_*BIT,
 * identity with UI_DEV_SETUP (or the legacy struct uinput_user_dev write),
 * then UI_DEV_CREATE registers an input_dev_t that behaves like any other:
 * it shows up as /dev/input/eventN, pointer frames go to the desktop pointer
 * (mouse_input_sync) and keyboard keys are injected into keyboard.c as
 * set-1 scancodes so they reach the shell / desktop.  UI_DEV_DESTROY, close
 * and process exit (fd teardown) release held keys/buttons and remove it.
 * ====================================================================== */
#define UINPUT_MAX_NAME_SIZE 80
#define UINPUT_VERSION 5
#define LX_EPERM 1

struct lx_input_id { uint16_t bustype, vendor, product, version; };
struct lx_uinput_setup { struct lx_input_id id; char name[UINPUT_MAX_NAME_SIZE]; uint32_t ff_effects_max; };
struct lx_uinput_abs_setup { uint16_t code; uint16_t pad; struct input_absinfo absinfo; };
struct lx_uinput_user_dev {
    char name[UINPUT_MAX_NAME_SIZE];
    struct lx_input_id id;
    uint32_t ff_effects_max;
    int32_t absmax[64], absmin[64], absfuzz[64], absflat[64];
};
_Static_assert(sizeof(struct lx_uinput_setup) == 92, "uinput_setup");
_Static_assert(sizeof(struct lx_uinput_abs_setup) == 28, "uinput_abs_setup");
_Static_assert(sizeof(struct lx_uinput_user_dev) == 1116, "uinput_user_dev");

typedef struct uinput {
    input_dev_t *dev;       /* allocated at open, registered at UI_DEV_CREATE */
    bool created;
    bool configured;        /* name/id set via UI_DEV_SETUP or legacy write */
    uint32_t owner_pid;
} uinput_t;

/* Linux KEY_* -> PS/2 set-1 (bit 0x100 = E0 prefix); 0 = none. */
static uint16_t key_to_set1(unsigned key) {
    if (key >= 1 && key <= 88) return (uint16_t)key;
    switch (key) {
    case 89: return 0x73; case 92: return 0x79; case 93: return 0x70;
    case 94: return 0x7B; case 124: return 0x7D;
    default: break;
    }
    for (unsigned i = 0; i < 128; i++)
        if (set1_e0_map[i] == key) return (uint16_t)(0x100 | i);
    return 0;
}

static void uinput_kbd_inject(unsigned key, bool down) {
    extern void keyboard_inject_scancode(uint8_t b);
    uint16_t s = key_to_set1(key);
    if (!s || key == 119) return;               /* Pause has no break code */
    if (s & 0x100) keyboard_inject_scancode(0xE0);
    keyboard_inject_scancode((uint8_t)((s & 0x7F) | (down ? 0 : 0x80)));
}

/* Emit one event from a uinput device, mirroring keyboard keys into
 * keyboard.c (the kernel "keyboard handler") unless an evdev client grabbed
 * the device. */
static void uinput_emit(input_dev_t *d, unsigned type, unsigned code, int32_t value) {
    if (type == EV_KEY && code < BTN_MISC && code <= KEY_MAX) {
        bool was = input_test_bit(d->key, code);
        input_event(d, type, code, value);
        bool now = input_test_bit(d->key, code);
        if (!d->grab && d->present && (was != now || (now && value == 2)))
            uinput_kbd_inject(code, now);
        return;
    }
    input_event(d, type, code, value);
}

long uinput_open(void **handle) {
    uinput_t *u = (uinput_t *)kalloc(sizeof(uinput_t));
    if (!u) return -LX_ENOMEM;
    memset(u, 0, sizeof(*u));
    u->dev = input_allocate("uinput device", BUS_VIRTUAL, 0, 0);
    if (!u->dev) { kfree(u); return -LX_ENOMEM; }
    u->dev->owner = u;
    process_t *p = get_current_process();
    u->owner_pid = p ? p->pid : 0;
    *handle = u;
    return 0;
}

static void uinput_destroy(uinput_t *u) {
    if (!u->created) return;
    input_dev_t *d = u->dev;
    /* release held keys/buttons through the normal path so keyboard.c sees
     * the break codes and the pointer handler sees buttons up */
    bool any = false;
    for (unsigned k = 0; k <= KEY_MAX; k++)
        if (input_test_bit(d->key, k)) { uinput_emit(d, EV_KEY, k, 0); any = true; }
    if (any) input_sync(d);
    int mx = 0, my = 0;
    extern void mouse_get_position(int *x, int *y);
    mouse_get_position(&mx, &my);
    serial_printf("uinput: destroy '%s' (event%d) pointer=(%d,%d)\n", d->name, d->minor, mx, my);
    input_unregister(d);
    /* the core frees it when the slot is reused with no clients left */
    d->owner = NULL;
    u->created = false;
    u->dev = NULL;
}

void uinput_release(void *handle) {
    uinput_t *u = (uinput_t *)handle;
    if (!u) return;
    if (u->created) uinput_destroy(u);
    else if (u->dev) kfree(u->dev);
    kfree(u);
}

static void uinput_set_name(input_dev_t *d, const char *n) {
    char tmp[UINPUT_MAX_NAME_SIZE + 1];
    memcpy(tmp, n, UINPUT_MAX_NAME_SIZE);
    tmp[UINPUT_MAX_NAME_SIZE] = 0;
    kcopy(d->name, tmp, sizeof(d->name));
}

long uinput_write(void *handle, const void *ubuf, size_t n) {
    uinput_t *u = (uinput_t *)handle;
    if (!u) return -LX_EINVAL;
    if (!u->created) {
        /* legacy setup: struct uinput_user_dev */
        if (n != sizeof(struct lx_uinput_user_dev)) return -LX_EINVAL;
        struct lx_uinput_user_dev *ud = (struct lx_uinput_user_dev *)kalloc(sizeof(*ud));
        if (!ud) return -LX_ENOMEM;
        if (copy_from_user(ud, ubuf, sizeof(*ud))) { kfree(ud); return -LX_EFAULT; }
        if (!ud->name[0]) { kfree(ud); return -LX_EINVAL; }
        input_dev_t *d = u->dev;
        uinput_set_name(d, ud->name);
        d->bustype = ud->id.bustype; d->vendor = ud->id.vendor;
        d->product = ud->id.product; d->version = ud->id.version;
        for (int i = 0; i <= ABS_MAX && i < 64; i++) {
            d->abs[i].minimum = ud->absmin[i]; d->abs[i].maximum = ud->absmax[i];
            d->abs[i].fuzz = ud->absfuzz[i];   d->abs[i].flat = ud->absflat[i];
        }
        u->configured = true;
        kfree(ud);
        return (long)n;
    }
    if (n < sizeof(struct lx_input_event)) return -LX_EINVAL;
    size_t cnt = n / sizeof(struct lx_input_event);
    for (size_t i = 0; i < cnt; i++) {
        struct lx_input_event ev;
        if (copy_from_user(&ev, (const uint8_t *)ubuf + i * sizeof(ev), sizeof(ev))) return i ? (long)(i * sizeof(ev)) : -LX_EFAULT;
        if (!u->dev || !u->dev->present) return -LX_ENODEV;
        uinput_emit(u->dev, ev.type, ev.code, ev.value);
    }
    return (long)(cnt * sizeof(struct lx_input_event));
}

long uinput_read(void *handle, void *ubuf, size_t n, bool nonblock) {
    (void)ubuf; (void)n; (void)nonblock;
    uinput_t *u = (uinput_t *)handle;
    if (!u || !u->created) return -LX_EINVAL;
    return -LX_EAGAIN;                          /* no force-feedback requests */
}

long uinput_ioctl(void *handle, unsigned long req, void *arg) {
    uinput_t *u = (uinput_t *)handle;
    if (!u) return -LX_EINVAL;
    unsigned type = (req >> 8) & 0xFF, nr = req & 0xFF;
    size_t size = (req >> 16) & 0x3FFF;
    if (type != 'U') return -LX_ENOTTY;
    input_dev_t *d = u->dev;
    unsigned long v = (unsigned long)arg;
    if (nr >= 100 && nr <= 110 && u->created) return -LX_EINVAL;   /* bits only before CREATE */
    switch (nr) {
    case 1: {                                   /* UI_DEV_CREATE */
        if (u->created) return -LX_EINVAL;
        if (!u->configured) return -LX_EINVAL;
        if (input_register(d) != 0) return -LX_ENOMEM;
        u->created = true;
        int mx = 0, my = 0;
        extern void mouse_get_position(int *x, int *y);
        mouse_get_position(&mx, &my);
        serial_printf("uinput: created '%s' as event%d (pid %u) pointer=(%d,%d)\n", d->name, d->minor, u->owner_pid, mx, my);
        return 0;
    }
    case 2:                                     /* UI_DEV_DESTROY */
        if (!u->created) return -LX_EINVAL;
        uinput_destroy(u);
        /* the file may be configured again for a new device */
        u->dev = input_allocate("uinput device", BUS_VIRTUAL, 0, 0);
        if (!u->dev) return -LX_ENOMEM;
        u->dev->owner = u;
        u->configured = false;
        return 0;
    case 3: {                                   /* UI_DEV_SETUP */
        if (u->created) return -LX_EINVAL;
        struct lx_uinput_setup s;
        if (copy_from_user(&s, arg, sizeof(s))) return -LX_EFAULT;
        if (!s.name[0]) return -LX_EINVAL;
        uinput_set_name(d, s.name);
        d->bustype = s.id.bustype; d->vendor = s.id.vendor;
        d->product = s.id.product; d->version = s.id.version;
        u->configured = true;
        return 0;
    }
    case 4: {                                   /* UI_ABS_SETUP */
        if (u->created) return -LX_EINVAL;
        struct lx_uinput_abs_setup s;
        if (copy_from_user(&s, arg, sizeof(s))) return -LX_EFAULT;
        if (s.code > ABS_MAX) return -LX_EINVAL;
        d->abs[s.code] = s.absinfo;
        input_set_capability(d, EV_ABS, s.code);
        return 0;
    }
    case 44: {                                  /* UI_GET_SYSNAME(len) */
        if (!u->created) return -LX_ENOENT;
        char nm[16] = "input";
        int m = d->minor;
        if (m >= 10) { nm[5] = (char)('0' + m / 10); nm[6] = (char)('0' + m % 10); nm[7] = 0; }
        else { nm[5] = (char)('0' + m); nm[6] = 0; }
        return put_str(arg, size, nm);
    }
    case 45: {                                  /* UI_GET_VERSION */
        unsigned int ver = UINPUT_VERSION;
        return copy_to_user(arg, &ver, sizeof(ver)) ? -LX_EFAULT : 0;
    }
    case 100:                                   /* UI_SET_EVBIT */
        if (v > EV_MAX) return -LX_EINVAL;
        input_set_bit(d->evbit, (unsigned)v);
        return 0;
    case 101: if (v > KEY_MAX) return -LX_EINVAL; input_set_capability(d, EV_KEY, (unsigned)v); return 0;
    case 102: if (v > REL_MAX) return -LX_EINVAL; input_set_capability(d, EV_REL, (unsigned)v); return 0;
    case 103: if (v > ABS_MAX) return -LX_EINVAL; input_set_capability(d, EV_ABS, (unsigned)v); return 0;
    case 104: if (v > MSC_MAX) return -LX_EINVAL; input_set_capability(d, EV_MSC, (unsigned)v); return 0;
    case 105: if (v > LED_MAX) return -LX_EINVAL; input_set_capability(d, EV_LED, (unsigned)v); return 0;
    case 106: case 107: case 109: return 0;     /* SND / FF / SW bits: accepted, unused */
    case 108: {                                 /* UI_SET_PHYS(char *) */
        char ph[sizeof(d->phys)];
        size_t i = 0;
        for (; i + 1 < sizeof(ph); i++) {
            char ch;
            if (copy_from_user(&ch, (const char *)arg + i, 1)) return -LX_EFAULT;
            ph[i] = ch;
            if (!ch) break;
        }
        ph[i < sizeof(ph) ? i : sizeof(ph) - 1] = 0;
        kcopy(d->phys, ph, sizeof(d->phys));
        return 0;
    }
    case 110:                                   /* UI_SET_PROPBIT */
        if (v > INPUT_PROP_MAX) return -LX_EINVAL;
        input_set_bit(d->propbit, (unsigned)v);
        return 0;
    default:
        return -LX_EINVAL;
    }
}

/* ---- synthetic /dev and /dev/input directory listings ---- */
int input_dev_dir_list(const char *path, char names[][16], uint8_t *is_dir, int max) {
    int n = 0;
    if (kstrcmp_input(path, "/dev/input")) {
        for (int i = 0; i < INPUT_MAX_DEVICES && n < max; i++) {
            if (!devs[i] || !devs[i]->present) continue;
            char *s = names[n];
            kcopy(s, "event", 16);
            if (i >= 10) { s[5] = (char)('0' + i / 10); s[6] = (char)('0' + i % 10); s[7] = 0; }
            else { s[5] = (char)('0' + i); s[6] = 0; }
            is_dir[n++] = 0;
        }
        return n;
    }
    if (kstrcmp_input(path, "/dev")) {
        static const char *const fixed[] = { "console", "fb0", "input", "null", "tty",
                                             "tty0", "tty1", "tty2", "tty3", "tty4", "uinput", "wm" };
        for (unsigned i = 0; i < sizeof(fixed) / sizeof(fixed[0]) && n < max; i++) {
            kcopy(names[n], fixed[i], 16);
            is_dir[n++] = fixed[i][0] == 'i' && fixed[i][1] == 'n';
        }
        return n;
    }
    return -1;
}
