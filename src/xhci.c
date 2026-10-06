/**
 * @file xhci.c
 * @brief xHCI (USB 3.0) Host Controller Driver implementation
 *
 * Implements the eXtensible Host Controller Interface for USB devices.
 * Supports controller initialization, port management, and control transfers.
 */

#include "xhci.h"
#include "pci.h"
#include "mem.h"
#include "serial.h"
#include "utils.h"
#include "hpet.h"
#include "string.h"

/** @brief Global xHCI state */
static xhci_state_t xhci = {0};

/* ============================================================================
 * Helper: wait for a register bit to clear, with timeout
 * ============================================================================ */
static int xhci_wait_bits_clear(volatile uint32_t *reg, uint32_t bits, uint32_t timeout_ms) {
    for (uint32_t i = 0; i < timeout_ms; i++) {
        if ((*reg & bits) == 0) return 0;
        hpet_sleep_counter(1);
    }
    return -1;
}

static int xhci_wait_bits_set(volatile uint32_t *reg, uint32_t bits, uint32_t timeout_ms) {
    for (uint32_t i = 0; i < timeout_ms; i++) {
        if ((*reg & bits) == bits) return 0;
        hpet_sleep_counter(1);
    }
    return -1;
}

/* ============================================================================
 * Command and Event Ring management
 * ============================================================================ */

/**
 * @brief Enqueue a TRB on the command ring and ring the doorbell
 */
static void xhci_cmd_ring_push(xhci_trb_t *trb) {
    uint32_t idx = xhci.cmd_ring_enqueue;

    /* Copy the TRB, setting the cycle bit */
    xhci.cmd_ring[idx].parameter = trb->parameter;
    xhci.cmd_ring[idx].status    = trb->status;

    /* Ensure the cycle bit matches our current producer cycle */
    uint32_t ctrl = trb->control;
    if (xhci.cmd_ring_cycle)
        ctrl |= TRB_CYCLE;
    else
        ctrl &= ~TRB_CYCLE;
    xhci.cmd_ring[idx].control = ctrl;

    /* Memory barrier */
    __asm__ volatile("mfence" ::: "memory");

    /* Advance enqueue pointer */
    xhci.cmd_ring_enqueue++;
    if (xhci.cmd_ring_enqueue >= XHCI_CMD_RING_SIZE - 1) {
        /* Wrap: write a Link TRB back to the start */
        xhci.cmd_ring[xhci.cmd_ring_enqueue].parameter = xhci.cmd_ring_phys;
        xhci.cmd_ring[xhci.cmd_ring_enqueue].status = 0;
        uint32_t link_ctrl = TRB_TYPE_LINK | TRB_TC;
        if (xhci.cmd_ring_cycle)
            link_ctrl |= TRB_CYCLE;
        else
            link_ctrl &= ~TRB_CYCLE;
        xhci.cmd_ring[xhci.cmd_ring_enqueue].control = link_ctrl;

        xhci.cmd_ring_enqueue = 0;
        xhci.cmd_ring_cycle ^= 1;  /* Toggle cycle */
    }

    /* Ring the Host Controller doorbell (doorbell 0 = command ring) */
    xhci.doorbells[0] = 0;
}

/**
 * @brief Wait for a command completion event
 * @return Completion code, or -1 on timeout
 */
static int xhci_wait_event(uint32_t timeout_ms) {
    for (uint32_t t = 0; t < timeout_ms; t++) {
        uint32_t idx = xhci.evt_ring_dequeue;
        xhci_trb_t *evt = &xhci.evt_ring[idx];

        /* Check if this event is valid (cycle bit matches) */
        bool evt_cycle = (evt->control & TRB_CYCLE) != 0;
        if (evt_cycle == (bool)xhci.evt_ring_cycle) {
            /* Valid event - consume it */
            uint32_t cc = (evt->status >> 24) & 0xFF;  /* Completion code */

            /* Advance dequeue */
            xhci.evt_ring_dequeue++;
            if (xhci.evt_ring_dequeue >= XHCI_EVT_RING_SIZE) {
                xhci.evt_ring_dequeue = 0;
                xhci.evt_ring_cycle ^= 1;
            }

            /* Update ERDP (Event Ring Dequeue Pointer) in runtime registers.
             * Interrupter 0 ERDP is at runtime_base + 0x20 + 0x38 */
            volatile uint64_t *erdp = (volatile uint64_t *)(
                (uint8_t *)xhci.runtime + 0x20 + 0x38);
            uint64_t new_erdp = xhci.evt_ring_phys +
                                xhci.evt_ring_dequeue * sizeof(xhci_trb_t);
            *erdp = new_erdp | (1 << 3);  /* Set EHB to clear busy */

            return (int)cc;
        }

        hpet_sleep_counter(1);
    }

    serial_puts("xhci: event timeout\n");
    return -1;
}

/* ============================================================================
 * Initialization
 * ============================================================================ */

int xhci_init(void) {
    serial_puts("xhci: initializing xHCI host controller\n");

    /* Find xHCI PCI device (class 0x0C, subclass 0x03, prog_if 0x30) */
    struct pci_device *dev = NULL;
    for (int i = 0; i < pci_device_count; i++) {
        if (pci_devices[i]->class_code == 0x0C &&
            pci_devices[i]->subclass_code == 0x03 &&
            pci_devices[i]->prog_if == 0x30) {
            dev = pci_devices[i];
            break;
        }
    }

    if (!dev) {
        serial_puts("xhci: no xHCI controller found\n");
        printf("xHCI: No controller found\n");
        return -1;
    }

    xhci.pci_dev = dev;
    serial_printf("xhci: found controller vendor=0x%x device=0x%x\n",
                  dev->vendor_id, dev->device_id);

    /* Enable PCI bus mastering and memory space */
    uint32_t cmd = pci_device_read(dev, 0x04, 2);
    cmd |= (1 << 1) | (1 << 2);  /* Memory space + Bus master */
    pci_device_write(dev, 0x04, 2, cmd);

    /* Read BAR0 (64-bit MMIO) */
    uint32_t bar0_lo = pci_device_read(dev, 0x10, 4);
    uint32_t bar0_hi = pci_device_read(dev, 0x14, 4);
    uint64_t bar0_phys = (bar0_lo & 0xFFFFFFF0ULL) | ((uint64_t)bar0_hi << 32);

    /* Map to virtual address */
    uint64_t bar0_virt = bar0_phys + vmm_higher_half_offset;

    serial_printf("xhci: BAR0 phys=0x%llx virt=0x%llx\n",
                  (unsigned long long)bar0_phys, (unsigned long long)bar0_virt);

    /* Set up register pointers */
    xhci.cap = (volatile xhci_cap_regs_t *)bar0_virt;
    uint8_t caplength = xhci.cap->caplength;

    xhci.op = (volatile xhci_op_regs_t *)(bar0_virt + caplength);
    xhci.doorbells = (volatile uint32_t *)(bar0_virt + xhci.cap->dboff);
    xhci.runtime = (volatile uint32_t *)(bar0_virt + xhci.cap->rtsoff);

    /* Parse HCS parameters */
    xhci.max_slots = (xhci.cap->hcsparams1 >> 0) & 0xFF;
    xhci.max_intrs = (xhci.cap->hcsparams1 >> 8) & 0x7FF;
    xhci.max_ports = (xhci.cap->hcsparams1 >> 24) & 0xFF;
    xhci.page_size = xhci.op->pagesize << 12;

    /* Port registers start at op_base + 0x400 */
    xhci.ports = (volatile xhci_port_regs_t *)(
        (uint8_t *)xhci.op + 0x400);

    if (xhci.max_slots > XHCI_MAX_SLOTS) xhci.max_slots = XHCI_MAX_SLOTS;
    if (xhci.max_ports > XHCI_MAX_PORTS) xhci.max_ports = XHCI_MAX_PORTS;

    serial_printf("xhci: version=0x%x slots=%d ports=%d intrs=%d pagesize=%d\n",
                  xhci.cap->hci_version, xhci.max_slots, xhci.max_ports,
                  xhci.max_intrs, xhci.page_size);

    /* usbcmd/usbsts live at offsets 0x00/0x04 of the (packed) op regs; both
     * are naturally 4-byte aligned, so taking their address via byte offsets
     * is safe and avoids -Waddress-of-packed-member. */
    volatile uint32_t *usbcmd_reg = (volatile uint32_t *)((uint8_t *)xhci.op + 0x00);
    volatile uint32_t *usbsts_reg = (volatile uint32_t *)((uint8_t *)xhci.op + 0x04);

    /* === Reset the controller === */
    /* Stop */
    xhci.op->usbcmd &= ~XHCI_CMD_RUN;
    if (xhci_wait_bits_set(usbsts_reg, XHCI_STS_HCH, 100) != 0) {
        serial_puts("xhci: controller did not halt\n");
        return -1;
    }

    /* Reset */
    xhci.op->usbcmd |= XHCI_CMD_HCRST;
    if (xhci_wait_bits_clear(usbcmd_reg, XHCI_CMD_HCRST, 500) != 0) {
        serial_puts("xhci: reset timeout\n");
        return -1;
    }
    /* Wait for CNR to clear */
    if (xhci_wait_bits_clear(usbsts_reg, XHCI_STS_CNR, 500) != 0) {
        serial_puts("xhci: controller not ready after reset\n");
        return -1;
    }

    serial_puts("xhci: controller reset complete\n");

    /* === Set max device slots === */
    xhci.op->config = xhci.max_slots;

    /* === Allocate DCBAA (Device Context Base Address Array) === */
    size_t dcbaa_size = (xhci.max_slots + 1) * sizeof(uint64_t);
    xhci.dcbaa = (uint64_t *)kalloc_high(dcbaa_size);
    if (!xhci.dcbaa) {
        serial_puts("xhci: failed to allocate DCBAA\n");
        return -1;
    }
    memset(xhci.dcbaa, 0, dcbaa_size);
    xhci.dcbaa_phys = virt_to_phys(xhci.dcbaa);
    xhci.op->dcbaap = xhci.dcbaa_phys;

    /* === Allocate Command Ring === */
    size_t ring_size = XHCI_CMD_RING_SIZE * sizeof(xhci_trb_t);
    xhci.cmd_ring = (xhci_trb_t *)kalloc_high(ring_size);
    if (!xhci.cmd_ring) {
        serial_puts("xhci: failed to allocate command ring\n");
        return -1;
    }
    memset(xhci.cmd_ring, 0, ring_size);
    xhci.cmd_ring_phys = virt_to_phys(xhci.cmd_ring);
    xhci.cmd_ring_enqueue = 0;
    xhci.cmd_ring_cycle = 1;

    /* Set Command Ring Control Register (CRCR) */
    xhci.op->crcr = xhci.cmd_ring_phys | 1;  /* RCS = 1 */

    /* === Allocate Event Ring === */
    xhci.evt_ring = (xhci_trb_t *)kalloc_high(
        XHCI_EVT_RING_SIZE * sizeof(xhci_trb_t));
    if (!xhci.evt_ring) {
        serial_puts("xhci: failed to allocate event ring\n");
        return -1;
    }
    memset(xhci.evt_ring, 0, XHCI_EVT_RING_SIZE * sizeof(xhci_trb_t));
    xhci.evt_ring_phys = virt_to_phys(xhci.evt_ring);
    xhci.evt_ring_dequeue = 0;
    xhci.evt_ring_cycle = 1;

    /* === Set up Event Ring Segment Table (ERST) === */
    /* Each ERST entry is 16 bytes: base address (8) + size (4) + reserved (4) */
    xhci.erst = kalloc_high(16);
    if (!xhci.erst) {
        serial_puts("xhci: failed to allocate ERST\n");
        return -1;
    }
    memset(xhci.erst, 0, 16);
    xhci.erst_phys = virt_to_phys(xhci.erst);

    uint64_t *erst_entry = (uint64_t *)xhci.erst;
    erst_entry[0] = xhci.evt_ring_phys;            /* Segment base */
    ((uint32_t *)xhci.erst)[2] = XHCI_EVT_RING_SIZE; /* Segment size */

    /* Configure Interrupter 0 (at runtime + 0x20) */
    volatile uint32_t *intr0 = (volatile uint32_t *)(
        (uint8_t *)xhci.runtime + 0x20);
    
    /* ERSTSZ (offset 0x28 from interrupter base) */
    intr0[2] = 1;  /* One segment */

    /* ERDP (offset 0x38 from interrupter base, 64-bit) */
    volatile uint64_t *erdp = (volatile uint64_t *)&intr0[14];
    *erdp = xhci.evt_ring_phys;

    /* ERSTBA (offset 0x30 from interrupter base, 64-bit) */
    volatile uint64_t *erstba = (volatile uint64_t *)&intr0[12];
    *erstba = xhci.erst_phys;

    /* Enable interrupter */
    intr0[0] |= (1 << 1);  /* IE bit in IMAN */

    /* === Start the controller === */
    xhci.op->usbcmd |= XHCI_CMD_RUN | XHCI_CMD_INTE;

    if (xhci_wait_bits_clear(usbsts_reg, XHCI_STS_HCH, 100) != 0) {
        serial_puts("xhci: controller did not start\n");
        return -1;
    }

    xhci.initialized = true;

    /* Scan ports for connected devices */
    uint32_t connected = 0;
    for (uint32_t p = 1; p <= xhci.max_ports; p++) {
        uint32_t portsc = xhci.ports[p - 1].portsc;
        if (portsc & XHCI_PORTSC_CCS) {
            uint8_t speed = (portsc & XHCI_PORTSC_SPEED_MASK) >> XHCI_PORTSC_SPEED_SHIFT;
            xhci.port_speed[p] = speed;
            connected++;

            const char *speed_str = "unknown";
            switch (speed) {
                case XHCI_SPEED_FULL:  speed_str = "Full (12Mbps)"; break;
                case XHCI_SPEED_LOW:   speed_str = "Low (1.5Mbps)"; break;
                case XHCI_SPEED_HIGH:  speed_str = "High (480Mbps)"; break;
                case XHCI_SPEED_SUPER: speed_str = "Super (5Gbps)"; break;
            }
            serial_printf("xhci: port %d connected at %s speed\n", p, speed_str);
        }
    }

    printf("xHCI: Initialized (%d ports, %d devices connected)\n",
           xhci.max_ports, connected);

    return 0;
}

bool xhci_is_available(void) {
    return xhci.initialized;
}

uint32_t xhci_get_port_count(void) {
    return xhci.initialized ? xhci.max_ports : 0;
}

bool xhci_port_connected(uint8_t port) {
    if (!xhci.initialized || port == 0 || port > xhci.max_ports) return false;
    return (xhci.ports[port - 1].portsc & XHCI_PORTSC_CCS) != 0;
}

uint8_t xhci_port_speed(uint8_t port) {
    if (!xhci.initialized || port == 0 || port > xhci.max_ports) return 0;
    return xhci.port_speed[port];
}

int xhci_port_reset(uint8_t port) {
    if (!xhci.initialized || port == 0 || port > xhci.max_ports) return -1;

    volatile xhci_port_regs_t *pr = &xhci.ports[port - 1];

    /* Check if device is connected */
    if (!(pr->portsc & XHCI_PORTSC_CCS)) {
        serial_printf("xhci: port %d: no device connected\n", port);
        return -1;
    }

    /* Issue port reset */
    uint32_t portsc = pr->portsc;
    portsc &= ~(XHCI_PORTSC_PED);  /* Don't clear PED */
    portsc |= XHCI_PORTSC_PR;       /* Set reset bit */
    /* Write-1-to-clear bits: clear any pending change bits */
    portsc |= XHCI_PORTSC_CSC | XHCI_PORTSC_PEC | XHCI_PORTSC_PRC;
    pr->portsc = portsc;

    /* Wait for reset to complete (PRC bit set) */
    for (int t = 0; t < 500; t++) {
        if (pr->portsc & XHCI_PORTSC_PRC) {
            /* Clear the change bit */
            pr->portsc = pr->portsc | XHCI_PORTSC_PRC;
            break;
        }
        hpet_sleep_counter(1);
    }

    /* Check if port is now enabled */
    if (!(pr->portsc & XHCI_PORTSC_PED)) {
        serial_printf("xhci: port %d: reset failed, port not enabled\n", port);
        return -1;
    }

    /* Update speed */
    xhci.port_speed[port] = (pr->portsc & XHCI_PORTSC_SPEED_MASK)
                             >> XHCI_PORTSC_SPEED_SHIFT;

    serial_printf("xhci: port %d: reset complete, speed=%d\n",
                  port, xhci.port_speed[port]);
    return 0;
}

/* ============================================================================
 * Control transfers
 * ============================================================================ */

int xhci_control_transfer(uint8_t slot, void *setup, void *data,
                           uint16_t data_len, bool dir_in) {
    if (!xhci.initialized || !setup) return -1;

    /* For a basic control transfer, we use the command ring
     * with Setup, optional Data, and Status TRBs.
     * In a full implementation, these would go on the device's
     * transfer ring for endpoint 0, but for initial enumeration
     * we use the command ring with evaluate context commands. */

    /* This is a simplified implementation that logs the transfer.
     * Full xHCI control transfers require:
     * 1. Enable Slot command
     * 2. Address Device command
     * 3. Setup TRB + Data TRB + Status TRB on the transfer ring */

    serial_printf("xhci: control transfer slot=%d len=%d dir=%s\n",
                  slot, data_len, dir_in ? "IN" : "OUT");

    /* For now, use the No-Op command to verify the command ring works */
    if (slot == 0 && data_len == 0) {
        xhci_trb_t noop = {0};
        noop.control = TRB_TYPE_NOOP_CMD;
        xhci_cmd_ring_push(&noop);

        int cc = xhci_wait_event(100);
        if (cc == TRB_CC_SUCCESS) {
            serial_puts("xhci: no-op command completed successfully\n");
            return 0;
        }
        serial_printf("xhci: no-op command failed (cc=%d)\n", cc);
        return -1;
    }

    return 0;
}

/* ============================================================================
 * Device Context and Input Context Allocation
 * ============================================================================ */

/* xHCI Slot Context (32 bytes) */
typedef struct __attribute__((packed)) {
    uint32_t route_string : 20;
    uint32_t speed : 4;
    uint32_t reserved1 : 1;
    uint32_t mtt : 1;
    uint32_t hub : 1;
    uint32_t context_entries : 5;
    uint16_t max_exit_latency;
    uint8_t  root_hub_port;
    uint8_t  num_ports;
    uint32_t dword3;
    uint32_t dword4;
    uint32_t reserved2[4];
} xhci_slot_ctx_t;

/* xHCI Endpoint Context (32 bytes) */
typedef struct __attribute__((packed)) {
    uint32_t ep_state : 3;
    uint32_t reserved1 : 5;
    uint32_t mult : 2;
    uint32_t max_pstreams : 5;
    uint32_t lsa : 1;
    uint32_t interval : 8;
    uint32_t max_esit_hi : 8;
    uint32_t reserved2 : 1;
    uint32_t cerr : 2;
    uint32_t ep_type : 3;
    uint32_t reserved3 : 1;
    uint32_t hid : 1;
    uint32_t max_burst : 8;
    uint32_t max_packet_size : 16;
    uint64_t tr_dequeue;        /* Transfer ring dequeue pointer + DCS */
    uint32_t avg_trb_len : 16;
    uint32_t max_esit_lo : 16;
    uint32_t reserved4[3];
} xhci_ep_ctx_t;

/* EP types for endpoint context */
#define EP_TYPE_CONTROL_BI  4
#define EP_TYPE_ISOCH_OUT   1
#define EP_TYPE_BULK_OUT    2
#define EP_TYPE_INT_OUT     3
#define EP_TYPE_ISOCH_IN    5
#define EP_TYPE_BULK_IN     6
#define EP_TYPE_INT_IN      7

/* Input Control Context */
typedef struct __attribute__((packed)) {
    uint32_t drop_flags;
    uint32_t add_flags;
    uint32_t reserved[6];
} xhci_input_ctrl_ctx_t;

/* Per-device tracking */
typedef struct {
    bool in_use;
    uint8_t port;
    uint8_t speed;
    uint8_t slot_id;
    bool is_hid_keyboard;
    bool is_hid_mouse;

    /* Device context (allocated, phys in DCBAA) */
    void *dev_ctx;
    uint64_t dev_ctx_phys;

    /* EP0 transfer ring */
    xhci_trb_t *ep0_ring;
    uint64_t ep0_ring_phys;
    uint32_t ep0_enqueue;
    uint8_t  ep0_cycle;

    /* Interrupt endpoint transfer ring (for HID) */
    xhci_trb_t *int_ring;
    uint64_t int_ring_phys;
    uint32_t int_enqueue;
    uint8_t  int_cycle;
    uint8_t  int_ep_dci;  /* Device context index for interrupt EP */

    /* HID report buffer */
    uint8_t  *report_buf;
    uint64_t report_buf_phys;
    uint16_t report_size;
} xhci_device_t;

static xhci_device_t xhci_devices[XHCI_MAX_SLOTS + 1];  /* 1-indexed */

/**
 * @brief Send Enable Slot command
 * @return Slot ID on success, -1 on failure
 */
static int xhci_enable_slot(void) {
    xhci_trb_t trb = {0};
    trb.control = TRB_TYPE_ENABLE_SLOT;
    xhci_cmd_ring_push(&trb);

    int cc = xhci_wait_event(500);
    if (cc != TRB_CC_SUCCESS) {
        serial_printf("xhci: enable slot failed (cc=%d)\n", cc);
        return -1;
    }

    /* The slot ID is in the event TRB.
     * After xhci_wait_event, we need to read it from the consumed event.
     * Since our event processing advances the dequeue, read from dequeue-1. */
    uint32_t prev_idx = (xhci.evt_ring_dequeue == 0)
        ? XHCI_EVT_RING_SIZE - 1
        : xhci.evt_ring_dequeue - 1;
    uint32_t slot_id = (xhci.evt_ring[prev_idx].control >> 24) & 0xFF;

    if (slot_id == 0 || slot_id > xhci.max_slots) {
        serial_printf("xhci: invalid slot ID %d\n", slot_id);
        return -1;
    }

    serial_printf("xhci: enabled slot %d\n", slot_id);
    return (int)slot_id;
}

/**
 * @brief Allocate and set up device contexts for a slot
 */
static int xhci_setup_device_context(uint8_t slot_id, uint8_t port, uint8_t speed) {
    xhci_device_t *dev = &xhci_devices[slot_id];
    dev->slot_id = slot_id;
    dev->port = port;
    dev->speed = speed;
    dev->in_use = true;

    /* Allocate device context (slot + 31 endpoints = 32 contexts x 32 bytes = 1024B) */
    dev->dev_ctx = kalloc_high(1024);
    if (!dev->dev_ctx) return -1;
    memset(dev->dev_ctx, 0, 1024);
    dev->dev_ctx_phys = virt_to_phys(dev->dev_ctx);

    /* Store in DCBAA */
    xhci.dcbaa[slot_id] = dev->dev_ctx_phys;

    /* Allocate EP0 transfer ring */
    size_t ring_bytes = XHCI_TRANSFER_RING_SIZE * sizeof(xhci_trb_t);
    dev->ep0_ring = (xhci_trb_t *)kalloc_high(ring_bytes);
    if (!dev->ep0_ring) return -1;
    memset(dev->ep0_ring, 0, ring_bytes);
    dev->ep0_ring_phys = virt_to_phys(dev->ep0_ring);
    dev->ep0_enqueue = 0;
    dev->ep0_cycle = 1;

    return 0;
}

/**
 * @brief Send Address Device command
 */
static int xhci_address_device(uint8_t slot_id, uint8_t port, uint8_t speed) {
    xhci_device_t *dev = &xhci_devices[slot_id];

    /* Allocate Input Context (same structure as device context + input control ctx) */
    void *input_ctx = kalloc_high(2048);
    if (!input_ctx) return -1;
    memset(input_ctx, 0, 2048);
    uint64_t input_ctx_phys = virt_to_phys(input_ctx);

    /* Input Control Context (at offset 0) */
    xhci_input_ctrl_ctx_t *input_ctrl = (xhci_input_ctrl_ctx_t *)input_ctx;
    input_ctrl->add_flags = (1 << 0) | (1 << 1);  /* Add Slot + EP0 */

    /* Slot Context (at offset 32) */
    xhci_slot_ctx_t *slot_ctx = (xhci_slot_ctx_t *)((uint8_t *)input_ctx + 32);
    slot_ctx->route_string = 0;
    slot_ctx->speed = speed;
    slot_ctx->context_entries = 1;  /* Only EP0 */
    slot_ctx->root_hub_port = port;

    /* EP0 Context (at offset 64) - endpoint context index 1 */
    xhci_ep_ctx_t *ep0_ctx = (xhci_ep_ctx_t *)((uint8_t *)input_ctx + 64);
    ep0_ctx->ep_type = EP_TYPE_CONTROL_BI;
    ep0_ctx->cerr = 3;  /* Error count = 3 retries */
    ep0_ctx->tr_dequeue = dev->ep0_ring_phys | 1;  /* DCS = 1 */
    ep0_ctx->avg_trb_len = 8;  /* Average TRB length */

    /* Max packet size depends on speed */
    switch (speed) {
    case XHCI_SPEED_LOW:
    case XHCI_SPEED_FULL:
        ep0_ctx->max_packet_size = 8;
        break;
    case XHCI_SPEED_HIGH:
        ep0_ctx->max_packet_size = 64;
        break;
    case XHCI_SPEED_SUPER:
        ep0_ctx->max_packet_size = 512;
        break;
    default:
        ep0_ctx->max_packet_size = 64;
    }

    /* Send Address Device command */
    xhci_trb_t trb = {0};
    trb.parameter = input_ctx_phys;
    trb.control = TRB_TYPE_ADDRESS_DEV | ((uint32_t)slot_id << 24);
    xhci_cmd_ring_push(&trb);

    int cc = xhci_wait_event(500);
    kfree(input_ctx);

    if (cc != TRB_CC_SUCCESS) {
        serial_printf("xhci: address device failed for slot %d (cc=%d)\n",
                      slot_id, cc);
        return -1;
    }

    serial_printf("xhci: device addressed on slot %d\n", slot_id);
    return 0;
}

/**
 * @brief Push a TRB onto a device's EP0 transfer ring and ring the doorbell
 */
static void xhci_ep0_push(xhci_device_t *dev, xhci_trb_t *trb) {
    uint32_t idx = dev->ep0_enqueue;

    dev->ep0_ring[idx].parameter = trb->parameter;
    dev->ep0_ring[idx].status = trb->status;

    uint32_t ctrl = trb->control;
    if (dev->ep0_cycle)
        ctrl |= TRB_CYCLE;
    else
        ctrl &= ~TRB_CYCLE;
    dev->ep0_ring[idx].control = ctrl;

    __asm__ volatile("mfence" ::: "memory");

    dev->ep0_enqueue++;
    if (dev->ep0_enqueue >= XHCI_TRANSFER_RING_SIZE - 1) {
        /* Link TRB */
        dev->ep0_ring[dev->ep0_enqueue].parameter = dev->ep0_ring_phys;
        dev->ep0_ring[dev->ep0_enqueue].status = 0;
        uint32_t link_ctrl = TRB_TYPE_LINK | TRB_TC;
        if (dev->ep0_cycle) link_ctrl |= TRB_CYCLE;
        dev->ep0_ring[dev->ep0_enqueue].control = link_ctrl;
        dev->ep0_enqueue = 0;
        dev->ep0_cycle ^= 1;
    }
}

/**
 * @brief Perform a USB control transfer using the device's EP0
 */
static int xhci_dev_control_transfer(uint8_t slot_id, xhci_setup_packet_t *setup,
                                      void *data, uint16_t data_len, bool dir_in) {
    if (slot_id == 0 || slot_id > XHCI_MAX_SLOTS) return -1;
    xhci_device_t *dev = &xhci_devices[slot_id];
    if (!dev->in_use) return -1;

    uint64_t data_phys = data ? virt_to_phys(data) : 0;

    /* Setup TRB */
    xhci_trb_t setup_trb = {0};
    memcpy(&setup_trb.parameter, setup, 8);
    setup_trb.status = 8;  /* Transfer length = 8 */
    setup_trb.control = TRB_TYPE_SETUP | TRB_IDT;
    if (data_len > 0) {
        setup_trb.control |= (dir_in ? 3 : 2) << 16;  /* TRT: IN=3, OUT=2 */
    }
    xhci_ep0_push(dev, &setup_trb);

    /* Data TRB (if needed) */
    if (data_len > 0 && data) {
        xhci_trb_t data_trb = {0};
        data_trb.parameter = data_phys;
        data_trb.status = data_len;
        data_trb.control = TRB_TYPE_DATA;
        if (dir_in) data_trb.control |= (1 << 16);  /* DIR = IN */
        xhci_ep0_push(dev, &data_trb);
    }

    /* Status TRB */
    xhci_trb_t status_trb = {0};
    status_trb.control = TRB_TYPE_STATUS | TRB_IOC;
    /* Status direction is opposite of data direction */
    if (data_len > 0 && !dir_in) {
        status_trb.control |= (1 << 16);  /* DIR = IN for status after OUT data */
    }
    xhci_ep0_push(dev, &status_trb);

    /* Ring the doorbell for EP0 (DCI = 1) */
    xhci.doorbells[slot_id] = 1;

    /* Wait for completion */
    int cc = xhci_wait_event(500);
    if (cc != TRB_CC_SUCCESS && cc != TRB_CC_SHORT_PACKET) {
        serial_printf("xhci: control transfer failed slot=%d cc=%d\n",
                      slot_id, cc);
        return -1;
    }

    return data_len;
}

/**
 * @brief Get USB device descriptor
 */
static int xhci_get_device_descriptor(uint8_t slot_id, usb_device_desc_t *desc) {
    xhci_setup_packet_t setup = {
        .bmRequestType = 0x80,  /* Device-to-host, standard, device */
        .bRequest = USB_REQ_GET_DESCRIPTOR,
        .wValue = (USB_DESC_DEVICE << 8) | 0,
        .wIndex = 0,
        .wLength = sizeof(usb_device_desc_t)
    };

    /* Allocate DMA-safe buffer */
    void *buf = kalloc_high(sizeof(usb_device_desc_t));
    if (!buf) return -1;
    memset(buf, 0, sizeof(usb_device_desc_t));

    int ret = xhci_dev_control_transfer(slot_id, &setup, buf,
                                         sizeof(usb_device_desc_t), true);
    if (ret >= 0) {
        memcpy(desc, buf, sizeof(usb_device_desc_t));
    }
    kfree(buf);
    return ret;
}

/**
 * @brief Set USB configuration
 */
static int xhci_set_configuration(uint8_t slot_id, uint8_t config_value) {
    xhci_setup_packet_t setup = {
        .bmRequestType = 0x00,
        .bRequest = USB_REQ_SET_CONFIG,
        .wValue = config_value,
        .wIndex = 0,
        .wLength = 0
    };
    return xhci_dev_control_transfer(slot_id, &setup, NULL, 0, false);
}

/**
 * @brief Set HID protocol (boot or report)
 */
static int xhci_hid_set_protocol(uint8_t slot_id, uint8_t interface, uint8_t protocol) {
    xhci_setup_packet_t setup = {
        .bmRequestType = 0x21,  /* Host-to-device, class, interface */
        .bRequest = USB_HID_SET_PROTOCOL,
        .wValue = protocol,     /* 0 = boot, 1 = report */
        .wIndex = interface,
        .wLength = 0
    };
    return xhci_dev_control_transfer(slot_id, &setup, NULL, 0, false);
}

/**
 * @brief Set HID idle rate
 */
static int xhci_hid_set_idle(uint8_t slot_id, uint8_t interface) {
    xhci_setup_packet_t setup = {
        .bmRequestType = 0x21,
        .bRequest = USB_HID_SET_IDLE,
        .wValue = 0,  /* Duration = 0 (indefinite), Report ID = 0 */
        .wIndex = interface,
        .wLength = 0
    };
    return xhci_dev_control_transfer(slot_id, &setup, NULL, 0, false);
}

/* ============================================================================
 * Device Enumeration
 * ============================================================================ */

int xhci_enumerate_device(uint8_t port) {
    if (!xhci.initialized || port == 0 || port > xhci.max_ports) return -1;

    /* Reset the port */
    if (xhci_port_reset(port) < 0) return -1;
    uint8_t speed = xhci.port_speed[port];

    /* Enable a slot */
    int slot_id = xhci_enable_slot();
    if (slot_id < 0) return -1;

    /* Set up device context */
    if (xhci_setup_device_context(slot_id, port, speed) < 0) return -1;
    xhci.port_slot[port] = slot_id;

    /* Address the device */
    if (xhci_address_device(slot_id, port, speed) < 0) return -1;

    /* Get device descriptor */
    usb_device_desc_t desc;
    if (xhci_get_device_descriptor(slot_id, &desc) < 0) {
        serial_printf("xhci: failed to get device descriptor for slot %d\n", slot_id);
        return slot_id;  /* Still return slot even if descriptor fails */
    }

    serial_printf("xhci: slot %d: USB %x.%x class=%d subclass=%d protocol=%d "
                  "vendor=0x%04x product=0x%04x\n",
                  slot_id, desc.bcdUSB >> 8, (desc.bcdUSB >> 4) & 0xF,
                  desc.bDeviceClass, desc.bDeviceSubClass, desc.bDeviceProtocol,
                  desc.idVendor, desc.idProduct);

    printf("USB: Device on port %d - vendor=0x%04x product=0x%04x",
           port, desc.idVendor, desc.idProduct);

    /* Set configuration (use first configuration) */
    if (desc.bNumConfigurations > 0) {
        xhci_set_configuration(slot_id, 1);
    }

    /* Check if this is a HID device */
    xhci_device_t *dev = &xhci_devices[slot_id];

    /* HID boot keyboard: class 3, subclass 1, protocol 1
     * HID boot mouse:    class 3, subclass 1, protocol 2
     * Note: class may be 0 at device level (defined at interface level) */
    if (desc.bDeviceClass == USB_CLASS_HID ||
        desc.bDeviceClass == 0) {  /* Class may be interface-defined */

        /* Try setting boot protocol regardless */
        xhci_hid_set_protocol(slot_id, 0, 0);  /* Boot protocol */
        xhci_hid_set_idle(slot_id, 0);

        if (desc.bDeviceProtocol == USB_HID_PROTOCOL_KEYBOARD ||
            desc.bDeviceClass == 0) {
            /* Assume keyboard if class is at interface level */
            dev->is_hid_keyboard = true;
            printf(" [HID Keyboard]");
        }
        if (desc.bDeviceProtocol == USB_HID_PROTOCOL_MOUSE) {
            dev->is_hid_mouse = true;
            printf(" [HID Mouse]");
        }
    }

    printf("\n");
    return slot_id;
}

void xhci_enumerate_all(void) {
    if (!xhci.initialized) return;

    serial_puts("xhci: enumerating all connected devices\n");
    printf("USB: Enumerating devices...\n");

    for (uint32_t p = 1; p <= xhci.max_ports; p++) {
        if (xhci_port_connected(p)) {
            int slot = xhci_enumerate_device(p);
            if (slot >= 0) {
                serial_printf("xhci: port %d -> slot %d\n", p, slot);
            }
        }
    }
}

void xhci_poll_hid(void) {
    /* In a full implementation, this would check interrupt transfer
     * completions on HID devices' interrupt endpoints.
     * For now, this is a placeholder that can be expanded. */
    if (!xhci.initialized) return;

    /* Future: process interrupt transfer completions from HID devices */
}

/* ============================================================================
 * Status display
 * ============================================================================ */

void xhci_print_status(void) {
    if (!xhci.initialized) {
        printf("xHCI: Not initialized\n");
        return;
    }

    printf("xHCI Controller Status:\n");
    printf("  Version: 0x%04x\n", xhci.cap->hci_version);
    printf("  Slots: %d, Ports: %d\n", xhci.max_slots, xhci.max_ports);
    printf("  USBCMD: 0x%08x  USBSTS: 0x%08x\n",
           xhci.op->usbcmd, xhci.op->usbsts);
    printf("  Running: %s\n",
           (xhci.op->usbsts & XHCI_STS_HCH) ? "No" : "Yes");

    for (uint32_t p = 1; p <= xhci.max_ports; p++) {
        uint32_t portsc = xhci.ports[p - 1].portsc;
        if (portsc & XHCI_PORTSC_CCS) {
            uint8_t speed = (portsc >> XHCI_PORTSC_SPEED_SHIFT) & 0xF;
            const char *sstr = "?";
            switch (speed) {
                case 1: sstr = "Full"; break;
                case 2: sstr = "Low"; break;
                case 3: sstr = "High"; break;
                case 4: sstr = "Super"; break;
            }
            printf("  Port %d: Connected (%s speed) PORTSC=0x%08x\n",
                   p, sstr, portsc);
        }
    }
}
