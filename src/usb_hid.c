/**
 * @file usb_hid.c
 * @brief USB HID boot-protocol keyboard and mouse over xHCI interrupt EPs.
 *
 * Keyboard reports are turned into PS/2 set-1 make/break codes and fed to
 * the PS/2 keyboard path (keyboard_inject_scancode), so layouts, Ctrl-C,
 * arrows, history, SDL key state and virtual consoles behave exactly like a
 * PS/2 keyboard. Typematic repeat is done here (500 ms delay, ~30/s).
 * Mouse reports go to mouse_feed_usb_relative(), like the old stub intended.
 */
#include "usb_core.h"
#include "mem.h"
#include "string.h"
#include "serial.h"
#include "stdio.h"
#include "ktime.h"

extern void *dma_alloc_pages(uint32_t bytes, uint64_t *phys);
extern void dma_free_pages(void *p);
extern void keyboard_inject_scancode(uint8_t b);
extern void mouse_feed_usb_relative(int8_t dx, int8_t dy, uint8_t buttons);

#define E0 0x100
/* HID usage (keyboard page) -> PS/2 set 1, E0 = extended prefix. */
static const uint16_t hid_to_set1[0x68] = {
    [0x04] = 0x1E, [0x05] = 0x30, [0x06] = 0x2E, [0x07] = 0x20, [0x08] = 0x12,
    [0x09] = 0x21, [0x0A] = 0x22, [0x0B] = 0x23, [0x0C] = 0x17, [0x0D] = 0x24,
    [0x0E] = 0x25, [0x0F] = 0x26, [0x10] = 0x32, [0x11] = 0x31, [0x12] = 0x18,
    [0x13] = 0x19, [0x14] = 0x10, [0x15] = 0x13, [0x16] = 0x1F, [0x17] = 0x14,
    [0x18] = 0x16, [0x19] = 0x2F, [0x1A] = 0x11, [0x1B] = 0x2D, [0x1C] = 0x15,
    [0x1D] = 0x2C,
    [0x1E] = 0x02, [0x1F] = 0x03, [0x20] = 0x04, [0x21] = 0x05, [0x22] = 0x06,
    [0x23] = 0x07, [0x24] = 0x08, [0x25] = 0x09, [0x26] = 0x0A, [0x27] = 0x0B,
    [0x28] = 0x1C, [0x29] = 0x01, [0x2A] = 0x0E, [0x2B] = 0x0F, [0x2C] = 0x39,
    [0x2D] = 0x0C, [0x2E] = 0x0D, [0x2F] = 0x1A, [0x30] = 0x1B, [0x31] = 0x2B,
    [0x32] = 0x2B, [0x33] = 0x27, [0x34] = 0x28, [0x35] = 0x29, [0x36] = 0x33,
    [0x37] = 0x34, [0x38] = 0x35, [0x39] = 0x3A,
    [0x3A] = 0x3B, [0x3B] = 0x3C, [0x3C] = 0x3D, [0x3D] = 0x3E, [0x3E] = 0x3F,
    [0x3F] = 0x40, [0x40] = 0x41, [0x41] = 0x42, [0x42] = 0x43, [0x43] = 0x44,
    [0x44] = 0x57, [0x45] = 0x58,
    [0x47] = 0x46,
    [0x49] = E0 | 0x52, [0x4A] = E0 | 0x47, [0x4B] = E0 | 0x49, [0x4C] = E0 | 0x53,
    [0x4D] = E0 | 0x4F, [0x4E] = E0 | 0x51, [0x4F] = E0 | 0x4D, [0x50] = E0 | 0x4B,
    [0x51] = E0 | 0x50, [0x52] = E0 | 0x48,
    [0x53] = 0x45, [0x54] = E0 | 0x35, [0x55] = 0x37, [0x56] = 0x4A, [0x57] = 0x4E,
    [0x58] = E0 | 0x1C, [0x59] = 0x4F, [0x5A] = 0x50, [0x5B] = 0x51, [0x5C] = 0x4B,
    [0x5D] = 0x4C, [0x5E] = 0x4D, [0x5F] = 0x47, [0x60] = 0x48, [0x61] = 0x49,
    [0x62] = 0x52, [0x63] = 0x53, [0x64] = 0x56,
};
/* modifier bit -> set 1 */
static const uint16_t mod_to_set1[8] = {
    0x1D, 0x2A, 0x38, E0 | 0x5B, E0 | 0x1D, 0x36, E0 | 0x38, E0 | 0x5C
};

typedef struct {
    usb_dev_t *dev;
    int drv;
    uint8_t iface;
    uint8_t ep;
    uint16_t len;
    uint8_t *buf;
    bool queued;
    uint8_t prev[8];
    uint8_t rep_key;
    uint64_t rep_at;
    uint32_t reports;
} usb_hid_t;

static void send_code(uint16_t c, bool down) {
    if (!c) return;
    if (c & E0) keyboard_inject_scancode(0xE0);
    keyboard_inject_scancode((uint8_t)((c & 0x7F) | (down ? 0 : 0x80)));
}

static bool has_key(const uint8_t *r, uint8_t k) {
    for (int i = 2; i < 8; i++) if (r[i] == k) return true;
    return false;
}

static void kbd_report(usb_hid_t *h, const uint8_t *r) {
    if (r[2] == 1) return;                      /* ErrorRollOver */
    uint8_t mchg = (uint8_t)(r[0] ^ h->prev[0]);
    for (int b = 0; b < 8; b++)
        if (mchg & (1u << b)) send_code(mod_to_set1[b], (r[0] >> b) & 1);
    for (int i = 2; i < 8; i++) {
        uint8_t k = h->prev[i];
        if (k >= 4 && k < 0x68 && !has_key(r, k)) {
            send_code(hid_to_set1[k], false);
            if (h->rep_key == k) h->rep_key = 0;
        }
    }
    for (int i = 2; i < 8; i++) {
        uint8_t k = r[i];
        if (k >= 4 && k < 0x68 && !has_key(h->prev, k)) {
            send_code(hid_to_set1[k], true);
            h->rep_key = k;
            h->rep_at = ktime_now_ms() + 500;
        }
    }
    memcpy(h->prev, r, 8);
}

int usb_hid_attach(usb_dev_t *d, usb_iface_info_t *ifc, int drv, void **data) {
    usb_ep_info_t *in = NULL;
    for (int e = 0; e < ifc->n_ep; e++)
        if (ifc->ep[e].attr == 3 && (ifc->ep[e].addr & 0x80)) { in = &ifc->ep[e]; break; }
    if (!in) return -1;
    usb_hid_t *h = (usb_hid_t *)kalloc(sizeof(usb_hid_t));
    if (!h) return -1;
    memset(h, 0, sizeof(*h));
    uint64_t ph;
    h->buf = (uint8_t *)dma_alloc_pages(4096, &ph);
    if (!h->buf) { kfree(h); return -1; }
    h->dev = d;
    h->drv = drv;
    h->iface = ifc->number;
    h->ep = in->addr;
    h->len = in->mps > 64 ? 64 : (in->mps ? in->mps : 8);
    /* Boot protocol (0) and idle 0 (report only on change). */
    usb_ctrl(d, 0x21, 0x0B, 0, ifc->number, 0, NULL);
    usb_ctrl(d, 0x21, 0x0A, 0, ifc->number, 0, NULL);
    if (xhci_configure_endpoint(d->slot, in->addr, 3, in->mps, in->interval, in->burst) != 0) {
        dma_free_pages(h->buf);
        kfree(h);
        return -1;
    }
    h->queued = xhci_int_queue(d->slot, h->ep, h->buf, h->len) == 0;
    printf("usb: dev %u: HID boot %s on interface %u (ep 0x%02x, %u bytes)\n",
           (unsigned)d->id, drv == USB_DRV_KBD ? "keyboard" : "mouse", ifc->number,
           in->addr, h->len);
    *data = h;
    return 0;
}

void usb_hid_detach(void *data) {
    usb_hid_t *h = (usb_hid_t *)data;
    if (!h) return;
    if (h->drv == USB_DRV_KBD) {                /* release anything held */
        uint8_t zero[8] = {0};
        kbd_report(h, zero);
    }
    dma_free_pages(h->buf);
    kfree(h);
}

void usb_hid_poll(void *data) {
    usb_hid_t *h = (usb_hid_t *)data;
    if (!h || !h->dev->present) return;
    if (h->queued) {
        uint32_t n = 0;
        int cc = 0;
        if (xhci_int_poll(h->dev->slot, h->ep, &n, &cc) == 1) {
            h->queued = false;
            if (cc == XHCI_CC_SUCCESS || cc == XHCI_CC_SHORT_PACKET) {
                h->reports++;
                if (h->reports == 1 || (h->reports % 500) == 0)
                    serial_printf("usb: dev %u %s report #%u (%u bytes: %02x %02x %02x %02x)\n",
                                  (unsigned)h->dev->id, h->drv == USB_DRV_KBD ? "keyboard" : "mouse",
                                  h->reports, n, h->buf[0], h->buf[1], h->buf[2], h->buf[3]);
                if (h->drv == USB_DRV_KBD && n >= 8) {
                    kbd_report(h, h->buf);
                } else if (h->drv == USB_DRV_MOUSE && n >= 3) {
                    mouse_feed_usb_relative((int8_t)h->buf[1], (int8_t)h->buf[2], h->buf[0] & 7);
                }
            }
        }
    }
    if (!h->queued)
        h->queued = xhci_int_queue(h->dev->slot, h->ep, h->buf, h->len) == 0;
    if (h->drv == USB_DRV_KBD && h->rep_key && ktime_now_ms() >= h->rep_at) {
        send_code(hid_to_set1[h->rep_key], true);
        h->rep_at = ktime_now_ms() + 33;
    }
}
