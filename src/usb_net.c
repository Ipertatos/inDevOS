/**
 * @file usb_net.c
 * @brief USB Ethernet: CDC-ECM, CDC-NCM and RNDIS on top of usb_core/xhci.
 *
 * One USB network adapter at a time is exposed to the network stack through
 * the usual net_device_t interface ("usbnet"), so DHCP, DNS, TCP and curl run
 * over it exactly like over the PCI NICs.
 *
 *  - CDC-ECM  (class 02/06): one Ethernet frame per bulk transfer. The MAC
 *    comes from the iMACAddress string of the Ethernet functional descriptor.
 *    Realtek RTL8152/8153 (and most ASIX/Apple/Samsung/... adapters) expose an
 *    ECM configuration that usb_core selects, so they work through this path.
 *  - CDC-NCM  (class 02/0D): NTB16 blocks, several frames per transfer.
 *  - RNDIS    (class 02/02/FF or E0/01/03, phone USB tethering): 44-byte
 *    packet header, INITIALIZE + QUERY MAC + SET packet filter.
 *
 * RX: one bulk IN transfer is kept queued; completions are picked up from
 * usb_poll() (main loop) and from the stack's receive path, split into
 * frames and put in a small ring. TX is a synchronous bulk OUT.
 */
#include "usb_core.h"
#include "net_device.h"
#include "mem.h"
#include "string.h"
#include "serial.h"
#include "stdio.h"
#include "ktime.h"
#include "atomic.h"

extern void *dma_alloc_pages(uint32_t bytes, uint64_t *phys);
extern void dma_free_pages(void *p);
extern void net_thread_notify_rx(void);

enum { UNET_ECM = 1, UNET_NCM, UNET_RNDIS };

#define UNET_RING      32
#define UNET_FRAME     1536
#define UNET_RX_ECM    2048
#define UNET_RX_BIG    (32u * 1024u)
#define UNET_TX_BUF    (16u * 1024u)

typedef struct {
    bool online;
    int type;
    usb_dev_t *dev;
    uint8_t ctl_if, data_if, data_alt;
    uint8_t ep_in, ep_out;
    uint16_t mps_out;
    uint8_t mac[6];
    uint8_t *rx, *tx, *ctl;
    uint32_t rx_size;
    bool rx_queued;
    /* frame ring: producer = pump (usb lock), consumer = receive_packet */
    uint8_t (*ring)[UNET_FRAME];
    uint16_t ring_len[UNET_RING];
    volatile uint32_t head, tail;
    spinlock_t ring_lk;
    /* NCM */
    uint32_t ntb_out_max;
    uint16_t ndp_div, ndp_rem, ndp_align;
    uint16_t ncm_seq;
    /* RNDIS */
    uint32_t rndis_id;
    /* stats */
    uint32_t rx_frames, tx_frames, rx_drop, tx_err, rx_err;
} unet_t;

static unet_t *unet;            /* the adapter bound to the network stack */
static unet_t *unet_spare;      /* detached adapter state, reused on next attach */
static uint32_t unet_leaked_bytes; /* only if two adapters are unplugged unreused */
static net_device_t unet_netdev;
static bool unet_registered;
static bool unet_stack_ready;   /* net_device layer initialised */
int usb_net_prefer_rndis;       /* test knob: `usbnet rndis` before plugging */

static const char *type_name(int t) {
    return t == UNET_ECM ? "CDC-ECM" : t == UNET_NCM ? "CDC-NCM" : t == UNET_RNDIS ? "RNDIS" : "?";
}

static inline void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static inline uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ----------------------------------------------------------------- RX */
static void ring_put(unet_t *n, const uint8_t *f, uint32_t len) {
    if (len == 0) return;                       /* zero-length packet ending a transfer */
    if (len < 14 || len > UNET_FRAME) { n->rx_err++; return; }
    spinlock_lock(&n->ring_lk);
    uint32_t h = n->head;
    if (h - n->tail >= UNET_RING) {
        spinlock_unlock(&n->ring_lk);
        n->rx_drop++;
        return;
    }
    memcpy(n->ring[h % UNET_RING], f, len);
    n->ring_len[h % UNET_RING] = (uint16_t)len;
    n->head = h + 1;
    spinlock_unlock(&n->ring_lk);
    n->rx_frames++;
}

static void rx_ncm(unet_t *n, const uint8_t *b, uint32_t len) {
    if (len < 12 || get32(b) != 0x484D434Eu) { n->rx_err++; return; }   /* "NCMH" */
    uint32_t ndp = get16(b + 10);
    for (int guard = 0; ndp && guard < 8; guard++) {
        if (ndp + 16 > len) { n->rx_err++; return; }
        uint32_t sig = get32(b + ndp);
        if (sig != 0x304D434Eu && sig != 0x314D434Eu) { n->rx_err++; return; }  /* NCM0/NCM1 */
        uint32_t nlen = get16(b + ndp + 4);
        uint32_t next = get16(b + ndp + 6);
        for (uint32_t o = ndp + 8; o + 4 <= ndp + nlen && o + 4 <= len; o += 4) {
            uint32_t di = get16(b + o), dl = get16(b + o + 2);
            if (!di || !dl) break;
            if (di + dl > len) { n->rx_err++; break; }
            ring_put(n, b + di, sig == 0x314D434Eu && dl > 4 ? dl - 4 : dl);
        }
        ndp = next;
    }
}

static void rx_rndis(unet_t *n, const uint8_t *b, uint32_t len) {
    uint32_t o = 0;
    while (o + 44 <= len) {
        uint32_t type = get32(b + o), mlen = get32(b + o + 4);
        if (type != 1 || mlen < 44 || o + mlen > len) { if (type != 1) n->rx_err++; break; }
        uint32_t doff = get32(b + o + 8), dlen = get32(b + o + 12);
        if (8 + doff + dlen <= mlen) ring_put(n, b + o + 8 + doff, dlen);
        else n->rx_err++;
        o += mlen;
    }
}

/* Pick up a completed bulk IN transfer and requeue. Caller holds usb_lock. */
static int unet_pump_locked(unet_t *n) {
    if (!n || !n->online) return 0;
    xhci_slot_t *s = n->dev->slot;
    if (!s) return 0;
    int got = 0;
    for (int round = 0; round < 4; round++) {
        if (!n->rx_queued) {
            if (xhci_int_queue(s, n->ep_in, n->rx, n->rx_size) != 0) return got;
            n->rx_queued = true;
        }
        uint32_t bytes = 0;
        int cc = 0;
        int r = xhci_int_poll(s, n->ep_in, &bytes, &cc);
        if (r <= 0) return got;
        n->rx_queued = false;
        if (cc != XHCI_CC_SUCCESS && cc != XHCI_CC_SHORT_PACKET) {
            n->rx_err++;
            if (cc == XHCI_CC_STALL) usb_clear_halt(n->dev, n->ep_in);
            return got;
        }
        uint32_t before = n->head;
        if (n->type == UNET_ECM) ring_put(n, n->rx, bytes);
        else if (n->type == UNET_NCM) rx_ncm(n, n->rx, bytes);
        else rx_rndis(n, n->rx, bytes);
        if (n->head != before) got++;
    }
    return got;
}

static void unet_pump(void) {
    unet_t *n = unet;
    if (!n || !n->online) return;
    if (!usb_trylock()) return;
    int got = unet_pump_locked(n);
    usb_unlock();
    if (got) net_thread_notify_rx();
}

/* --------------------------------------------------------- net_device ops */
static int unet_send(const uint8_t *data, uint32_t length) {
    unet_t *n = unet;
    if (!n || !n->online || !data || length < 14 || length > 1514) return -1;
    uint32_t flen = length < 60 ? 60 : length;
    uint32_t total;
    usb_lock();
    if (!n->online) { usb_unlock(); return -1; }
    uint8_t *t = n->tx;
    if (n->type == UNET_ECM) {
        memcpy(t, data, length);
        if (flen > length) memset(t + length, 0, flen - length);
        total = flen;
    } else if (n->type == UNET_RNDIS) {
        memset(t, 0, 44);
        put32(t, 1);
        put32(t + 4, 44 + flen);
        put32(t + 8, 36);
        put32(t + 12, flen);
        memcpy(t + 44, data, length);
        if (flen > length) memset(t + 44 + length, 0, flen - length);
        total = 44 + flen;
    } else {
        /* NTH16 | NDP16 (one datagram) | datagram */
        uint32_t ndp_off = 12;
        uint32_t al = n->ndp_align >= 4 ? n->ndp_align : 4;
        ndp_off = (ndp_off + al - 1) & ~(al - 1);
        uint32_t doff = ndp_off + 16;
        uint32_t div = n->ndp_div ? n->ndp_div : 4;
        while (doff % div != (uint32_t)(n->ndp_rem % div)) doff++;
        total = doff + flen;
        if (total > n->ntb_out_max || total > UNET_TX_BUF) { usb_unlock(); return -1; }
        memset(t, 0, doff);
        put32(t, 0x484D434Eu);
        put16(t + 4, 12);
        put16(t + 6, n->ncm_seq++);
        put16(t + 10, (uint16_t)ndp_off);
        put32(t + ndp_off, 0x304D434Eu);
        put16(t + ndp_off + 4, 16);
        put16(t + ndp_off + 6, 0);
        put16(t + ndp_off + 8, (uint16_t)doff);
        put16(t + ndp_off + 10, (uint16_t)flen);
        memcpy(t + doff, data, length);
        if (flen > length) memset(t + doff + length, 0, flen - length);
    }
    /* Avoid needing a zero-length packet: pad one byte (like Linux usbnet). */
    if (n->mps_out && (total % n->mps_out) == 0 && total < UNET_TX_BUF) {
        t[total++] = 0;
        if (n->type == UNET_NCM) put16(t + 8, (uint16_t)total);
        else if (n->type == UNET_RNDIS) put32(t + 4, total);
    } else if (n->type == UNET_NCM) {
        put16(t + 8, (uint16_t)total);
    }
    int r = xhci_bulk(n->dev->slot, n->ep_out, t, total, 1000);
    if (r == -XHCI_CC_STALL) usb_clear_halt(n->dev, n->ep_out);
    if (r != (int)total) n->tx_err++;
    else n->tx_frames++;
    usb_unlock();
    return r == (int)total ? 0 : -1;          /* stack convention: 0 = sent */
}

static int unet_receive(uint8_t *buffer, uint32_t max_length) {
    unet_t *n = unet;
    if (!n) return 0;
    if (n->head == n->tail) unet_pump();
    spinlock_lock(&n->ring_lk);
    if (n->head == n->tail) { spinlock_unlock(&n->ring_lk); return 0; }
    uint32_t i = n->tail % UNET_RING;
    uint32_t len = n->ring_len[i];
    if (len > max_length) len = max_length;
    memcpy(buffer, n->ring[i], len);
    n->tail++;
    spinlock_unlock(&n->ring_lk);
    return (int)len;
}

static int unet_pending(void) {
    unet_t *n = unet;
    if (!n) return 0;
    if (n->head == n->tail) unet_pump();
    return n->head != n->tail;
}

static void unet_get_mac(uint8_t *mac) {
    if (unet) memcpy(mac, unet->mac, 6);
    else memcpy(mac, unet_netdev.mac_address, 6);
}
static void unet_irq(void) { }
static int unet_init_op(void) { return unet && unet->online ? 0 : -1; }
static const char *unet_name(void) {
    if (!unet) return "usbnet";
    return unet->type == UNET_ECM ? "usbnet (CDC-ECM)" :
           unet->type == UNET_NCM ? "usbnet (CDC-NCM)" : "usbnet (RNDIS)";
}
static int unet_is_init(void) { return unet && unet->online; }

static net_device_t unet_netdev = {
    .name = "usbnet",
    .ops = {
        .init = unet_init_op,
        .send_packet = unet_send,
        .receive_packet = unet_receive,
        .get_mac_address = unet_get_mac,
        .has_pending_receive = unet_pending,
        .handle_interrupt = unet_irq,
        .get_driver_name = unet_name,
        .is_initialized = unet_is_init,
    },
};

static int unet_register(void) {
    if (!unet || !unet->online || !unet_stack_ready) return -1;
    memcpy(unet_netdev.mac_address, unet->mac, 6);
    unet_netdev.initialized = 1;
    unet_netdev.pci_dev = NULL;
    unet_netdev.driver_data = unet;
    if (!unet_registered) {
        unet_registered = true;
        return net_device_register(&unet_netdev);
    }
    /* re-plugged: become current again unless another NIC is active */
    net_device_t *cur = net_device_get_current();
    if (!cur || cur == &unet_netdev || !cur->initialized) net_device_set_current(&unet_netdev);
    return 0;
}

/* Called by net_device_pci_bringup(): the stack is ready; register an
 * adapter that was found during USB enumeration. Returns 0 if registered. */
int usb_net_bringup(void) {
    unet_stack_ready = true;
    if (!unet || !unet->online) return -1;
    printf("net: using USB ethernet %s %02x:%02x:%02x:%02x:%02x:%02x\n", type_name(unet->type),
           unet->mac[0], unet->mac[1], unet->mac[2], unet->mac[3], unet->mac[4], unet->mac[5]);
    return unet_register();
}

bool usb_net_present(void) { return unet && unet->online; }

/* ------------------------------------------------------------- RNDIS */
static int rndis_cmd(unet_t *n, uint32_t len, uint32_t expect, uint8_t **resp) {
    uint32_t id = get32(n->ctl + 8);
    if (usb_ctrl(n->dev, 0x21, 0x00, 0, n->ctl_if, (uint16_t)len, n->ctl) < 0) return -1;
    uint8_t *r = n->ctl + 1024;
    for (int t = 0; t < 50; t++) {
        int got = usb_ctrl(n->dev, 0xA1, 0x01, 0, n->ctl_if, 1024, r);
        if (got >= 16 && get32(r) == expect && get32(r + 8) == id) {
            if (get32(r + 12) != 0) return -2;       /* RNDIS_STATUS not success */
            *resp = r;
            return got;
        }
        ktime_busywait_ms(10);
    }
    return -1;
}

static int rndis_init(unet_t *n) {
    uint8_t *c = n->ctl, *r;
    memset(c, 0, 64);
    put32(c, 2); put32(c + 4, 24); put32(c + 8, ++n->rndis_id);
    put32(c + 12, 1); put32(c + 16, 0); put32(c + 20, 0x4000);
    if (rndis_cmd(n, 24, 0x80000002u, &r) < 0) { printf("usbnet: RNDIS INITIALIZE failed\n"); return -1; }
    /* OID_802_3_PERMANENT_ADDRESS */
    memset(c, 0, 64);
    put32(c, 4); put32(c + 4, 28); put32(c + 8, ++n->rndis_id); put32(c + 12, 0x01010101u);
    int got = rndis_cmd(n, 28, 0x80000004u, &r);
    if (got >= 24) {
        uint32_t blen = get32(r + 16), boff = get32(r + 20);
        if (blen >= 6 && 8 + boff + 6 <= (uint32_t)got) memcpy(n->mac, r + 8 + boff, 6);
    }
    /* OID_GEN_CURRENT_PACKET_FILTER = directed | multicast | broadcast */
    memset(c, 0, 64);
    put32(c, 5); put32(c + 4, 32); put32(c + 8, ++n->rndis_id); put32(c + 12, 0x0001010Eu);
    put32(c + 16, 4); put32(c + 20, 20); put32(c + 28, 0x0B);
    if (rndis_cmd(n, 32, 0x80000005u, &r) < 0) { printf("usbnet: RNDIS packet filter failed\n"); return -1; }
    return 0;
}

/* ------------------------------------------------------------- attach */
static int hexval(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static bool mac_from_string(unet_t *n, uint8_t idx) {
    if (!idx) return false;
    uint8_t *b = n->ctl;
    int got = usb_ctrl(n->dev, 0x80, 6, (uint16_t)(0x0300 | idx), 0x0409, 64, b);
    if (got < 2 + 24 || b[1] != 3) return false;
    for (int i = 0; i < 6; i++) {
        int hi = hexval((char)b[2 + i * 4]), lo = hexval((char)b[2 + i * 4 + 2]);
        if (hi < 0 || lo < 0) return false;
        n->mac[i] = (uint8_t)(hi << 4 | lo);
    }
    return true;
}

int usb_net_kind(const usb_iface_info_t *ifc) {
    if (ifc->cls == 0x02 && ifc->sub == 0x06) return UNET_ECM;
    if (ifc->cls == 0x02 && ifc->sub == 0x0D) return UNET_NCM;
    if (ifc->cls == 0x02 && ifc->sub == 0x02 && ifc->proto == 0xFF) return UNET_RNDIS;
    if (ifc->cls == 0xE0 && ifc->sub == 0x01 && ifc->proto == 0x03) return UNET_RNDIS;
    if (ifc->cls == 0xEF && ifc->sub == 0x04 && ifc->proto == 0x01) return UNET_RNDIS;
    return 0;
}

int usb_net_attach(usb_dev_t *d, usb_iface_info_t *ifc, void **data) {
    int kind = usb_net_kind(ifc);
    if (!kind) return -1;
    if (unet) {
        printf("usbnet: dev %u ignored, only one USB ethernet adapter is supported\n", (unsigned)d->id);
        return -1;
    }
    /* data interface: the interface after the control one with class 0A */
    usb_iface_info_t *di = NULL;
    for (int i = 0; i < d->n_iface; i++) {
        usb_iface_info_t *f = &d->iface[i];
        if (f->cls == 0x0A && f->number > ifc->number && f->n_ep >= 2) { di = f; break; }
    }
    if (!di) {
        printf("usbnet: dev %u: no CDC data interface with bulk endpoints\n", (unsigned)d->id);
        return -1;
    }
    usb_ep_info_t *ein = NULL, *eout = NULL;
    for (int e = 0; e < di->n_ep; e++) {
        if (di->ep[e].attr != 2) continue;
        if ((di->ep[e].addr & 0x80) && !ein) ein = &di->ep[e];
        if (!(di->ep[e].addr & 0x80) && !eout) eout = &di->ep[e];
    }
    if (!ein || !eout) return -1;

    /* Reuse the adapter state of a previously unplugged adapter. It can not be
     * freed at detach time (a sender may still hold the pointer while waiting
     * for usb_lock), so recycling it bounds the memory to one adapter instead of
     * leaking ~70-100 KiB per unplug. Buffers are always sized for the largest
     * mode so any kind can reuse them. */
    unet_t *n = unet_spare;
    bool reused = n != NULL;
    uint64_t ph;
    if (reused) {
        unet_spare = NULL;
        spinlock_lock(&n->ring_lk);
        n->head = n->tail = 0;
        spinlock_unlock(&n->ring_lk);
        n->online = false;
        n->rx_queued = false;
        memset(n->mac, 0, sizeof(n->mac));
        n->ndp_rem = 0; n->ncm_seq = 0; n->rndis_id = 0;
        n->rx_frames = n->tx_frames = n->rx_drop = n->tx_err = n->rx_err = 0;
    } else {
        n = (unet_t *)kalloc(sizeof(unet_t));
        if (!n) return -1;
        memset(n, 0, sizeof(*n));
        n->ring = (uint8_t (*)[UNET_FRAME])kalloc(UNET_RING * UNET_FRAME);
        n->rx = (uint8_t *)dma_alloc_pages(UNET_RX_BIG, &ph);
        n->tx = (uint8_t *)dma_alloc_pages(UNET_TX_BUF, &ph);
        n->ctl = (uint8_t *)dma_alloc_pages(4096, &ph);
        if (!n->ring || !n->rx || !n->tx || !n->ctl) goto fail_free;
    }
    n->rx_size = kind == UNET_ECM ? UNET_RX_ECM : UNET_RX_BIG;
    n->type = kind;
    n->dev = d;
    n->ctl_if = ifc->number;
    n->data_if = di->number;
    n->data_alt = di->alt;
    n->ep_in = ein->addr;
    n->ep_out = eout->addr;
    n->mps_out = eout->mps;
    n->ntb_out_max = UNET_TX_BUF;
    n->ndp_div = 4;
    n->ndp_align = 4;

    if (kind == UNET_NCM) {
        uint8_t *p = n->ctl;
        if (usb_ctrl(d, 0xA1, 0x80, 0, n->ctl_if, 28, p) >= 28) {     /* GET_NTB_PARAMETERS */
            uint32_t in_max = get32(p + 4);
            n->ntb_out_max = get32(p + 16);
            n->ndp_div = get16(p + 20);
            n->ndp_rem = get16(p + 22);
            n->ndp_align = get16(p + 24);
            if (n->ntb_out_max > UNET_TX_BUF || !n->ntb_out_max) n->ntb_out_max = UNET_TX_BUF;
            if (in_max > n->rx_size) {                               /* SET_NTB_INPUT_SIZE */
                put32(p, n->rx_size);
                if (usb_ctrl(d, 0x21, 0x86, 0, n->ctl_if, 4, p) < 0)
                    printf("usbnet: NCM device wants %u-byte NTBs, SET_NTB_INPUT_SIZE failed\n", in_max);
            }
        }
    }
    if (xhci_configure_endpoint(d->slot, n->ep_in, 2, ein->mps, 0, ein->burst) != 0 ||
        xhci_configure_endpoint(d->slot, n->ep_out, 2, eout->mps, 0, eout->burst) != 0) {
        printf("usbnet: configure endpoint failed\n");
        goto fail;
    }
    if (kind != UNET_RNDIS) {
        /* select the data alternate setting that has the bulk endpoints */
        if (n->data_alt && usb_ctrl(d, 0x01, 0x0B, n->data_alt, n->data_if, 0, NULL) < 0)
            printf("usbnet: SET_INTERFACE %u alt %u failed\n", n->data_if, n->data_alt);
        if (!mac_from_string(n, ifc->cdc_mac)) {
            n->mac[0] = 0x02; n->mac[1] = 0x55; n->mac[2] = 0x53;
            n->mac[3] = 0x42; n->mac[4] = (uint8_t)d->desc.idProduct; n->mac[5] = (uint8_t)d->id;
            printf("usbnet: no MAC string, using locally administered address\n");
        }
        /* SET_ETHERNET_PACKET_FILTER: directed | broadcast | all multicast */
        usb_ctrl(d, 0x21, 0x43, 0x000E, n->ctl_if, 0, NULL);
    } else if (rndis_init(n) != 0) {
        goto fail;
    }
    n->online = true;
    unet = n;
    *data = n;
    printf("usbnet: dev %u %04x:%04x %s, MAC %02x:%02x:%02x:%02x:%02x:%02x, bulk in %02x out %02x\n",
           (unsigned)d->id, d->desc.idVendor, d->desc.idProduct, type_name(kind),
           n->mac[0], n->mac[1], n->mac[2], n->mac[3], n->mac[4], n->mac[5], n->ep_in, n->ep_out);
    unet_pump_locked(n);
    if (unet_stack_ready) unet_register();
    return 0;
fail:
    /* never bound: nobody else can hold it, but keep it for the next attach */
    unet_spare = n;
    return -1;
fail_free:
    if (n->rx) dma_free_pages(n->rx);
    if (n->tx) dma_free_pages(n->tx);
    if (n->ctl) dma_free_pages(n->ctl);
    if (n->ring) kfree(n->ring);
    kfree(n);
    return -1;
}

void usb_net_detach(void *data) {
    unet_t *n = (unet_t *)data;
    if (!n) return;
    n->online = false;
    if (unet == n) {
        unet_netdev.initialized = 0;
        unet_netdev.driver_data = NULL;
        unet = NULL;
        printf("usbnet: adapter removed\n");
    }
    /* Buffers are not freed (a send may still be inside xhci_bulk on another
     * CPU waiting for usb_lock); the state is recycled by the next attach. */
    if (!unet_spare) unet_spare = n;
    else unet_leaked_bytes += sizeof(unet_t) + UNET_RING * UNET_FRAME + UNET_RX_BIG + UNET_TX_BUF + 4096;
}

void usb_net_poll(void *data) {
    unet_t *n = (unet_t *)data;
    if (!n || n != unet || !n->online) return;
    if (unet_pump_locked(n)) net_thread_notify_rx();
}

void usb_net_status(void) {
    printf("usbnet: preferred function for new adapters: %s\n",
           usb_net_prefer_rndis ? "RNDIS" : "CDC-ECM");
    printf("usbnet: spare state %s, unrecycled %u bytes\n", unet_spare ? "cached" : "none",
           unet_leaked_bytes);
    unet_t *n = unet;
    if (!n) { printf("usbnet: no USB ethernet adapter\n"); return; }
    printf("usbnet: %s on usb dev %u, MAC %02x:%02x:%02x:%02x:%02x:%02x\n", type_name(n->type),
           (unsigned)n->dev->id, n->mac[0], n->mac[1], n->mac[2], n->mac[3], n->mac[4], n->mac[5]);
    printf("  rx %u frames (%u dropped, %u errors), tx %u frames (%u errors)\n", n->rx_frames,
           n->rx_drop, n->rx_err, n->tx_frames, n->tx_err);
}
