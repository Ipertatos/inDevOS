/*
 * input.h - kernel input core and Linux evdev (/dev/input/eventN).
 *
 * Sources (PS/2 keyboard and mouse, USB HID boot keyboards and mice, later
 * uinput) report Linux input events; the core keeps per-device state,
 * fans each event out to evdev clients and, on SYN_REPORT, to kernel
 * handlers (the desktop/console pointer handler lives in mouse.c).  Event
 * types, codes and the ioctl ABI follow <linux/input.h>.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_REL 0x02
#define EV_ABS 0x03
#define EV_MSC 0x04
#define EV_LED 0x11
#define EV_REP 0x14
#define EV_MAX 0x1f
#define SYN_REPORT  0
#define SYN_DROPPED 3
#define REL_X 0x00
#define REL_Y 0x01
#define REL_HWHEEL 0x06
#define REL_WHEEL 0x08
#define REL_WHEEL_HI_RES 0x0b
#define REL_MAX 0x0f
#define ABS_MAX 0x3f
#define KEY_MAX 0x2ff
#define LED_MAX 0x0f
#define MSC_MAX 0x07
#define BTN_LEFT   0x110
#define BTN_RIGHT  0x111
#define BTN_MIDDLE 0x112
#define BTN_SIDE   0x113
#define BTN_EXTRA  0x114
#ifndef BTN_TOUCH
#define BTN_TOUCH  0x14a
#endif
#ifndef ABS_X
#define ABS_X 0x00
#define ABS_Y 0x01
#endif
#define INPUT_PROP_MAX 0x1f

#define BUS_I8042 0x11
#define BUS_USB   0x03
#define BUS_VIRTUAL 0x06
#ifndef BTN_MISC
#define BTN_MISC 0x100
#endif

#define INPUT_MAX_DEVICES 16

struct input_absinfo { int32_t value, minimum, maximum, fuzz, flat, resolution; };

typedef struct input_dev {
    int      minor;                 /* N in /dev/input/eventN, -1 = unregistered */
    bool     present;
    char     name[64];
    char     phys[32];
    uint16_t bustype, vendor, product, version;
    /* capability bitmaps, sized in whole 64-bit longs like Linux */
    uint64_t evbit[1], keybit[(KEY_MAX + 64) / 64], relbit[1], absbit[1];
    uint64_t mscbit[1], ledbit[1], propbit[1];
    struct input_absinfo abs[ABS_MAX + 1];
    /* state */
    uint64_t key[(KEY_MAX + 64) / 64];          /* keys/buttons currently down */
    uint64_t led[1];
    int32_t  rel_accum[REL_MAX + 1];            /* since the last SYN_REPORT */
    bool     frame_dirty;
    uint64_t abs_changed;                       /* ABS codes (<64) changed since last SYN_REPORT */
    void    *grab;                              /* evdev client with EVIOCGRAB */
    uint8_t  set1_e0, set1_skip;                /* PS/2 set-1 byte decoder */
    void    *owner;                             /* uinput: creating file */
} input_dev_t;

/* Kernel handler: called after each SYN_REPORT (not while grabbed). */
typedef void (*input_sync_fn)(input_dev_t *dev);

static inline void input_set_bit(uint64_t *bm, unsigned n) { bm[n / 64] |= 1ull << (n % 64); }
static inline bool input_test_bit(const uint64_t *bm, unsigned n) { return (bm[n / 64] >> (n % 64)) & 1; }

void input_init(void);
input_dev_t *input_allocate(const char *name, uint16_t bus, uint16_t vendor, uint16_t product);
void input_set_capability(input_dev_t *d, unsigned type, unsigned code);
int  input_register(input_dev_t *d);
void input_unregister(input_dev_t *d);   /* releases held keys, wakes readers with ENODEV */
void input_event(input_dev_t *d, unsigned type, unsigned code, int32_t value);
static inline void input_sync(input_dev_t *d) { input_event(d, EV_SYN, SYN_REPORT, 0); }
void input_add_sync_handler(input_sync_fn fn);
bool input_is_pointer(const input_dev_t *d);

/* Feed one PS/2 scan-code-set-1 byte (with E0/E1 prefixes) to a keyboard dev. */
void input_kbd_set1_byte(input_dev_t *d, uint8_t b);
/* Same for an already split make/break code (USB keyboards). */
void input_kbd_set1_key(input_dev_t *d, bool e0, uint8_t code, bool down);
/* Capabilities of a standard 105-key keyboard. */
void input_setup_keyboard(input_dev_t *d);

/* Built-in devices (registered by keyboard.c / mouse.c) */
extern input_dev_t *input_ps2_kbd, *input_ps2_mouse;

/* ---- evdev file interface (linux_syscall.c) ---- */
#define FD_TYPE_DEV_INPUT 10
int  evdev_parse_path(const char *path);                   /* minor or -1 */
long evdev_open(int minor, void **handle);
long evdev_read(void *handle, void *ubuf, size_t n, bool nonblock);
long evdev_write(void *handle, const void *ubuf, size_t n);
long evdev_ioctl(void *handle, unsigned long req, void *arg);
bool evdev_poll_ready(void *handle, bool *hup);
void evdev_release(void *handle);
int  input_list_minors(int *out, int max);                 /* for readdir/stat */
int  evdev_minor(void *handle);                            /* N of the open eventN */

/* ---- /dev/uinput (Linux uinput ABI v5) ---- */
#define FD_TYPE_DEV_UINPUT 11
long uinput_open(void **handle);
long uinput_read(void *handle, void *ubuf, size_t n, bool nonblock);
long uinput_write(void *handle, const void *ubuf, size_t n);
long uinput_ioctl(void *handle, unsigned long req, void *arg);
void uinput_release(void *handle);   /* destroys the device, releasing held keys */

/* Synthetic directory entries for "/dev" and "/dev/input": fills names
 * (and is_dir flags), returns the count, or -1 for other paths. */
int  input_dev_dir_list(const char *path, char names[][16], uint8_t *is_dir, int max);
