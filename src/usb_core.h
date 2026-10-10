/**
 * @file usb_core.h
 * @brief USB device model on top of xhci.c: enumeration, hubs, class binding.
 */
#pragma once

#include "types.h"
#include "xhci.h"

#define USB_MAX_DEVICES 32
#define USB_MAX_BOUND   4

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
} usb_dev_desc_t;

/* One endpoint of an interface, from the configuration descriptor. */
typedef struct {
    uint8_t addr;        /* bit 7 = IN */
    uint8_t attr;        /* 2 bulk, 3 interrupt */
    uint16_t mps;
    uint8_t interval;
    uint8_t burst;       /* SuperSpeed companion bMaxBurst */
} usb_ep_info_t;

typedef struct {
    uint8_t number;
    uint8_t cls, sub, proto;
    uint8_t n_ep;
    usb_ep_info_t ep[4];
} usb_iface_info_t;

enum { USB_DRV_NONE = 0, USB_DRV_HUB, USB_DRV_KBD, USB_DRV_MOUSE, USB_DRV_MSC };

typedef struct usb_dev {
    bool present;
    int id;
    xhci_ctrl_t *hc;
    xhci_slot_t *slot;
    struct usb_dev *parent;      /* hub, or NULL for a root port */
    uint8_t port;                /* port on the parent hub / root hub */
    uint8_t root_port;
    uint8_t depth;               /* 0 = on a root port */
    uint32_t route;
    uint8_t speed;
    uint8_t tt_slot, tt_port;
    usb_dev_desc_t desc;
    uint8_t config_value;
    char manufacturer[32];
    char product[40];
    uint8_t n_iface;
    usb_iface_info_t iface[8];
    /* bound class drivers */
    uint8_t n_bound;
    struct {
        int drv;
        uint8_t iface;
        void *data;
    } bound[USB_MAX_BOUND];
    /* hub state */
    uint8_t hub_ports;
    bool hub_ss;
    struct usb_dev *child[16];
    uint8_t hub_int_ep;
    uint8_t *hub_int_buf;
    bool hub_int_queued;
} usb_dev_t;

/* Boot-time bring-up: controllers, root ports, hubs, class drivers. */
void usb_core_init(void);
/* Main-loop service: HID reports, hub/root port hotplug. Cheap if idle. */
void usb_poll(void);
/* `lsusb` */
void usb_list_devices(bool verbose);
usb_dev_t *usb_get_device(int i);

/* Helpers for class drivers (caller holds usb_lock). */
int usb_ctrl(usb_dev_t *d, uint8_t rt, uint8_t req, uint16_t val, uint16_t idx,
             uint16_t len, void *data);
int usb_clear_halt(usb_dev_t *d, uint8_t epaddr);

/* Class drivers */
int usb_hid_attach(usb_dev_t *d, usb_iface_info_t *ifc, int drv, void **data);
void usb_hid_detach(void *data);
void usb_hid_poll(void *data);
int usb_msc_attach(usb_dev_t *d, usb_iface_info_t *ifc, void **data);
void usb_msc_detach(void *data);
