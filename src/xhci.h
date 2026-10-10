/**
 * @file xhci.h
 * @brief xHCI (USB 3.x) host controller driver.
 *
 * The driver owns the controllers, rings and device slots. Device-level logic
 * (descriptors, hubs, HID, mass storage) lives in usb_core.c / usb_hid.c /
 * usb_msc.c and talks to the host controller only through the functions below.
 */
#pragma once

#include "types.h"
#include "pci.h"

#define XHCI_MAX_CTRL      4
#define XHCI_MAX_SLOTS     64
#define XHCI_MAX_PORTS     32
#define XHCI_RING_TRBS     256          /* per ring, last one is the link TRB */
#define XHCI_EVT_TRBS      256
#define XHCI_TD_MAX        8            /* TRBs per TD: 64 KiB boundaries of a <=448 KiB buffer */

/* Completion codes */
#define XHCI_CC_SUCCESS        1
#define XHCI_CC_DATA_BUFFER    2
#define XHCI_CC_BABBLE         3
#define XHCI_CC_TRANSACTION    4
#define XHCI_CC_TRB            5
#define XHCI_CC_STALL          6
#define XHCI_CC_SHORT_PACKET   13
#define XHCI_CC_TIMEOUT        (-1)     /* driver-side timeout */

/* USB speeds as reported by PORTSC / slot context */
#define USB_SPEED_FULL   1
#define USB_SPEED_LOW    2
#define USB_SPEED_HIGH   3
#define USB_SPEED_SUPER  4
#define USB_SPEED_SUPERP 5

typedef struct __attribute__((packed)) {
    uint64_t param;
    uint32_t status;
    uint32_t control;
} xhci_trb_t;

/* A producer ring (command ring or one endpoint's transfer ring). */
typedef struct {
    xhci_trb_t *trbs;
    uint64_t phys;
    uint32_t enq;
    uint32_t cycle;
} xhci_ring_t;

/* Per-endpoint completion state filled in by the event dispatcher. */
typedef struct {
    xhci_ring_t ring;
    bool active;
    volatile bool done;          /* a completion for last_trb arrived */
    int cc;
    uint32_t residual;
    uint64_t last_trb;           /* phys of the TRB carrying IOC */
    uint8_t type;                /* xHCI EP type (2 bulk out, 6 bulk in, 7 int in, 4 control) */
    uint16_t mps;
    /* short packet inside a TD (ISP) */
    bool short_seen;
    uint64_t short_trb;
    uint32_t short_residual;
    /* TRBs of the last queued bulk/interrupt TD */
    uint32_t td_n;
    uint32_t td_total;
    uint64_t td_phys[XHCI_TD_MAX];
    uint32_t td_len[XHCI_TD_MAX];
} xhci_ep_t;

struct xhci_ctrl;

typedef struct xhci_slot {
    bool in_use;
    struct xhci_ctrl *hc;
    uint8_t slot_id;
    uint8_t speed;
    uint8_t root_port;           /* 1-based root hub port */
    uint32_t route;              /* route string */
    uint8_t address;             /* USB address assigned by the controller */
    void *ctrl_buf;              /* 4 KiB DMA buffer for control transfers */
    uint64_t ctrl_buf_phys;
    void *out_ctx;               /* device (output) context */
    uint64_t out_ctx_phys;
    void *in_ctx;                /* input context, reused for every command */
    uint64_t in_ctx_phys;
    xhci_ep_t ep[32];            /* index = DCI */
} xhci_slot_t;

typedef struct xhci_ctrl {
    bool initialized;
    int index;
    struct pci_device *pci;
    volatile uint8_t *cap;
    volatile uint8_t *op;
    volatile uint8_t *rt;
    volatile uint32_t *db;
    uint32_t max_slots;
    uint32_t max_ports;
    uint32_t ctx_size;           /* 32 or 64 */
    bool ac64;
    uint64_t *dcbaa;
    uint64_t dcbaa_phys;
    uint64_t *sp_array;
    xhci_ring_t cmd;
    xhci_trb_t *evt;
    uint64_t evt_phys;
    uint32_t evt_deq;
    uint32_t evt_cycle;
    void *erst;
    /* command completion */
    volatile bool cmd_done;
    int cmd_cc;
    uint8_t cmd_slot;
    uint64_t cmd_trb;
    /* root ports */
    uint8_t port_major[XHCI_MAX_PORTS + 1];   /* 2 or 3 from protocol caps */
    uint8_t port_slot[XHCI_MAX_PORTS + 1];
    volatile uint32_t port_changed;            /* bitmask from PSC events */
    xhci_slot_t *slots[XHCI_MAX_SLOTS + 1];
} xhci_ctrl_t;

/* Recursive (per CPU) lock around USB command/transfer sequences. */
void usb_lock(void);
void usb_unlock(void);
bool usb_trylock(void);

/* Controller bring-up (all xHCI controllers on PCI). Returns the count. */
int xhci_init(void);
int xhci_controller_count(void);
xhci_ctrl_t *xhci_get_controller(int i);

/* Event processing; called from waits and from the periodic USB poll. */
void xhci_process_events(xhci_ctrl_t *hc);

/* Root ports */
bool xhci_port_connected(xhci_ctrl_t *hc, uint32_t port);
int xhci_port_reset(xhci_ctrl_t *hc, uint32_t port, uint8_t *speed_out);
void xhci_port_ack_changes(xhci_ctrl_t *hc, uint32_t port);
uint32_t xhci_port_status(xhci_ctrl_t *hc, uint32_t port);
const char *xhci_speed_name(uint8_t speed);

/* Slots */
xhci_slot_t *xhci_alloc_slot(xhci_ctrl_t *hc);
void xhci_free_slot(xhci_slot_t *s);
/* Address a device (BSR=0). tt_slot/tt_port describe the transaction
 * translator for LS/FS devices behind a HS hub (0 = none). */
int xhci_address_device(xhci_slot_t *s, uint8_t root_port, uint32_t route,
                        uint8_t speed, uint8_t tt_slot, uint8_t tt_port,
                        uint16_t ep0_mps);
int xhci_update_ep0_mps(xhci_slot_t *s, uint16_t mps);
/* Mark the slot as a hub (number of ports, multi-TT, think time). */
int xhci_set_hub(xhci_slot_t *s, uint8_t nports, bool mtt, uint8_t ttt);
/* Add an endpoint (epaddr with bit 7 = IN, attr 2 bulk / 3 interrupt). */
int xhci_configure_endpoint(xhci_slot_t *s, uint8_t epaddr, uint8_t attr,
                            uint16_t mps, uint8_t interval, uint8_t max_burst);
int xhci_reset_endpoint(xhci_slot_t *s, uint8_t epaddr);

/* Transfers. Return bytes transferred (>= 0) or a negative value:
 * -XHCI_CC_STALL for a stall, -1000 for a timeout, -cc otherwise. */
#define XHCI_ERR_TIMEOUT (-1000)
int xhci_control(xhci_slot_t *s, uint8_t bmRequestType, uint8_t bRequest,
                 uint16_t wValue, uint16_t wIndex, uint16_t wLength,
                 void *data, uint32_t timeout_ms);
int xhci_bulk(xhci_slot_t *s, uint8_t epaddr, void *buf, uint32_t len,
              uint32_t timeout_ms);
/* Interrupt IN: queue one transfer, then poll for its completion. */
int xhci_int_queue(xhci_slot_t *s, uint8_t epaddr, void *buf, uint32_t len);
/* 1 = completed (result in *bytes, *cc), 0 = still pending. */
int xhci_int_poll(xhci_slot_t *s, uint8_t epaddr, uint32_t *bytes, int *cc);

void xhci_print_status(void);
