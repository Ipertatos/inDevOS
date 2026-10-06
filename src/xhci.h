/**
 * @file xhci.h
 * @brief xHCI (USB 3.0) Host Controller Driver
 *
 * Implements the eXtensible Host Controller Interface for USB.
 * Supports USB 1.x, 2.0, and 3.0 devices through a unified controller.
 * Uses Transfer Request Blocks (TRBs) and ring buffers for DMA communication.
 */

#ifndef XHCI_H
#define XHCI_H

#include "types.h"
#include "pci.h"

/* ============================================================================
 * xHCI Register Structures (Capability + Operational + Runtime + Doorbell)
 * ============================================================================ */

/** @brief xHCI Capability Registers (read-only, base address from BAR0) */
typedef struct __attribute__((packed)) {
    uint8_t  caplength;     /**< Capability register length */
    uint8_t  reserved;
    uint16_t hci_version;   /**< Interface version (e.g. 0x0100 = 1.0) */
    uint32_t hcsparams1;    /**< Structural parameters 1 */
    uint32_t hcsparams2;    /**< Structural parameters 2 */
    uint32_t hcsparams3;    /**< Structural parameters 3 */
    uint32_t hccparams1;    /**< Capability parameters 1 */
    uint32_t dboff;         /**< Doorbell offset */
    uint32_t rtsoff;        /**< Runtime register space offset */
    uint32_t hccparams2;    /**< Capability parameters 2 */
} xhci_cap_regs_t;

/** @brief xHCI Operational Registers */
typedef struct __attribute__((packed)) {
    uint32_t usbcmd;        /**< USB Command Register */
    uint32_t usbsts;        /**< USB Status Register */
    uint32_t pagesize;      /**< Page Size */
    uint32_t reserved1[2];
    uint32_t dnctrl;        /**< Device Notification Control */
    uint64_t crcr;          /**< Command Ring Control */
    uint32_t reserved2[4];
    uint64_t dcbaap;        /**< Device Context Base Address Array Pointer */
    uint32_t config;        /**< Configure Register */
} xhci_op_regs_t;

/** @brief xHCI Port Register Set */
typedef struct __attribute__((packed)) {
    uint32_t portsc;        /**< Port Status and Control */
    uint32_t portpmsc;      /**< Port Power Management Status/Control */
    uint32_t portli;        /**< Port Link Info */
    uint32_t porthlpmc;     /**< Port Hardware LPM Control */
} xhci_port_regs_t;

/* USBCMD bits */
#define XHCI_CMD_RUN        (1 << 0)    /**< Run/Stop */
#define XHCI_CMD_HCRST      (1 << 1)    /**< Host Controller Reset */
#define XHCI_CMD_INTE       (1 << 2)    /**< Interrupter Enable */
#define XHCI_CMD_HSEE       (1 << 3)    /**< Host System Error Enable */

/* USBSTS bits */
#define XHCI_STS_HCH        (1 << 0)    /**< HCHalted */
#define XHCI_STS_HSE        (1 << 2)    /**< Host System Error */
#define XHCI_STS_EINT       (1 << 3)    /**< Event Interrupt */
#define XHCI_STS_PCD        (1 << 4)    /**< Port Change Detect */
#define XHCI_STS_CNR        (1 << 11)   /**< Controller Not Ready */

/* PORTSC bits */
#define XHCI_PORTSC_CCS     (1 << 0)    /**< Current Connect Status */
#define XHCI_PORTSC_PED     (1 << 1)    /**< Port Enabled/Disabled */
#define XHCI_PORTSC_OCA     (1 << 3)    /**< Over-current Active */
#define XHCI_PORTSC_PR      (1 << 4)    /**< Port Reset */
#define XHCI_PORTSC_PP      (1 << 9)    /**< Port Power */
#define XHCI_PORTSC_CSC     (1 << 17)   /**< Connect Status Change */
#define XHCI_PORTSC_PEC     (1 << 18)   /**< Port Enabled Change */
#define XHCI_PORTSC_PRC     (1 << 21)   /**< Port Reset Change */
#define XHCI_PORTSC_SPEED_MASK  (0xF << 10)
#define XHCI_PORTSC_SPEED_SHIFT 10

/* Port speeds */
#define XHCI_SPEED_FULL     1   /**< Full speed (12 Mbps) */
#define XHCI_SPEED_LOW      2   /**< Low speed (1.5 Mbps) */
#define XHCI_SPEED_HIGH     3   /**< High speed (480 Mbps) */
#define XHCI_SPEED_SUPER    4   /**< Super speed (5 Gbps) */

/* ============================================================================
 * Transfer Request Block (TRB) - the fundamental unit of xHCI communication
 * ============================================================================ */

/** @brief TRB structure (16 bytes, aligned) */
typedef struct __attribute__((packed)) {
    uint64_t parameter;     /**< Parameter (address or data) */
    uint32_t status;        /**< Status / transfer length */
    uint32_t control;       /**< Control (TRB type, flags) */
} xhci_trb_t;

/* TRB types (bits 15:10 of control) */
#define TRB_TYPE_SHIFT      10
#define TRB_TYPE(x)         ((x) << TRB_TYPE_SHIFT)

#define TRB_TYPE_NORMAL         TRB_TYPE(1)
#define TRB_TYPE_SETUP          TRB_TYPE(2)
#define TRB_TYPE_DATA           TRB_TYPE(3)
#define TRB_TYPE_STATUS         TRB_TYPE(4)
#define TRB_TYPE_LINK           TRB_TYPE(6)
#define TRB_TYPE_EVENT_DATA     TRB_TYPE(7)
#define TRB_TYPE_NOOP           TRB_TYPE(8)
#define TRB_TYPE_ENABLE_SLOT    TRB_TYPE(9)
#define TRB_TYPE_DISABLE_SLOT   TRB_TYPE(10)
#define TRB_TYPE_ADDRESS_DEV    TRB_TYPE(11)
#define TRB_TYPE_CONFIG_EP      TRB_TYPE(12)
#define TRB_TYPE_EVAL_CTX       TRB_TYPE(13)
#define TRB_TYPE_RESET_EP       TRB_TYPE(14)
#define TRB_TYPE_STOP_EP        TRB_TYPE(15)
#define TRB_TYPE_NOOP_CMD       TRB_TYPE(23)

/* Event TRB types */
#define TRB_TYPE_TRANSFER_EVENT TRB_TYPE(32)
#define TRB_TYPE_CMD_COMPLETE   TRB_TYPE(33)
#define TRB_TYPE_PORT_STATUS    TRB_TYPE(34)

/* TRB control flags */
#define TRB_CYCLE           (1 << 0)     /**< Cycle bit */
#define TRB_TC              (1 << 1)     /**< Toggle cycle (for link TRBs) */
#define TRB_ISP             (1 << 2)     /**< Interrupt on Short Packet */
#define TRB_CH              (1 << 4)     /**< Chain bit */
#define TRB_IOC             (1 << 5)     /**< Interrupt on Completion */
#define TRB_IDT             (1 << 6)     /**< Immediate Data */

/* TRB status completion codes */
#define TRB_CC_SUCCESS      1
#define TRB_CC_SHORT_PACKET 13

/* ============================================================================
 * Ring buffer sizes
 * ============================================================================ */

#define XHCI_CMD_RING_SIZE      64      /**< Command ring TRB count */
#define XHCI_EVT_RING_SIZE      64      /**< Event ring TRB count */
#define XHCI_TRANSFER_RING_SIZE 64      /**< Transfer ring TRB count per EP */
#define XHCI_MAX_SLOTS          32      /**< Maximum device slots */
#define XHCI_MAX_PORTS          16      /**< Maximum root hub ports */

/* ============================================================================
 * xHCI Driver State
 * ============================================================================ */

typedef struct {
    bool initialized;
    struct pci_device *pci_dev;

    /* Memory-mapped register bases */
    volatile xhci_cap_regs_t *cap;
    volatile xhci_op_regs_t  *op;
    volatile uint32_t        *doorbells;
    volatile uint32_t        *runtime;
    volatile xhci_port_regs_t *ports;

    /* Controller parameters */
    uint32_t max_slots;
    uint32_t max_ports;
    uint32_t max_intrs;
    uint32_t page_size;

    /* Device Context Base Address Array */
    uint64_t *dcbaa;
    uint64_t dcbaa_phys;

    /* Command Ring */
    xhci_trb_t *cmd_ring;
    uint64_t cmd_ring_phys;
    uint32_t cmd_ring_enqueue;
    uint8_t  cmd_ring_cycle;

    /* Event Ring */
    xhci_trb_t *evt_ring;
    uint64_t evt_ring_phys;
    uint32_t evt_ring_dequeue;
    uint8_t  evt_ring_cycle;

    /* Event Ring Segment Table */
    void    *erst;
    uint64_t erst_phys;

    /* Per-slot tracking */
    uint8_t slot_enabled[XHCI_MAX_SLOTS + 1];  /* 1-indexed */

    /* Port info */
    uint8_t port_speed[XHCI_MAX_PORTS + 1];    /* 1-indexed */
    uint8_t port_slot[XHCI_MAX_PORTS + 1];     /* port -> slot mapping */
} xhci_state_t;

/* ============================================================================
 * Public API
 * ============================================================================ */

/**
 * @brief Initialize the xHCI host controller
 * @return 0 on success, -1 if no xHCI found or init failed
 */
int xhci_init(void);

/**
 * @brief Check if xHCI is available and initialized
 */
bool xhci_is_available(void);

/**
 * @brief Get the number of root hub ports
 */
uint32_t xhci_get_port_count(void);

/**
 * @brief Check if a device is connected to a port
 * @param port Port number (1-based)
 * @return true if a device is connected
 */
bool xhci_port_connected(uint8_t port);

/**
 * @brief Get the speed of a connected device
 * @param port Port number (1-based)
 * @return Speed code (XHCI_SPEED_*)
 */
uint8_t xhci_port_speed(uint8_t port);

/**
 * @brief Reset a port and enable the connected device
 * @param port Port number (1-based)
 * @return 0 on success
 */
int xhci_port_reset(uint8_t port);

/**
 * @brief Send a USB control transfer
 *
 * @param slot      Device slot
 * @param setup     8-byte setup packet
 * @param data      Data buffer (or NULL for no-data transfers)
 * @param data_len  Length of data buffer
 * @param dir_in    true for device-to-host, false for host-to-device
 * @return Bytes transferred, or -1 on error
 */
int xhci_control_transfer(uint8_t slot, void *setup, void *data,
                           uint16_t data_len, bool dir_in);

/**
 * @brief Enumerate a device on a port
 *
 * Performs Enable Slot, Address Device, Get Descriptor, and
 * Set Configuration to bring up the device. For HID devices,
 * also sets boot protocol and starts polling.
 *
 * @param port  Port number (1-based)
 * @return Slot number on success, -1 on failure
 */
int xhci_enumerate_device(uint8_t port);

/**
 * @brief Enumerate all connected devices on all ports
 *
 * Scans all ports, resets connected devices, and enumerates them.
 */
void xhci_enumerate_all(void);

/**
 * @brief Poll HID devices for input reports
 *
 * Checks for pending interrupt transfer completions from HID
 * devices and processes keyboard/mouse reports.
 */
void xhci_poll_hid(void);

/**
 * @brief Print xHCI controller status
 */
void xhci_print_status(void);

/* ============================================================================
 * USB Descriptor Structures
 * ============================================================================ */

/** @brief USB Device Descriptor */
typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} usb_device_desc_t;

/** @brief USB Configuration Descriptor */
typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
} usb_config_desc_t;

/** @brief USB Interface Descriptor */
typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bInterfaceNumber;
    uint8_t  bAlternateSetting;
    uint8_t  bNumEndpoints;
    uint8_t  bInterfaceClass;
    uint8_t  bInterfaceSubClass;
    uint8_t  bInterfaceProtocol;
    uint8_t  iInterface;
} usb_interface_desc_t;

/** @brief USB Endpoint Descriptor */
typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;
    uint8_t  bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} usb_endpoint_desc_t;

/* USB descriptor types (guarded to avoid redefinition from usb.h) */
#ifndef USB_DESC_DEVICE
#define USB_DESC_DEVICE         1
#endif
#ifndef USB_DESC_CONFIGURATION
#define USB_DESC_CONFIGURATION  2
#endif
#ifndef USB_DESC_STRING
#define USB_DESC_STRING         3
#endif
#ifndef USB_DESC_INTERFACE
#define USB_DESC_INTERFACE      4
#endif
#ifndef USB_DESC_ENDPOINT
#define USB_DESC_ENDPOINT       5
#endif
#ifndef USB_DESC_HID
#define USB_DESC_HID            0x21
#endif

/* USB device classes */
#ifndef USB_CLASS_HID
#define USB_CLASS_HID           3
#endif

/* USB HID subclasses */
#ifndef USB_HID_SUBCLASS_BOOT
#define USB_HID_SUBCLASS_BOOT   1
#endif

/* USB HID protocols */
#ifndef USB_HID_PROTOCOL_KEYBOARD
#define USB_HID_PROTOCOL_KEYBOARD   1
#endif
#ifndef USB_HID_PROTOCOL_MOUSE
#define USB_HID_PROTOCOL_MOUSE      2
#endif

/* USB request types */
#ifndef USB_REQ_GET_DESCRIPTOR
#define USB_REQ_GET_DESCRIPTOR  6
#endif
#ifndef USB_REQ_SET_CONFIG
#define USB_REQ_SET_CONFIG      9
#endif
#ifndef USB_REQ_SET_INTERFACE
#define USB_REQ_SET_INTERFACE   11
#endif

/* HID class-specific requests */
#ifndef USB_HID_SET_IDLE
#define USB_HID_SET_IDLE        0x0A
#endif
#ifndef USB_HID_SET_PROTOCOL
#define USB_HID_SET_PROTOCOL    0x0B
#endif

/* USB setup packet for xHCI (uses standard USB field names) */
typedef struct __attribute__((packed)) {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} xhci_setup_packet_t;

#endif /* XHCI_H */
