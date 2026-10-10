/**
 * @file xhci.c
 * @brief xHCI (USB 3.x) host controller driver.
 *
 * Polled (no interrupts): every wait drains the event ring itself, and the
 * main loop calls usb_poll() which drains it for HID reports and hotplug.
 *
 * Bring-up per controller: BIOS->OS handoff (USBLEGSUP), SMI disable, Intel
 * EHCI->xHCI port routing, halt + reset, BAR mapped uncached, DCBAA,
 * scratchpad buffers, command ring, one-segment event ring on interrupter 0,
 * port power. Protocol capabilities tell USB2 from USB3 root ports (they
 * reset differently).
 *
 * Locking: callers take usb_lock() (recursive per CPU) around any sequence
 * of commands/transfers; the periodic poll uses usb_trylock().
 */
#include "xhci.h"
#include "pci.h"
#include "mem.h"
#include "serial.h"
#include "string.h"
#include "ktime.h"
#include "stdio.h"
#include "atomic.h"

extern uint64_t pci_map_bar_uc(uint64_t phys, uint64_t size);
extern void *dma_alloc_pages(uint32_t bytes, uint64_t *phys);
extern void dma_free_pages(void *p);

static xhci_ctrl_t xhci_ctrls[XHCI_MAX_CTRL];
static int xhci_count;

/* ---------------------------------------------------------------- lock */
static spinlock_t usb_lk;
static volatile int usb_owner = -1;
static int usb_depth;

void usb_lock(void) {
    preempt_disable();
    int c = (int)smp_cpu_index_fast();
    if (usb_owner == c) { usb_depth++; return; }
    spinlock_lock(&usb_lk);
    usb_owner = c;
    usb_depth = 1;
}

bool usb_trylock(void) {
    preempt_disable();
    int c = (int)smp_cpu_index_fast();
    if (usb_owner == c) { usb_depth++; return true; }
    if (__sync_lock_test_and_set(&usb_lk.lock, 1) == 0) {
        usb_owner = c;
        usb_depth = 1;
        return true;
    }
    preempt_enable();
    return false;
}

void usb_unlock(void) {
    if (--usb_depth == 0) {
        usb_owner = -1;
        spinlock_unlock(&usb_lk);
    }
    preempt_enable();
}

/* ------------------------------------------------------------ registers */
#define OP_USBCMD   0x00
#define OP_USBSTS   0x04
#define OP_PAGESIZE 0x08
#define OP_DNCTRL   0x14
#define OP_CRCR     0x18
#define OP_DCBAAP   0x30
#define OP_CONFIG   0x38
#define OP_PORTSC(p) (0x400u + 0x10u * ((p) - 1u))

#define CMD_RS    (1u << 0)
#define CMD_HCRST (1u << 1)
#define STS_HCH   (1u << 0)
#define STS_HSE   (1u << 2)
#define STS_CNR   (1u << 11)

#define PORTSC_CCS  (1u << 0)
#define PORTSC_PED  (1u << 1)
#define PORTSC_PR   (1u << 4)
#define PORTSC_PP   (1u << 9)
#define PORTSC_CSC  (1u << 17)
#define PORTSC_PRC  (1u << 21)
#define PORTSC_WRC  (1u << 19)
#define PORTSC_WPR  (1u << 31)
#define PORTSC_CHANGES 0x00FE0000u
#define PORTSC_KEEP    (PORTSC_PP | (7u << 25))

#define RT_IMAN   0x20
#define RT_IMOD   0x24
#define RT_ERSTSZ 0x28
#define RT_ERSTBA 0x30
#define RT_ERDP   0x38

/* TRB types and bits */
#define TRB_NORMAL   1
#define TRB_SETUP    2
#define TRB_DATA     3
#define TRB_STATUS   4
#define TRB_LINK     6
#define TRB_EN_SLOT  9
#define TRB_DIS_SLOT 10
#define TRB_ADDR_DEV 11
#define TRB_CFG_EP   12
#define TRB_EVAL_CTX 13
#define TRB_RESET_EP 14
#define TRB_STOP_EP  15
#define TRB_SET_DEQ  16
#define TRB_EV_XFER  32
#define TRB_EV_CMD   33
#define TRB_EV_PORT  34
#define TRB_EV_HC    37

#define TRBF_CYCLE (1u << 0)
#define TRBF_TC    (1u << 1)
#define TRBF_ISP   (1u << 2)
#define TRBF_CH    (1u << 4)
#define TRBF_IOC   (1u << 5)
#define TRBF_IDT   (1u << 6)
#define TRBF_TYPE(t) ((uint32_t)(t) << 10)

static inline uint32_t rd32(volatile uint8_t *b, uint32_t off) {
    return *(volatile uint32_t *)(b + off);
}
static inline void wr32(volatile uint8_t *b, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(b + off) = v;
}
static inline void wr64(volatile uint8_t *b, uint32_t off, uint64_t v) {
    wr32(b, off, (uint32_t)v);
    wr32(b, off + 4, (uint32_t)(v >> 32));
}
static inline void mb(void) { __asm__ volatile("mfence" ::: "memory"); }

int xhci_controller_count(void) { return xhci_count; }
xhci_ctrl_t *xhci_get_controller(int i) {
    return (i >= 0 && i < xhci_count && xhci_ctrls[i].initialized) ? &xhci_ctrls[i] : NULL;
}

const char *xhci_speed_name(uint8_t speed) {
    switch (speed) {
    case USB_SPEED_LOW: return "low (1.5M)";
    case USB_SPEED_FULL: return "full (12M)";
    case USB_SPEED_HIGH: return "high (480M)";
    case USB_SPEED_SUPER: return "super (5G)";
    case USB_SPEED_SUPERP: return "super+ (10G)";
    default: return "?";
    }
}

/* ---------------------------------------------------------------- rings */
static int ring_init(xhci_ring_t *r) {
    r->trbs = (xhci_trb_t *)dma_alloc_pages(XHCI_RING_TRBS * sizeof(xhci_trb_t), &r->phys);
    if (!r->trbs) return -1;
    r->enq = 0;
    r->cycle = 1;
    xhci_trb_t *l = &r->trbs[XHCI_RING_TRBS - 1];
    l->param = r->phys;
    l->status = 0;
    l->control = TRBF_TYPE(TRB_LINK) | TRBF_TC;        /* cycle 0: not ours yet */
    return 0;
}

static void ring_free(xhci_ring_t *r) {
    if (r->trbs) dma_free_pages(r->trbs);
    r->trbs = NULL;
}

/* Queue one TRB; returns its physical address. */
static uint64_t ring_push(xhci_ring_t *r, uint64_t param, uint32_t status, uint32_t control) {
    xhci_trb_t *t = &r->trbs[r->enq];
    uint64_t phys = r->phys + (uint64_t)r->enq * sizeof(xhci_trb_t);
    t->param = param;
    t->status = status;
    mb();
    t->control = (control & ~TRBF_CYCLE) | r->cycle;
    r->enq++;
    if (r->enq == XHCI_RING_TRBS - 1) {
        xhci_trb_t *l = &r->trbs[r->enq];
        l->param = r->phys;
        l->status = 0;
        mb();
        l->control = TRBF_TYPE(TRB_LINK) | TRBF_TC | (control & TRBF_CH) | r->cycle;
        r->enq = 0;
        r->cycle ^= 1;
    }
    return phys;
}

static uint64_t ring_deq_ptr(xhci_ring_t *r) {
    return (r->phys + (uint64_t)r->enq * sizeof(xhci_trb_t)) | r->cycle;
}

/* --------------------------------------------------------------- events */
void xhci_process_events(xhci_ctrl_t *hc) {
    if (!hc || !hc->initialized) return;
    bool any = false;
    for (int guard = 0; guard < XHCI_EVT_TRBS; guard++) {
        xhci_trb_t *e = &hc->evt[hc->evt_deq];
        uint32_t ctl = e->control;
        if ((ctl & TRBF_CYCLE) != hc->evt_cycle)
            break;
        mb();
        uint32_t type = (ctl >> 10) & 0x3F;
        int cc = (int)(e->status >> 24);
        if (type == TRB_EV_CMD) {
            hc->cmd_cc = cc;
            hc->cmd_slot = (uint8_t)(ctl >> 24);
            hc->cmd_trb = e->param;
            hc->cmd_done = true;
        } else if (type == TRB_EV_XFER) {
            uint8_t slot = (uint8_t)(ctl >> 24);
            uint8_t dci = (uint8_t)((ctl >> 16) & 0x1F);
            xhci_slot_t *s = (slot <= XHCI_MAX_SLOTS) ? hc->slots[slot] : NULL;
            if (s && dci < 32) {
                xhci_ep_t *ep = &s->ep[dci];
                bool last = (e->param == ep->last_trb);
                if (cc == XHCI_CC_SHORT_PACKET && !last) {
                    /* Short data stage / short TRB inside a TD. */
                    ep->short_trb = e->param;
                    ep->short_residual = e->status & 0xFFFFFF;
                    ep->short_seen = true;
                    if (ep->type != 4) {          /* bulk/int: TD ends here */
                        ep->cc = cc;
                        ep->residual = e->status & 0xFFFFFF;
                        ep->done = true;
                    }
                } else if (last || (cc != XHCI_CC_SUCCESS && cc != XHCI_CC_SHORT_PACKET)) {
                    ep->cc = cc;
                    ep->residual = e->status & 0xFFFFFF;
                    ep->done = true;
                    if (last && cc == XHCI_CC_SHORT_PACKET) {
                        ep->short_trb = e->param;
                        ep->short_residual = e->status & 0xFFFFFF;
                        ep->short_seen = true;
                    }
                }
            }
        } else if (type == TRB_EV_PORT) {
            uint32_t port = (uint32_t)(e->param >> 24) & 0xFF;
            if (port >= 1 && port <= 31)
                hc->port_changed |= 1u << port;
        } else if (type == TRB_EV_HC) {
            serial_printf("xhci%d: host controller event cc=%d\n", hc->index, cc);
        }
        hc->evt_deq++;
        if (hc->evt_deq == XHCI_EVT_TRBS) {
            hc->evt_deq = 0;
            hc->evt_cycle ^= 1;
        }
        any = true;
    }
    if (any)
        wr64(hc->rt, RT_ERDP, (hc->evt_phys + (uint64_t)hc->evt_deq * sizeof(xhci_trb_t)) | 8u);
}

/* ------------------------------------------------------------- commands */
static int xhci_command(xhci_ctrl_t *hc, uint64_t param, uint32_t status,
                        uint32_t control, uint32_t timeout_ms) {
    hc->cmd_done = false;
    uint64_t trb = ring_push(&hc->cmd, param, status, control);
    hc->db[0] = 0;
    uint64_t dl = ktime_deadline_ms(timeout_ms);
    for (;;) {
        xhci_process_events(hc);
        if (hc->cmd_done && hc->cmd_trb == trb)
            return hc->cmd_cc;
        if (hc->cmd_done)
            hc->cmd_done = false;          /* stale completion */
        if (ktime_expired(dl)) {
            serial_printf("xhci%d: command type %u timed out\n", hc->index,
                          (control >> 10) & 0x3F);
            return XHCI_CC_TIMEOUT;
        }
        __asm__ volatile("pause");
    }
}

/* -------------------------------------------------------------- contexts */
static inline uint32_t *in_ctrl(xhci_slot_t *s) { return (uint32_t *)s->in_ctx; }
static inline uint32_t *in_slot(xhci_slot_t *s) {
    return (uint32_t *)((uint8_t *)s->in_ctx + s->hc->ctx_size);
}
static inline uint32_t *in_ep(xhci_slot_t *s, uint32_t dci) {
    return (uint32_t *)((uint8_t *)s->in_ctx + s->hc->ctx_size * (1u + dci));
}
static inline uint32_t *out_slot(xhci_slot_t *s) { return (uint32_t *)s->out_ctx; }
static inline uint32_t *out_ep(xhci_slot_t *s, uint32_t dci) {
    return (uint32_t *)((uint8_t *)s->out_ctx + s->hc->ctx_size * dci);
}

static void in_clear(xhci_slot_t *s) {
    memset(s->in_ctx, 0, s->hc->ctx_size * 33u);
}

/* ---------------------------------------------------------- bring-up */
static uint64_t bar_size(struct pci_device *dev, uint32_t off, bool is64) {
    uint32_t cmd = pci_device_read(dev, 0x04, 2);
    pci_device_write(dev, 0x04, 2, cmd & ~0x6u);
    uint32_t lo = pci_device_read(dev, off, 4);
    uint32_t hi = is64 ? pci_device_read(dev, off + 4, 4) : 0;
    pci_device_write(dev, off, 4, 0xFFFFFFFFu);
    if (is64) pci_device_write(dev, off + 4, 4, 0xFFFFFFFFu);
    uint64_t m = pci_device_read(dev, off, 4) & ~0xFull;
    if (is64) m |= (uint64_t)pci_device_read(dev, off + 4, 4) << 32;
    else m |= 0xFFFFFFFF00000000ull;
    pci_device_write(dev, off, 4, lo);
    if (is64) pci_device_write(dev, off + 4, 4, hi);
    pci_device_write(dev, 0x04, 2, cmd);
    uint64_t size = ~m + 1;
    if (size < 0x1000 || size > (64ull << 20)) size = 0x10000;
    return size;
}

static void xhci_ext_caps(xhci_ctrl_t *hc) {
    uint32_t hcc1 = rd32(hc->cap, 0x10);
    uint32_t off = ((hcc1 >> 16) & 0xFFFF) << 2;
    for (int guard = 0; off && guard < 64; guard++) {
        uint32_t v = rd32(hc->cap, off);
        uint8_t id = v & 0xFF;
        if (id == 1) {
            /* USB Legacy Support: take ownership from the BIOS. */
            if (v & (1u << 16)) {
                *(volatile uint8_t *)(hc->cap + off + 3) = 1;
                uint64_t dl = ktime_deadline_ms(1000);
                while ((rd32(hc->cap, off) & (1u << 16)) && !ktime_expired(dl))
                    __asm__ volatile("pause");
                if (rd32(hc->cap, off) & (1u << 16)) {
                    serial_printf("xhci%d: BIOS did not release the controller; forcing\n",
                                  hc->index);
                    *(volatile uint8_t *)(hc->cap + off + 2) = 0;
                }
                printf("xhci%d: took ownership from the BIOS (USBLEGSUP)\n", hc->index);
            } else {
                *(volatile uint8_t *)(hc->cap + off + 3) = 1;
            }
            /* USBLEGCTLSTS: disable SMIs, clear the RW1C status bits. */
            uint32_t ctl = rd32(hc->cap, off + 4);
            wr32(hc->cap, off + 4, (ctl & ~0x0000E011u) | 0xE0000000u);
        } else if (id == 2) {
            uint8_t major = (uint8_t)(v >> 24);
            uint32_t d2 = rd32(hc->cap, off + 8);
            uint32_t first = d2 & 0xFF, cnt = (d2 >> 8) & 0xFF;
            for (uint32_t p = first; p < first + cnt && p <= XHCI_MAX_PORTS; p++)
                if (p >= 1) hc->port_major[p] = major;
            serial_printf("xhci%d: USB %u.%x ports %u-%u\n", hc->index, major,
                          (v >> 16) & 0xFF, first, first + cnt - 1);
        }
        uint32_t next = (v >> 8) & 0xFF;
        off = next ? off + (next << 2) : 0;
    }
}

static int xhci_init_one(xhci_ctrl_t *hc, struct pci_device *dev) {
    hc->pci = dev;
    uint32_t bar_lo = pci_device_read(dev, 0x10, 4);
    bool is64 = (bar_lo & 0x6) == 0x4;
    uint64_t phys = bar_lo & ~0xFull;
    if (is64) phys |= (uint64_t)pci_device_read(dev, 0x14, 4) << 32;
    if (bar_lo & 1 || phys == 0) {
        printf("xhci%d: BAR0 is not a memory BAR\n", hc->index);
        return -1;
    }
    uint64_t size = bar_size(dev, 0x10, is64);
    uint32_t cmd = pci_device_read(dev, 0x04, 2);
    pci_device_write(dev, 0x04, 2, (cmd | 0x6u) & ~0x400u);   /* mem + master */

    uint64_t va = pci_map_bar_uc(phys, size);
    if (!va) {
        printf("xhci%d: cannot map BAR0 0x%llx\n", hc->index, (unsigned long long)phys);
        return -1;
    }
    hc->cap = (volatile uint8_t *)va;
    hc->op = hc->cap + (rd32(hc->cap, 0) & 0xFF);
    hc->rt = hc->cap + (rd32(hc->cap, 0x18) & ~0x1Fu);
    hc->db = (volatile uint32_t *)(hc->cap + (rd32(hc->cap, 0x14) & ~0x3u));
    uint32_t hcs1 = rd32(hc->cap, 0x04), hcs2 = rd32(hc->cap, 0x08);
    uint32_t hcc1 = rd32(hc->cap, 0x10);
    hc->max_slots = hcs1 & 0xFF;
    if (hc->max_slots > XHCI_MAX_SLOTS) hc->max_slots = XHCI_MAX_SLOTS;
    hc->max_ports = (hcs1 >> 24) & 0xFF;
    if (hc->max_ports > XHCI_MAX_PORTS) hc->max_ports = XHCI_MAX_PORTS;
    hc->ctx_size = (hcc1 & (1u << 2)) ? 64 : 32;
    hc->ac64 = (hcc1 & 1) != 0;
    printf("xhci%d: %02x:%02x.%x %04x:%04x v%x.%02x BAR 0x%llx (%llu KiB) slots %u ports %u ctx %u%s\n",
           hc->index, dev->bus, dev->device, dev->function, dev->vendor_id, dev->device_id,
           rd32(hc->cap, 0) >> 24, (rd32(hc->cap, 0) >> 16) & 0xFF,
           (unsigned long long)phys, (unsigned long long)(size >> 10),
           hc->max_slots, hc->max_ports, hc->ctx_size, hc->ac64 ? "" : " (32-bit DMA)");

    /* Intel PCH: route the shared ports to xHCI instead of EHCI. */
    if (dev->vendor_id == 0x8086) {
        uint32_t ss_mask = pci_device_read(dev, 0xDC, 4);
        uint32_t hs_mask = pci_device_read(dev, 0xD4, 4);
        if (ss_mask || hs_mask) {
            pci_device_write(dev, 0xD8, 4, ss_mask);
            pci_device_write(dev, 0xD0, 4, hs_mask);
            serial_printf("xhci%d: Intel port routing USB3 0x%x USB2 0x%x\n", hc->index,
                          ss_mask, hs_mask);
        }
    }

    xhci_ext_caps(hc);
    serial_printf("xhci%d: halting\n", hc->index);

    /* Halt, then reset. */
    wr32(hc->op, OP_USBCMD, rd32(hc->op, OP_USBCMD) & ~CMD_RS);
    uint64_t dl = ktime_deadline_ms(200);
    while (!(rd32(hc->op, OP_USBSTS) & STS_HCH) && !ktime_expired(dl))
        __asm__ volatile("pause");
    wr32(hc->op, OP_USBCMD, CMD_HCRST);
    ktime_busywait_ms(2);
    dl = ktime_deadline_ms(1000);
    while ((rd32(hc->op, OP_USBCMD) & CMD_HCRST) || (rd32(hc->op, OP_USBSTS) & STS_CNR)) {
        if (ktime_expired(dl)) {
            printf("xhci%d: reset timed out\n", hc->index);
            return -1;
        }
        __asm__ volatile("pause");
    }
    if (!(rd32(hc->op, OP_PAGESIZE) & 1)) {
        printf("xhci%d: controller does not support 4 KiB pages\n", hc->index);
        return -1;
    }

    serial_printf("xhci%d: reset done\n", hc->index);
    wr32(hc->op, OP_CONFIG, hc->max_slots);
    wr32(hc->op, OP_DNCTRL, 0);

    hc->dcbaa = (uint64_t *)dma_alloc_pages(4096, &hc->dcbaa_phys);
    if (!hc->dcbaa) return -1;
    uint32_t nsp = (((hcs2 >> 21) & 0x1F) << 5) | ((hcs2 >> 27) & 0x1F);
    if (nsp) {
        uint64_t arr_phys;
        hc->sp_array = (uint64_t *)dma_alloc_pages(nsp * 8u, &arr_phys);
        if (!hc->sp_array) return -1;
        for (uint32_t i = 0; i < nsp; i++) {
            uint64_t pp;
            if (!dma_alloc_pages(4096, &pp)) return -1;
            hc->sp_array[i] = pp;
        }
        hc->dcbaa[0] = arr_phys;
        serial_printf("xhci%d: %u scratchpad buffers\n", hc->index, nsp);
    }
    wr64(hc->op, OP_DCBAAP, hc->dcbaa_phys);

    if (ring_init(&hc->cmd) != 0) return -1;
    wr64(hc->op, OP_CRCR, hc->cmd.phys | 1u);

    hc->evt = (xhci_trb_t *)dma_alloc_pages(XHCI_EVT_TRBS * sizeof(xhci_trb_t), &hc->evt_phys);
    uint64_t erst_phys;
    hc->erst = dma_alloc_pages(64, &erst_phys);
    if (!hc->evt || !hc->erst) return -1;
    ((uint64_t *)hc->erst)[0] = hc->evt_phys;
    ((uint32_t *)hc->erst)[2] = XHCI_EVT_TRBS;
    hc->evt_deq = 0;
    hc->evt_cycle = 1;
    wr32(hc->rt, RT_ERSTSZ, 1);
    wr64(hc->rt, RT_ERDP, hc->evt_phys);
    wr64(hc->rt, RT_ERSTBA, erst_phys);
    wr32(hc->rt, RT_IMOD, 0);
    wr32(hc->rt, RT_IMAN, 1);              /* clear IP, interrupts off (polled) */

    wr32(hc->op, OP_USBCMD, CMD_RS);
    dl = ktime_deadline_ms(200);
    while ((rd32(hc->op, OP_USBSTS) & STS_HCH) && !ktime_expired(dl))
        __asm__ volatile("pause");
    if (rd32(hc->op, OP_USBSTS) & STS_HCH) {
        printf("xhci%d: controller did not start\n", hc->index);
        return -1;
    }
    hc->initialized = true;

    /* Power the ports (controllers without PPC keep them powered). */
    bool powered = false;
    for (uint32_t p = 1; p <= hc->max_ports; p++) {
        uint32_t sc = rd32(hc->op, OP_PORTSC(p));
        if (!(sc & PORTSC_PP)) {
            wr32(hc->op, OP_PORTSC(p), (sc & PORTSC_KEEP) | PORTSC_PP);
            powered = true;
        }
    }
    if (powered) ktime_busywait_ms(20);
    serial_printf("xhci%d: running, no-op command\n", hc->index);
    /* A no-op command proves the command and event rings work. */
    int cc = xhci_command(hc, 0, 0, TRBF_TYPE(23), 500);
    if (cc != XHCI_CC_SUCCESS) {
        printf("xhci%d: no-op command failed (cc=%d)\n", hc->index, cc);
        hc->initialized = false;
        return -1;
    }
    return 0;
}

int xhci_init(void) {
    static bool done;
    if (done) return xhci_count;
    done = true;
    /* The HPET main counter is only started by hpet_set_periodic(), which
     * runs after USB init; every timeout here is measured on it. This
     * enables the counter (timer 0 interrupt stays off) and leaves it on. */
    extern void hpet_sleep_counter(uint64_t ms);
    hpet_sleep_counter(1);
    {
        uint64_t t0 = ktime_now_us();
        for (volatile int i = 0; i < 200000 && ktime_now_us() == t0; i++)
            __asm__ volatile("pause");
        if (ktime_now_us() == t0)
            printf("xhci: WARNING: system clock is not advancing; USB timeouts will hang\n");
    }
    for (int i = 0; i < pci_device_count && xhci_count < XHCI_MAX_CTRL; i++) {
        struct pci_device *d = pci_devices[i];
        if (d->class_code != 0x0C || d->subclass_code != 0x03 || d->prog_if != 0x30)
            continue;
        xhci_ctrl_t *hc = &xhci_ctrls[xhci_count];
        memset(hc, 0, sizeof(*hc));
        hc->index = xhci_count;
        usb_lock();
        int rc = xhci_init_one(hc, d);
        usb_unlock();
        if (rc != 0) {
            printf("xhci%d: %02x:%02x.%x not usable\n", hc->index, d->bus, d->device, d->function);
            memset(hc, 0, sizeof(*hc));
            continue;
        }
        xhci_count++;
    }
    if (!xhci_count)
        serial_printf("xhci: no usable controller\n");
    return xhci_count;
}

/* ---------------------------------------------------------------- ports */
bool xhci_port_connected(xhci_ctrl_t *hc, uint32_t port) {
    if (!hc || port < 1 || port > hc->max_ports) return false;
    return (rd32(hc->op, OP_PORTSC(port)) & PORTSC_CCS) != 0;
}

uint32_t xhci_port_status(xhci_ctrl_t *hc, uint32_t port) {
    if (!hc || port < 1 || port > hc->max_ports) return 0;
    return rd32(hc->op, OP_PORTSC(port));
}

void xhci_port_ack_changes(xhci_ctrl_t *hc, uint32_t port) {
    uint32_t sc = rd32(hc->op, OP_PORTSC(port));
    if (sc & PORTSC_CHANGES)
        wr32(hc->op, OP_PORTSC(port), (sc & PORTSC_KEEP) | (sc & PORTSC_CHANGES));
}

int xhci_port_reset(xhci_ctrl_t *hc, uint32_t port, uint8_t *speed_out) {
    if (!hc || port < 1 || port > hc->max_ports) return -1;
    uint32_t sc = rd32(hc->op, OP_PORTSC(port));
    if (!(sc & PORTSC_CCS)) return -1;
    uint8_t major = hc->port_major[port];
    if (!major) major = (((sc >> 10) & 0xF) >= USB_SPEED_SUPER) ? 3 : 2;
    if (major >= 3) {
        /* USB3 ports train on their own; warm reset only if not enabled. */
        uint64_t dl = ktime_deadline_ms(200);
        while (!(rd32(hc->op, OP_PORTSC(port)) & PORTSC_PED) && !ktime_expired(dl))
            ktime_busywait_ms(1);
        sc = rd32(hc->op, OP_PORTSC(port));
        if (!(sc & PORTSC_PED)) {
            wr32(hc->op, OP_PORTSC(port), (sc & PORTSC_KEEP) | PORTSC_WPR);
            dl = ktime_deadline_ms(500);
            while (!(rd32(hc->op, OP_PORTSC(port)) & (PORTSC_PRC | PORTSC_WRC)) &&
                   !ktime_expired(dl))
                ktime_busywait_ms(1);
        }
    } else {
        wr32(hc->op, OP_PORTSC(port), (sc & PORTSC_KEEP) | PORTSC_PR);
        uint64_t dl = ktime_deadline_ms(500);
        while (!(rd32(hc->op, OP_PORTSC(port)) & PORTSC_PRC) && !ktime_expired(dl))
            ktime_busywait_ms(1);
    }
    ktime_busywait_ms(10);                 /* TRSTRCY */
    xhci_port_ack_changes(hc, port);
    sc = rd32(hc->op, OP_PORTSC(port));
    if (!(sc & PORTSC_PED)) {
        serial_printf("xhci%d: port %u not enabled after reset (PORTSC 0x%x)\n",
                      hc->index, port, sc);
        return -1;
    }
    if (speed_out) *speed_out = (uint8_t)((sc >> 10) & 0xF);
    return 0;
}

/* ---------------------------------------------------------------- slots */
xhci_slot_t *xhci_alloc_slot(xhci_ctrl_t *hc) {
    int cc = xhci_command(hc, 0, 0, TRBF_TYPE(TRB_EN_SLOT), 1000);
    if (cc != XHCI_CC_SUCCESS) {
        serial_printf("xhci%d: enable slot failed cc=%d\n", hc->index, cc);
        return NULL;
    }
    uint8_t id = hc->cmd_slot;
    if (id == 0 || id > hc->max_slots) return NULL;
    xhci_slot_t *s = (xhci_slot_t *)kalloc(sizeof(xhci_slot_t));
    if (!s) return NULL;
    memset(s, 0, sizeof(*s));
    s->hc = hc;
    s->slot_id = id;
    s->out_ctx = dma_alloc_pages(4096, &s->out_ctx_phys);
    s->in_ctx = dma_alloc_pages(4096, &s->in_ctx_phys);
    s->ctrl_buf = dma_alloc_pages(4096, &s->ctrl_buf_phys);
    if (!s->out_ctx || !s->in_ctx || !s->ctrl_buf) {
        xhci_free_slot(s);
        return NULL;
    }
    hc->dcbaa[id] = s->out_ctx_phys;
    hc->slots[id] = s;
    s->in_use = true;
    return s;
}

void xhci_free_slot(xhci_slot_t *s) {
    if (!s) return;
    xhci_ctrl_t *hc = s->hc;
    if (s->in_use) {
        xhci_command(hc, 0, 0, TRBF_TYPE(TRB_DIS_SLOT) | ((uint32_t)s->slot_id << 24), 500);
        hc->dcbaa[s->slot_id] = 0;
        hc->slots[s->slot_id] = NULL;
    }
    for (int i = 0; i < 32; i++) ring_free(&s->ep[i].ring);
    if (s->out_ctx) dma_free_pages(s->out_ctx);
    if (s->in_ctx) dma_free_pages(s->in_ctx);
    if (s->ctrl_buf) dma_free_pages(s->ctrl_buf);
    kfree(s);
}

int xhci_address_device(xhci_slot_t *s, uint8_t root_port, uint32_t route,
                        uint8_t speed, uint8_t tt_slot, uint8_t tt_port,
                        uint16_t ep0_mps) {
    s->speed = speed;
    s->root_port = root_port;
    s->route = route;
    if (!s->ep[1].ring.trbs && ring_init(&s->ep[1].ring) != 0) return -1;
    s->ep[1].type = 4;
    s->ep[1].mps = ep0_mps;
    s->ep[1].active = true;
    in_clear(s);
    in_ctrl(s)[1] = 3;                                   /* A0 | A1 */
    uint32_t *sl = in_slot(s);
    sl[0] = (route & 0xFFFFF) | ((uint32_t)speed << 20) | (1u << 27);
    sl[1] = (uint32_t)root_port << 16;
    sl[2] = (uint32_t)tt_slot | ((uint32_t)tt_port << 8);
    uint32_t *e0 = in_ep(s, 1);
    e0[1] = (3u << 1) | (4u << 3) | ((uint32_t)ep0_mps << 16);
    uint64_t dq = ring_deq_ptr(&s->ep[1].ring);
    e0[2] = (uint32_t)dq;
    e0[3] = (uint32_t)(dq >> 32);
    e0[4] = 8;
    int cc = xhci_command(s->hc, s->in_ctx_phys, 0,
                          TRBF_TYPE(TRB_ADDR_DEV) | ((uint32_t)s->slot_id << 24), 2000);
    if (cc != XHCI_CC_SUCCESS) {
        serial_printf("xhci%d: address device slot %u failed cc=%d\n", s->hc->index,
                      s->slot_id, cc);
        return -1;
    }
    s->address = (uint8_t)(out_slot(s)[3] & 0xFF);
    return 0;
}

int xhci_update_ep0_mps(xhci_slot_t *s, uint16_t mps) {
    if (s->ep[1].mps == mps) return 0;
    in_clear(s);
    in_ctrl(s)[1] = 1u << 1;
    uint32_t *e0 = in_ep(s, 1);
    memcpy(e0, out_ep(s, 1), 20);
    e0[0] &= ~7u;
    e0[1] = (e0[1] & 0xFFFFu) | ((uint32_t)mps << 16);
    int cc = xhci_command(s->hc, s->in_ctx_phys, 0,
                          TRBF_TYPE(TRB_EVAL_CTX) | ((uint32_t)s->slot_id << 24), 1000);
    if (cc != XHCI_CC_SUCCESS) return -1;
    s->ep[1].mps = mps;
    return 0;
}

static uint32_t ctx_entries(xhci_slot_t *s) {
    return out_slot(s)[0] >> 27;
}

int xhci_set_hub(xhci_slot_t *s, uint8_t nports, bool mtt, uint8_t ttt) {
    in_clear(s);
    in_ctrl(s)[1] = 1;                                   /* slot only */
    uint32_t *sl = in_slot(s);
    memcpy(sl, out_slot(s), 16);
    sl[0] |= (1u << 26) | (mtt ? (1u << 25) : 0);
    sl[1] = (sl[1] & 0x00FFFFFFu) | ((uint32_t)nports << 24);
    sl[2] = (sl[2] & ~(3u << 16)) | ((uint32_t)(ttt & 3) << 16);
    sl[3] = 0;
    int cc = xhci_command(s->hc, s->in_ctx_phys, 0,
                          TRBF_TYPE(TRB_CFG_EP) | ((uint32_t)s->slot_id << 24), 1000);
    if (cc != XHCI_CC_SUCCESS) {
        serial_printf("xhci%d: hub slot %u configure failed cc=%d\n", s->hc->index,
                      s->slot_id, cc);
        return -1;
    }
    return 0;
}

static uint32_t ep_dci(uint8_t epaddr) {
    return ((uint32_t)(epaddr & 0x0F) << 1) | ((epaddr & 0x80) ? 1u : 0u);
}

int xhci_configure_endpoint(xhci_slot_t *s, uint8_t epaddr, uint8_t attr,
                            uint16_t mps, uint8_t interval, uint8_t max_burst) {
    uint32_t dci = ep_dci(epaddr);
    if (dci < 2 || dci > 31) return -1;
    bool in = (epaddr & 0x80) != 0;
    uint8_t xt = (attr & 3) == 2 ? (in ? 6 : 2) : (attr & 3) == 3 ? (in ? 7 : 3) : 0;
    if (!xt) return -1;
    xhci_ep_t *ep = &s->ep[dci];
    if (!ep->ring.trbs && ring_init(&ep->ring) != 0) return -1;
    ep->type = xt;
    ep->mps = mps;

    uint32_t ivl = 0;
    if ((attr & 3) == 3) {
        if (s->speed >= USB_SPEED_HIGH) {
            ivl = interval ? (uint32_t)interval - 1u : 0;
            if (ivl > 15) ivl = 15;
        } else {
            uint32_t frames = interval ? interval : 1;
            ivl = 3;
            while (ivl < 10 && (1u << ivl) < frames * 8u) ivl++;
        }
    }
    in_clear(s);
    in_ctrl(s)[1] = 1u | (1u << dci);
    uint32_t *sl = in_slot(s);
    memcpy(sl, out_slot(s), 16);
    uint32_t ents = ctx_entries(s);
    if (dci > ents) ents = dci;
    sl[0] = (sl[0] & 0x07FFFFFFu) | (ents << 27);
    sl[3] = 0;
    uint32_t *e = in_ep(s, dci);
    e[0] = ivl << 16;
    e[1] = (3u << 1) | ((uint32_t)xt << 3) | ((uint32_t)max_burst << 8) | ((uint32_t)mps << 16);
    uint64_t dq = ring_deq_ptr(&ep->ring);
    e[2] = (uint32_t)dq;
    e[3] = (uint32_t)(dq >> 32);
    uint32_t esit = (attr & 3) == 3 ? (uint32_t)mps * (max_burst + 1u) : 0;
    e[4] = ((attr & 3) == 3 ? mps : 3072u) | (esit << 16);
    int cc = xhci_command(s->hc, s->in_ctx_phys, 0,
                          TRBF_TYPE(TRB_CFG_EP) | ((uint32_t)s->slot_id << 24), 1000);
    if (cc != XHCI_CC_SUCCESS) {
        serial_printf("xhci%d: configure ep 0x%x slot %u failed cc=%d\n", s->hc->index,
                      epaddr, s->slot_id, cc);
        return -1;
    }
    ep->active = true;
    return 0;
}

/* Bring a halted/stopped endpoint back: Reset Endpoint (if halted) and move
 * the dequeue pointer to our enqueue position, dropping unfinished TRBs. */
static int ep_recover(xhci_slot_t *s, uint32_t dci) {
    uint32_t state = out_ep(s, dci)[0] & 7;
    uint32_t sid = (uint32_t)s->slot_id << 24, d = dci << 16;
    if (state == 2) {                      /* halted */
        xhci_command(s->hc, 0, 0, TRBF_TYPE(TRB_RESET_EP) | sid | d, 1000);
    } else if (state == 1) {               /* running: stop it first */
        xhci_command(s->hc, 0, 0, TRBF_TYPE(TRB_STOP_EP) | sid | d, 1000);
    }
    int cc = xhci_command(s->hc, ring_deq_ptr(&s->ep[dci].ring), 0,
                          TRBF_TYPE(TRB_SET_DEQ) | sid | d, 1000);
    s->ep[dci].done = false;
    return cc == XHCI_CC_SUCCESS ? 0 : -1;
}

int xhci_reset_endpoint(xhci_slot_t *s, uint8_t epaddr) {
    uint32_t dci = ep_dci(epaddr);
    if (dci > 31 || !s->ep[dci].active) return -1;
    return ep_recover(s, dci);
}

static int ep_wait(xhci_slot_t *s, uint32_t dci, uint32_t timeout_ms) {
    xhci_ep_t *ep = &s->ep[dci];
    uint64_t dl = ktime_deadline_ms(timeout_ms);
    for (;;) {
        xhci_process_events(s->hc);
        if (ep->done) return ep->cc;
        if (ktime_expired(dl)) return XHCI_CC_TIMEOUT;
        __asm__ volatile("pause");
    }
}

static int xfer_result(xhci_slot_t *s, uint32_t dci, int cc) {
    if (cc == XHCI_CC_SUCCESS || cc == XHCI_CC_SHORT_PACKET) return 0;
    ep_recover(s, dci);
    if (cc == XHCI_CC_TIMEOUT) return XHCI_ERR_TIMEOUT;
    return -cc;
}

/* --------------------------------------------------------------- control */
int xhci_control(xhci_slot_t *s, uint8_t rt, uint8_t req, uint16_t val,
                 uint16_t idx, uint16_t len, void *data, uint32_t timeout_ms) {
    if (!s || !s->ep[1].active || len > 4096) return -1;
    xhci_ep_t *ep = &s->ep[1];
    bool in = (rt & 0x80) != 0;
    if (len && !in && data) memcpy(s->ctrl_buf, data, len);
    if (len && in) memset(s->ctrl_buf, 0, len);
    uint64_t setup = (uint64_t)rt | ((uint64_t)req << 8) | ((uint64_t)val << 16) |
                     ((uint64_t)idx << 32) | ((uint64_t)len << 48);
    ep->done = false;
    ep->short_seen = false;
    ep->last_trb = ~0ull;
    uint32_t trt = len ? (in ? 3u : 2u) : 0u;
    ring_push(&ep->ring, setup, 8, TRBF_TYPE(TRB_SETUP) | TRBF_IDT | (trt << 16));
    if (len)
        ring_push(&ep->ring, s->ctrl_buf_phys, len,
                  TRBF_TYPE(TRB_DATA) | TRBF_ISP | (in ? (1u << 16) : 0));
    uint32_t sdir = (len && in) ? 0 : (1u << 16);
    ep->last_trb = ring_push(&ep->ring, 0, 0, TRBF_TYPE(TRB_STATUS) | TRBF_IOC | sdir);
    mb();
    s->hc->db[s->slot_id] = 1;
    int cc = ep_wait(s, 1, timeout_ms);
    int r = xfer_result(s, 1, cc);
    if (r < 0) return r;
    uint32_t got = len;
    if (ep->short_seen && ep->short_residual <= len) got = len - ep->short_residual;
    if (in && data && got) memcpy(data, s->ctrl_buf, got);
    return (int)got;
}

/* --------------------------------------------------------- bulk / interrupt */
static int queue_td(xhci_slot_t *s, uint32_t dci, void *buf, uint32_t len) {
    xhci_ep_t *ep = &s->ep[dci];
    uint64_t phys = virt_to_phys(buf);
    ep->done = false;
    ep->short_seen = false;
    ep->td_n = 0;
    ep->last_trb = ~0ull;
    uint32_t off = 0;
    if (len == 0) {
        ep->td_phys[0] = ep->last_trb =
            ring_push(&ep->ring, phys, 0, TRBF_TYPE(TRB_NORMAL) | TRBF_IOC);
        ep->td_len[0] = 0;
        ep->td_n = 1;
    }
    while (off < len) {
        uint64_t p = phys + off;
        uint32_t chunk = 0x10000u - (uint32_t)(p & 0xFFFFu);
        if (chunk > len - off) chunk = len - off;
        if (ep->td_n >= XHCI_TD_MAX) return -1;
        bool last = off + chunk >= len;
        uint32_t rem = len - off - chunk;
        uint32_t tds = last ? 0 : (rem + ep->mps - 1u) / (ep->mps ? ep->mps : 512u);
        if (tds > 31) tds = 31;
        uint32_t ctl = TRBF_TYPE(TRB_NORMAL) | TRBF_ISP | (last ? TRBF_IOC : TRBF_CH);
        uint64_t t = ring_push(&ep->ring, p, chunk | (tds << 17), ctl);
        ep->td_phys[ep->td_n] = t;
        ep->td_len[ep->td_n] = chunk;
        ep->td_n++;
        if (last) ep->last_trb = t;
        off += chunk;
    }
    ep->td_total = len;
    mb();
    s->hc->db[s->slot_id] = dci;
    return 0;
}

static uint32_t td_bytes(xhci_ep_t *ep) {
    if (!ep->short_seen) return ep->td_total - (ep->residual <= ep->td_total ? ep->residual : 0);
    uint32_t sum = 0;
    for (uint32_t k = 0; k < ep->td_n; k++) {
        if (ep->td_phys[k] == ep->short_trb) {
            uint32_t r = ep->short_residual <= ep->td_len[k] ? ep->short_residual : ep->td_len[k];
            return sum + ep->td_len[k] - r;
        }
        sum += ep->td_len[k];
    }
    return ep->td_total;
}

int xhci_bulk(xhci_slot_t *s, uint8_t epaddr, void *buf, uint32_t len, uint32_t timeout_ms) {
    uint32_t dci = ep_dci(epaddr);
    if (!s || dci > 31 || !s->ep[dci].active) return -1;
    if (queue_td(s, dci, buf, len) != 0) return -1;
    int cc = ep_wait(s, dci, timeout_ms);
    int r = xfer_result(s, dci, cc);
    if (r < 0) return r;
    return (int)td_bytes(&s->ep[dci]);
}

int xhci_int_queue(xhci_slot_t *s, uint8_t epaddr, void *buf, uint32_t len) {
    uint32_t dci = ep_dci(epaddr);
    if (!s || dci > 31 || !s->ep[dci].active) return -1;
    return queue_td(s, dci, buf, len);
}

int xhci_int_poll(xhci_slot_t *s, uint8_t epaddr, uint32_t *bytes, int *cc) {
    uint32_t dci = ep_dci(epaddr);
    if (!s || dci > 31 || !s->ep[dci].active) return -1;
    xhci_process_events(s->hc);
    xhci_ep_t *ep = &s->ep[dci];
    if (!ep->done) return 0;
    ep->done = false;
    if (cc) *cc = ep->cc;
    if (bytes) *bytes = td_bytes(ep);
    if (ep->cc != XHCI_CC_SUCCESS && ep->cc != XHCI_CC_SHORT_PACKET)
        ep_recover(s, dci);
    return 1;
}

/* ---------------------------------------------------------------- status */
void xhci_print_status(void) {
    if (!xhci_count) {
        printf("xHCI: no controller\n");
        return;
    }
    for (int i = 0; i < xhci_count; i++) {
        xhci_ctrl_t *hc = &xhci_ctrls[i];
        if (!hc->initialized) continue;
        printf("xhci%d: %02x:%02x.%x %04x:%04x USBSTS 0x%08x slots %u ports %u\n", i,
               hc->pci->bus, hc->pci->device, hc->pci->function, hc->pci->vendor_id,
               hc->pci->device_id, rd32(hc->op, OP_USBSTS), hc->max_slots, hc->max_ports);
        for (uint32_t p = 1; p <= hc->max_ports; p++) {
            uint32_t sc = rd32(hc->op, OP_PORTSC(p));
            printf("  port %2u USB%u %s%s speed %s PORTSC 0x%08x\n", p,
                   hc->port_major[p] ? hc->port_major[p] : 2,
                   (sc & PORTSC_CCS) ? "connected" : "empty",
                   (sc & PORTSC_PED) ? " enabled" : "",
                   (sc & PORTSC_CCS) ? xhci_speed_name((uint8_t)((sc >> 10) & 0xF)) : "-", sc);
        }
    }
}
