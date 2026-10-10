/**
 * @file usb_core.c
 * @brief USB enumeration, hubs and class binding (xHCI only).
 *
 * attach: Enable Slot -> Address Device (route string, root port, TT for
 * LS/FS devices behind a HS hub) -> device descriptor (EP0 max packet fixed
 * up for FS) -> configuration descriptor -> SET_CONFIGURATION -> bind class
 * drivers per interface (hub, HID boot keyboard/mouse, mass storage BOT).
 *
 * Hotplug: root ports via xHCI port status change events, hub ports via the
 * hub's status-change interrupt endpoint; both serviced from usb_poll().
 */
#include "usb_core.h"
#include "mem.h"
#include "string.h"
#include "serial.h"
#include "stdio.h"
#include "ktime.h"

#define USB_REQ_GET_STATUS     0
#define USB_REQ_CLEAR_FEATURE  1
#define USB_REQ_SET_FEATURE    3
#define USB_REQ_GET_DESCRIPTOR 6
#define USB_REQ_SET_CONFIG     9
#define USB_REQ_SET_HUB_DEPTH  12

#define HUB_PORT_CONNECTION   0
#define HUB_PORT_RESET        4
#define HUB_PORT_POWER        8
#define HUB_C_PORT_CONNECTION 16
#define HUB_C_PORT_ENABLE     17
#define HUB_C_PORT_SUSPEND    18
#define HUB_C_PORT_OVER       19
#define HUB_C_PORT_RESET      20
#define HUB_BH_PORT_RESET     28
#define HUB_C_BH_PORT_RESET   29
#define HUB_C_PORT_LINK_STATE 25
#define HUB_C_PORT_CONFIG_ERR 26

#define CTRL_TIMEOUT 2000

static usb_dev_t usb_devs[USB_MAX_DEVICES];
static bool usb_ready;

usb_dev_t *usb_get_device(int i) {
    return (i >= 0 && i < USB_MAX_DEVICES && usb_devs[i].present) ? &usb_devs[i] : NULL;
}

int usb_ctrl(usb_dev_t *d, uint8_t rt, uint8_t req, uint16_t val, uint16_t idx,
             uint16_t len, void *data) {
    if (!d || !d->slot) return -1;
    return xhci_control(d->slot, rt, req, val, idx, len, data, CTRL_TIMEOUT);
}

int usb_clear_halt(usb_dev_t *d, uint8_t epaddr) {
    xhci_reset_endpoint(d->slot, epaddr);
    return usb_ctrl(d, 0x02, USB_REQ_CLEAR_FEATURE, 0, epaddr, 0, NULL);
}

static const char *class_name(uint8_t c) {
    switch (c) {
    case 0x00: return "per-interface";
    case 0x01: return "audio";
    case 0x02: return "comm";
    case 0x03: return "HID";
    case 0x07: return "printer";
    case 0x08: return "mass-storage";
    case 0x09: return "hub";
    case 0x0A: return "CDC-data";
    case 0x0B: return "smart-card";
    case 0x0E: return "video";
    case 0xE0: return "wireless";
    case 0xEF: return "misc";
    case 0xFF: return "vendor";
    default: return "other";
    }
}

static void get_string(usb_dev_t *d, uint8_t index, char *out, uint32_t outsz) {
    out[0] = '\0';
    if (!index) return;
    uint8_t buf[128];
    int n = usb_ctrl(d, 0x80, USB_REQ_GET_DESCRIPTOR, (uint16_t)(0x0300 | index), 0x0409,
                     sizeof(buf), buf);
    if (n < 4 || buf[1] != 3) return;
    uint32_t len = buf[0] < (uint32_t)n ? buf[0] : (uint32_t)n;
    uint32_t o = 0;
    for (uint32_t i = 2; i + 1 < len && o + 1 < outsz; i += 2) {
        uint16_t ch = (uint16_t)(buf[i] | (buf[i + 1] << 8));
        out[o++] = (ch >= 0x20 && ch < 0x7F) ? (char)ch : '?';
    }
    out[o] = '\0';
    while (o > 0 && out[o - 1] == ' ') out[--o] = '\0';
}

static void parse_config(usb_dev_t *d, const uint8_t *cfg, uint32_t len) {
    d->n_iface = 0;
    usb_iface_info_t *cur = NULL;
    for (uint32_t o = 0; o + 2 <= len;) {
        uint8_t bl = cfg[o], ty = cfg[o + 1];
        if (bl < 2 || o + bl > len) break;
        if (ty == 4 && bl >= 9) {
            cur = NULL;
            if (cfg[o + 3] == 0 && d->n_iface < 8) {     /* alternate setting 0 only */
                cur = &d->iface[d->n_iface++];
                memset(cur, 0, sizeof(*cur));
                cur->number = cfg[o + 2];
                cur->cls = cfg[o + 5];
                cur->sub = cfg[o + 6];
                cur->proto = cfg[o + 7];
            }
        } else if (ty == 5 && bl >= 7 && cur && cur->n_ep < 4) {
            usb_ep_info_t *e = &cur->ep[cur->n_ep++];
            e->addr = cfg[o + 2];
            e->attr = cfg[o + 3] & 3;
            e->mps = (uint16_t)((cfg[o + 4] | (cfg[o + 5] << 8)) & 0x7FF);
            e->interval = cfg[o + 6];
            e->burst = 0;
        } else if (ty == 0x30 && bl >= 6 && cur && cur->n_ep) {
            cur->ep[cur->n_ep - 1].burst = cfg[o + 2];   /* SS endpoint companion */
        }
        o += bl;
    }
}

static usb_dev_t *dev_alloc(void) {
    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        if (!usb_devs[i].present) {
            memset(&usb_devs[i], 0, sizeof(usb_devs[i]));
            usb_devs[i].id = i;
            return &usb_devs[i];
        }
    }
    return NULL;
}

static void hub_attach(usb_dev_t *d, usb_iface_info_t *ifc);
static void usb_detach(usb_dev_t *d);
static void port_path(usb_dev_t *d, char *out, uint32_t sz);

static usb_dev_t *usb_attach(xhci_ctrl_t *hc, usb_dev_t *parent, uint8_t port, uint8_t speed) {
    usb_dev_t *d = dev_alloc();
    if (!d) {
        printf("usb: device table full\n");
        return NULL;
    }
    d->hc = hc;
    d->parent = parent;
    d->port = port;
    d->speed = speed;
    if (parent) {
        d->root_port = parent->root_port;
        d->depth = (uint8_t)(parent->depth + 1);
        d->route = parent->route | ((uint32_t)(port > 15 ? 15 : port) << (4u * parent->depth));
        if ((speed == USB_SPEED_LOW || speed == USB_SPEED_FULL) &&
            parent->speed == USB_SPEED_HIGH) {
            d->tt_slot = parent->slot->slot_id;
            d->tt_port = port;
        } else {
            d->tt_slot = parent->tt_slot;
            d->tt_port = parent->tt_port;
        }
    } else {
        d->root_port = port;
    }
    d->slot = xhci_alloc_slot(hc);
    if (!d->slot) goto fail;
    uint16_t mps0 = speed >= USB_SPEED_SUPER ? 512 : speed == USB_SPEED_HIGH ? 64 : 8;
    if (xhci_address_device(d->slot, d->root_port, d->route, speed, d->tt_slot, d->tt_port,
                            mps0) != 0)
        goto fail;
    ktime_busywait_ms(2);
    uint8_t head[8];
    if (usb_ctrl(d, 0x80, USB_REQ_GET_DESCRIPTOR, 0x0100, 0, 8, head) < 8) {
        serial_printf("usb: no device descriptor (port %u)\n", port);
        goto fail;
    }
    uint16_t real_mps = speed >= USB_SPEED_SUPER ? (uint16_t)(1u << (head[7] & 0xF)) : head[7];
    if (real_mps && real_mps != mps0 && xhci_update_ep0_mps(d->slot, real_mps) != 0)
        serial_printf("usb: EP0 max packet update failed\n");
    if (usb_ctrl(d, 0x80, USB_REQ_GET_DESCRIPTOR, 0x0100, 0, 18, &d->desc) < 18)
        goto fail;
    uint8_t cfg9[9];
    if (usb_ctrl(d, 0x80, USB_REQ_GET_DESCRIPTOR, 0x0200, 0, 9, cfg9) < 9)
        goto fail;
    uint16_t total = (uint16_t)(cfg9[2] | (cfg9[3] << 8));
    if (total > 1024) total = 1024;
    uint8_t *cfg = (uint8_t *)kalloc(1024);
    if (!cfg) goto fail;
    int got = usb_ctrl(d, 0x80, USB_REQ_GET_DESCRIPTOR, 0x0200, 0, total, cfg);
    if (got < 9) { kfree(cfg); goto fail; }
    d->config_value = cfg[5];
    parse_config(d, cfg, (uint32_t)got);
    kfree(cfg);
    get_string(d, d->desc.iManufacturer, d->manufacturer, sizeof(d->manufacturer));
    get_string(d, d->desc.iProduct, d->product, sizeof(d->product));
    if (usb_ctrl(d, 0x00, USB_REQ_SET_CONFIG, d->config_value, 0, 0, NULL) < 0) {
        serial_printf("usb: SET_CONFIGURATION failed\n");
        goto fail;
    }
    d->present = true;
    {
        char path[32];
        port_path(d, path, sizeof(path));
        printf("usb: dev %u %04x:%04x %s, %s speed, port %s %s %s\n", (unsigned)d->id,
               d->desc.idVendor, d->desc.idProduct,
               class_name(d->desc.bDeviceClass ? d->desc.bDeviceClass :
                          (d->n_iface ? d->iface[0].cls : 0)),
               xhci_speed_name(speed), path, d->manufacturer, d->product);
    }

    for (uint8_t i = 0; i < d->n_iface && d->n_bound < USB_MAX_BOUND; i++) {
        usb_iface_info_t *ifc = &d->iface[i];
        void *data = NULL;
        if (ifc->cls == 9) {
            hub_attach(d, ifc);
        } else if (ifc->cls == 3 && ifc->sub == 1 && (ifc->proto == 1 || ifc->proto == 2)) {
            int drv = ifc->proto == 1 ? USB_DRV_KBD : USB_DRV_MOUSE;
            if (usb_hid_attach(d, ifc, drv, &data) == 0) {
                d->bound[d->n_bound].drv = drv;
                d->bound[d->n_bound].iface = ifc->number;
                d->bound[d->n_bound].data = data;
                d->n_bound++;
            }
        } else if (ifc->cls == 8 && ifc->sub == 6 && ifc->proto == 0x50) {
            if (usb_msc_attach(d, ifc, &data) == 0) {
                d->bound[d->n_bound].drv = USB_DRV_MSC;
                d->bound[d->n_bound].iface = ifc->number;
                d->bound[d->n_bound].data = data;
                d->n_bound++;
            }
        } else if (ifc->cls == 8) {
            printf("usb dev %u: mass storage subclass %u protocol 0x%x not supported (BOT/SCSI only)\n",
                   (unsigned)d->id, ifc->sub, ifc->proto);
        }
    }
    return d;
fail:
    printf("usb: enumeration failed on %s port %u\n", parent ? "hub" : "root", port);
    if (d->slot) xhci_free_slot(d->slot);
    d->slot = NULL;
    d->present = false;
    return NULL;
}

static void usb_detach(usb_dev_t *d) {
    if (!d || !d->present) return;
    for (int i = 0; i < 16; i++) {
        if (d->child[i]) {
            usb_detach(d->child[i]);
            d->child[i] = NULL;
        }
    }
    for (int i = 0; i < d->n_bound; i++) {
        if (d->bound[i].drv == USB_DRV_MSC) usb_msc_detach(d->bound[i].data);
        else if (d->bound[i].drv == USB_DRV_KBD || d->bound[i].drv == USB_DRV_MOUSE)
            usb_hid_detach(d->bound[i].data);
    }
    d->n_bound = 0;
    printf("usb: dev %u %04x:%04x disconnected\n", (unsigned)d->id, d->desc.idVendor,
           d->desc.idProduct);
    if (d->hub_int_buf) {
        extern void dma_free_pages(void *p);
        dma_free_pages(d->hub_int_buf);
        d->hub_int_buf = NULL;
    }
    xhci_free_slot(d->slot);
    d->slot = NULL;
    if (!d->parent) d->hc->port_slot[d->root_port] = 0;
    d->present = false;
}

/* ------------------------------------------------------------------ hubs */
static int hub_port_status(usb_dev_t *h, uint8_t port, uint16_t *st, uint16_t *chg) {
    uint8_t b[4];
    if (usb_ctrl(h, 0xA3, USB_REQ_GET_STATUS, 0, port, 4, b) < 4) return -1;
    *st = (uint16_t)(b[0] | (b[1] << 8));
    *chg = (uint16_t)(b[2] | (b[3] << 8));
    return 0;
}

static void hub_clear(usb_dev_t *h, uint8_t port, uint16_t feature) {
    usb_ctrl(h, 0x23, USB_REQ_CLEAR_FEATURE, feature, port, 0, NULL);
}

static void hub_clear_changes(usb_dev_t *h, uint8_t port, uint16_t chg) {
    if (chg & 0x01) hub_clear(h, port, HUB_C_PORT_CONNECTION);
    if (chg & 0x02) hub_clear(h, port, HUB_C_PORT_ENABLE);
    if (!h->hub_ss && (chg & 0x04)) hub_clear(h, port, HUB_C_PORT_SUSPEND);
    if (chg & 0x08) hub_clear(h, port, HUB_C_PORT_OVER);
    if (chg & 0x10) hub_clear(h, port, HUB_C_PORT_RESET);
    if (h->hub_ss) {
        if (chg & 0x20) hub_clear(h, port, HUB_C_BH_PORT_RESET);
        if (chg & 0x40) hub_clear(h, port, HUB_C_PORT_LINK_STATE);
        if (chg & 0x80) hub_clear(h, port, HUB_C_PORT_CONFIG_ERR);
    }
}

static void hub_port_connect(usb_dev_t *h, uint8_t port) {
    uint16_t st = 0, chg = 0;
    if (hub_port_status(h, port, &st, &chg) != 0) return;
    hub_clear_changes(h, port, chg);
    if (!(st & 0x0001)) return;                     /* nothing connected */
    ktime_busywait_ms(100);                         /* debounce */
    usb_ctrl(h, 0x23, USB_REQ_SET_FEATURE, HUB_PORT_RESET, port, 0, NULL);
    uint64_t dl = ktime_deadline_ms(500);
    bool ok = false;
    while (!ktime_expired(dl)) {
        ktime_busywait_ms(10);
        if (hub_port_status(h, port, &st, &chg) != 0) break;
        if ((chg & 0x10) || (!(st & 0x0010) && (st & 0x0002))) { ok = true; break; }
    }
    hub_clear_changes(h, port, chg);
    if (!ok || !(st & 0x0002)) {
        serial_printf("usb dev %u: hub port %u reset failed (status 0x%x)\n", (unsigned)h->id, port, st);
        return;
    }
    ktime_busywait_ms(10);
    uint8_t speed;
    if (h->hub_ss) speed = USB_SPEED_SUPER;
    else if (st & 0x0200) speed = USB_SPEED_LOW;
    else if (st & 0x0400) speed = USB_SPEED_HIGH;
    else speed = USB_SPEED_FULL;
    usb_dev_t *c = usb_attach(h->hc, h, port, speed);
    if (c && port < 16) h->child[port] = c;
}

static void hub_attach(usb_dev_t *d, usb_iface_info_t *ifc) {
    d->hub_ss = d->speed >= USB_SPEED_SUPER;
    uint8_t hd[12];
    int n = usb_ctrl(d, 0xA0, USB_REQ_GET_DESCRIPTOR, d->hub_ss ? 0x2A00 : 0x2900, 0,
                     sizeof(hd), hd);
    if (n < 7) {
        printf("usb dev %u: hub descriptor failed\n", (unsigned)d->id);
        return;
    }
    uint8_t nports = hd[2];
    if (nports > 15) nports = 15;
    uint16_t chars = (uint16_t)(hd[3] | (hd[4] << 8));
    uint32_t pgood = (uint32_t)hd[5] * 2u;
    d->hub_ports = nports;
    bool mtt = d->desc.bDeviceProtocol == 2;
    if (xhci_set_hub(d->slot, nports, mtt, (uint8_t)((chars >> 5) & 3)) != 0)
        return;
    if (d->hub_ss)
        usb_ctrl(d, 0x20, USB_REQ_SET_HUB_DEPTH, d->depth, 0, 0, NULL);
    printf("usb dev %u: hub with %u ports%s\n", (unsigned)d->id, nports, mtt ? " (multi-TT)" : "");
    d->bound[d->n_bound].drv = USB_DRV_HUB;
    d->bound[d->n_bound].iface = ifc->number;
    d->bound[d->n_bound].data = NULL;
    d->n_bound++;
    for (uint8_t p = 1; p <= nports; p++)
        usb_ctrl(d, 0x23, USB_REQ_SET_FEATURE, HUB_PORT_POWER, p, 0, NULL);
    ktime_busywait_ms(pgood < 100 ? 100 : pgood);
    for (uint8_t p = 1; p <= nports; p++)
        hub_port_connect(d, p);
    /* Status-change endpoint for hotplug behind the hub. */
    for (uint8_t e = 0; e < ifc->n_ep; e++) {
        usb_ep_info_t *ep = &ifc->ep[e];
        if (ep->attr == 3 && (ep->addr & 0x80)) {
            uint64_t ph;
            extern void *dma_alloc_pages(uint32_t bytes, uint64_t *phys);
            if (xhci_configure_endpoint(d->slot, ep->addr, 3, ep->mps, ep->interval,
                                        ep->burst) == 0 &&
                (d->hub_int_buf = (uint8_t *)dma_alloc_pages(4096, &ph)) != NULL) {
                d->hub_int_ep = ep->addr;
                d->hub_int_queued = xhci_int_queue(d->slot, ep->addr, d->hub_int_buf,
                                                   ep->mps ? ep->mps : 1) == 0;
            }
            break;
        }
    }
}

static void hub_service(usb_dev_t *h) {
    if (!h->hub_int_queued) return;
    uint32_t bytes = 0;
    int cc = 0;
    if (xhci_int_poll(h->slot, h->hub_int_ep, &bytes, &cc) != 1) return;
    uint32_t bitmap = 0;
    for (uint32_t i = 0; i < bytes && i < 4; i++) bitmap |= (uint32_t)h->hub_int_buf[i] << (8 * i);
    if (cc == XHCI_CC_SUCCESS || cc == XHCI_CC_SHORT_PACKET) {
        for (uint8_t p = 1; p <= h->hub_ports; p++) {
            if (!(bitmap & (1u << p))) continue;
            uint16_t st = 0, chg = 0;
            if (hub_port_status(h, p, &st, &chg) != 0) continue;
            if (!(chg & 0x01)) { hub_clear_changes(h, p, chg); continue; }
            if (h->child[p]) {
                usb_detach(h->child[p]);
                h->child[p] = NULL;
            }
            if (st & 0x0001) hub_port_connect(h, p);
            else hub_clear_changes(h, p, chg);
        }
    }
    h->hub_int_queued = xhci_int_queue(h->slot, h->hub_int_ep, h->hub_int_buf,
                                       h->slot->ep[((h->hub_int_ep & 0xF) << 1) | 1].mps) == 0;
}

/* ------------------------------------------------------------- root ports */
static usb_dev_t *root_dev(xhci_ctrl_t *hc, uint32_t port) {
    for (int i = 0; i < USB_MAX_DEVICES; i++)
        if (usb_devs[i].present && usb_devs[i].hc == hc && !usb_devs[i].parent &&
            usb_devs[i].root_port == port)
            return &usb_devs[i];
    return NULL;
}

static void root_port_check(xhci_ctrl_t *hc, uint32_t port) {
    usb_dev_t *d = root_dev(hc, port);
    uint32_t sc = xhci_port_status(hc, port);
    bool conn = (sc & 1u) != 0;
    bool csc = (sc & (1u << 17)) != 0;
    xhci_port_ack_changes(hc, port);
    if (d && (!conn || csc)) {           /* unplugged, or replaced */
        usb_detach(d);
        d = NULL;
    }
    if (conn && !d) {
        uint8_t speed = 0;
        serial_printf("usb: root port %u connected (PORTSC 0x%x)\n", port, sc);
        ktime_busywait_ms(50);
        if (xhci_port_reset(hc, port, &speed) != 0) return;
        usb_dev_t *nd = usb_attach(hc, NULL, (uint8_t)port, speed);
        if (nd) hc->port_slot[port] = nd->slot->slot_id;
    }
}

void usb_core_init(void) {
    int n = xhci_init();
    usb_lock();
    for (int i = 0; i < n; i++) {
        xhci_ctrl_t *hc = xhci_get_controller(i);
        if (!hc) continue;
        ktime_busywait_ms(100);           /* let devices connect after power-on */
        for (uint32_t p = 1; p <= hc->max_ports; p++) {
            if (xhci_port_connected(hc, p)) root_port_check(hc, p);
            else xhci_port_ack_changes(hc, p);
        }
        xhci_process_events(hc);
        hc->port_changed = 0;
    }
    usb_ready = true;
    usb_unlock();
    int devs = 0;
    for (int i = 0; i < USB_MAX_DEVICES; i++) if (usb_devs[i].present) devs++;
    printf("USB: %d xHCI controller(s), %d device(s)\n", n, devs);
}

void usb_poll(void) {
    if (!usb_ready) return;
    static uint64_t last_us;
    uint64_t now = ktime_now_us();
    if (now - last_us < 2000) return;
    last_us = now;
    if (!usb_trylock()) return;
    for (int i = 0; i < XHCI_MAX_CTRL; i++) {
        xhci_ctrl_t *hc = xhci_get_controller(i);
        if (!hc) continue;
        xhci_process_events(hc);
        uint32_t ch = hc->port_changed;
        if (ch) {
            hc->port_changed = 0;
            for (uint32_t p = 1; p <= hc->max_ports; p++)
                if (ch & (1u << p)) root_port_check(hc, p);
        }
    }
    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        usb_dev_t *d = &usb_devs[i];
        if (!d->present) continue;
        if (d->hub_ports) hub_service(d);
        for (int b = 0; b < d->n_bound; b++) {
            if (d->bound[b].drv == USB_DRV_KBD || d->bound[b].drv == USB_DRV_MOUSE)
                usb_hid_poll(d->bound[b].data);
        }
    }
    usb_unlock();
}

static void port_path(usb_dev_t *d, char *out, uint32_t sz) {
    if (!d->parent) {
        snprintf(out, sz, "%u", d->root_port);
        return;
    }
    char p[24];
    port_path(d->parent, p, sizeof(p));
    snprintf(out, sz, "%s.%u", p, d->port);
}

static const char *drv_name(int drv) {
    switch (drv) {
    case USB_DRV_HUB: return "hub";
    case USB_DRV_KBD: return "keyboard";
    case USB_DRV_MOUSE: return "mouse";
    case USB_DRV_MSC: return "storage";
    default: return "-";
    }
}

void usb_list_devices(bool verbose) {
    int shown = 0;
    usb_lock();
    for (int i = 0; i < USB_MAX_DEVICES; i++) {
        usb_dev_t *d = &usb_devs[i];
        if (!d->present) continue;
        char path[32];
        port_path(d, path, sizeof(path));
        uint8_t cls = d->desc.bDeviceClass;
        uint8_t sub = d->desc.bDeviceSubClass, pro = d->desc.bDeviceProtocol;
        if (!cls && d->n_iface) {
            cls = d->iface[0].cls;
            sub = d->iface[0].sub;
            pro = d->iface[0].proto;
        }
        printf("Bus %d Port %-7s Dev %2u: ID %04x:%04x class %02x/%02x/%02x %-12s %-12s",
               d->hc->index, path, d->slot ? d->slot->address : 0,
               d->desc.idVendor, d->desc.idProduct, cls, sub, pro, class_name(cls),
               xhci_speed_name(d->speed));
        for (int b = 0; b < d->n_bound; b++) printf(" [%s]", drv_name(d->bound[b].drv));
        if (d->manufacturer[0] || d->product[0])
            printf(" %s %s", d->manufacturer, d->product);
        printf("\n");
        if (verbose) {
            printf("    USB %x.%02x, slot %u, route 0x%05x, TT %u/%u, config %u, %u interface(s)\n",
                   d->desc.bcdUSB >> 8, d->desc.bcdUSB & 0xFF, d->slot ? d->slot->slot_id : 0,
                   d->route, d->tt_slot, d->tt_port, d->config_value, d->n_iface);
            for (int k = 0; k < d->n_iface; k++) {
                usb_iface_info_t *f = &d->iface[k];
                printf("    if %u: class %02x/%02x/%02x %s", f->number, f->cls, f->sub,
                       f->proto, class_name(f->cls));
                for (int e = 0; e < f->n_ep; e++)
                    printf(" ep%02x(%s,%u)", f->ep[e].addr,
                           f->ep[e].attr == 2 ? "bulk" : f->ep[e].attr == 3 ? "int" : "iso",
                           f->ep[e].mps);
                printf("\n");
            }
        }
        shown++;
    }
    usb_unlock();
    if (!shown)
        printf("No USB devices (%d xHCI controller(s)).\n", xhci_controller_count());
}
