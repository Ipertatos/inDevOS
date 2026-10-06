#pragma once

#include "types.h"
#include "pci.h"
#include "storage.h"

// USB Debug Control
// Set to 1 to enable verbose USB debug output, 0 to disable
#define USB_DEBUG_VERBOSE 0  // Disabled to reduce spam

// USB Debug Macro
#ifdef DEBUG_PRINT
#define USB_DEBUG(...) do { if (USB_DEBUG_VERBOSE) { serial_printf(__VA_ARGS__); } } while(0)
#else
#define USB_DEBUG(...) do {} while(0)
#endif

// USB Controller Types
#define USB_CONTROLLER_UHCI    0x00
#define USB_CONTROLLER_OHCI    0x10
#define USB_CONTROLLER_EHCI    0x20
#define USB_CONTROLLER_XHCI    0x30

// USB Device Classes
#define USB_CLASS_MASS_STORAGE 0x08
#define USB_CLASS_HUB          0x09
#define USB_CLASS_HID          0x03

// USB HID Subclasses
#define USB_HID_SUBCLASS_BOOT  0x01

// USB HID Protocols
#define USB_HID_PROTOCOL_KEYBOARD 0x01
#define USB_HID_PROTOCOL_MOUSE    0x02

// USB HID Report Types
#define USB_HID_REPORT_TYPE_INPUT   0x01
#define USB_HID_REPORT_TYPE_OUTPUT  0x02
#define USB_HID_REPORT_TYPE_FEATURE 0x03

// USB HID Requests
#define USB_HID_REQ_SET_IDLE    0x0A
#define USB_HID_REQ_GET_IDLE    0x02
#define USB_HID_REQ_SET_REPORT  0x09
#define USB_HID_REQ_GET_REPORT  0x01
#define USB_HID_REQ_SET_PROTOCOL 0x0B
#define USB_HID_REQ_GET_PROTOCOL 0x03

// USB Request Types
#define USB_REQ_DIR_OUT        0x00
#define USB_REQ_DIR_IN         0x80
#define USB_REQ_TYPE_STANDARD  0x00
#define USB_REQ_TYPE_CLASS     0x20
#define USB_REQ_TYPE_VENDOR    0x40

// USB Standard Requests
#define USB_REQ_GET_STATUS     0x00
#define USB_REQ_CLEAR_FEATURE  0x01
#define USB_REQ_SET_FEATURE    0x03
#define USB_REQ_SET_ADDRESS    0x05
#define USB_REQ_GET_DESCRIPTOR 0x06
#define USB_REQ_SET_DESCRIPTOR 0x07
#define USB_REQ_GET_CONFIG     0x08
#define USB_REQ_SET_CONFIG     0x09

// USB Descriptor Types
#define USB_DESC_DEVICE        0x01
#define USB_DESC_CONFIG        0x02
#define USB_DESC_STRING        0x03
#define USB_DESC_INTERFACE     0x04
#define USB_DESC_ENDPOINT      0x05

// USB Endpoint Types
#define USB_ENDPOINT_CONTROL   0x00
#define USB_ENDPOINT_ISOCH     0x01
#define USB_ENDPOINT_BULK      0x02
#define USB_ENDPOINT_INTERRUPT 0x03

// USB Device Descriptor
typedef struct {
    uint8_t  length;
    uint8_t  type;
    uint16_t usb_version;
    uint8_t  device_class;
    uint8_t  device_subclass;
    uint8_t  device_protocol;
    uint8_t  max_packet_size;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t device_version;
    uint8_t  manufacturer_string;
    uint8_t  product_string;
    uint8_t  serial_number_string;
    uint8_t  num_configurations;
} __attribute__((packed)) usb_device_descriptor_t;

// USB Configuration Descriptor
typedef struct {
    uint8_t  length;
    uint8_t  type;
    uint16_t total_length;
    uint8_t  num_interfaces;
    uint8_t  config_value;
    uint8_t  config_string;
    uint8_t  attributes;
    uint8_t  max_power;
} __attribute__((packed)) usb_config_descriptor_t;

// USB Interface Descriptor
typedef struct {
    uint8_t  length;
    uint8_t  type;
    uint8_t  interface_number;
    uint8_t  alternate_setting;
    uint8_t  num_endpoints;
    uint8_t  interface_class;
    uint8_t  interface_subclass;
    uint8_t  interface_protocol;
    uint8_t  interface_string;
} __attribute__((packed)) usb_interface_descriptor_t;

// USB Endpoint Descriptor
typedef struct {
    uint8_t  length;
    uint8_t  type;
    uint8_t  endpoint_address;
    uint8_t  attributes;
    uint16_t max_packet_size;
    uint8_t  interval;
} __attribute__((packed)) usb_endpoint_descriptor_t;

// USB HID Descriptor
typedef struct {
    uint8_t  length;
    uint8_t  type;
    uint16_t hid_version;
    uint8_t  country_code;
    uint8_t  num_descriptors;
    struct {
        uint8_t  type;
        uint16_t length;
    } __attribute__((packed)) descriptors[1];
} __attribute__((packed)) usb_hid_descriptor_t;

// USB Keyboard Report (Boot Protocol)
typedef struct {
    uint8_t modifier_keys;  // Ctrl, Shift, Alt, etc.
    uint8_t reserved;       // Always 0
    uint8_t key_codes[6];   // Up to 6 keys pressed simultaneously
} __attribute__((packed)) usb_keyboard_report_t;

// USB Mouse Report (Boot Protocol)
typedef struct {
    uint8_t buttons;        // Bit 0=left, bit 1=right, bit 2=middle
    int8_t  x_movement;     // Relative X movement
    int8_t  y_movement;     // Relative Y movement
} __attribute__((packed)) usb_mouse_report_t;

// Forward declaration for controller
struct usb_controller;

// USB Device Structure
typedef struct usb_device {
    uint8_t  address;
    uint8_t  port;
    uint8_t  speed;
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t  device_class;
    uint8_t  device_subclass;
    uint8_t  device_protocol;
    usb_device_descriptor_t device_desc;
    struct usb_controller *controller;
    struct usb_device *next;
} usb_device_t;

// USB Controller Structure
typedef struct usb_controller {
    struct pci_device *pci_dev;
    uint64_t base_address;
    uint8_t controller_type;
    bool initialized;
    usb_device_t *devices;
    uint8_t device_count;
    uint8_t max_devices;
} usb_controller_t;

// USB HID Device Structure
typedef struct usb_hid_device {
    usb_device_t *usb_device;
    uint8_t protocol;
    uint8_t idle_rate;
    uint8_t report_descriptor_length;
    uint8_t *report_descriptor;
    uint8_t input_endpoint;
    uint8_t output_endpoint;
    uint8_t interrupt_endpoint;
    uint16_t max_packet_size;
    bool boot_protocol;
    struct usb_hid_device *next;
} usb_hid_device_t;

// USB Setup Packet
typedef struct {
    uint8_t  request_type;
    uint8_t  request;
    uint16_t value;
    uint16_t index;
    uint16_t length;
} __attribute__((packed)) usb_setup_packet_t;

// Function declarations
void usb_init();
uint8_t usb_get_controller_count();
usb_controller_t* usb_get_controller(uint8_t index);
int usb_controller_init(usb_controller_t *ctrl);
int usb_device_enumeration(usb_controller_t *ctrl);
int usb_send_setup_packet(usb_controller_t *ctrl, uint8_t device_addr, usb_setup_packet_t *setup, void *data);
int usb_get_device_descriptor(usb_controller_t *ctrl, uint8_t device_addr, usb_device_descriptor_t *desc);
int usb_set_device_address(usb_controller_t *ctrl, uint8_t device_addr, uint8_t new_addr);
int usb_get_config_descriptor(usb_controller_t *ctrl, uint8_t device_addr, uint8_t config_index, void *buffer, uint16_t length);

// USB Mass Storage support
int usb_mass_storage_init(usb_device_t *device);
int usb_mass_storage_read_sectors(storage_device_t *dev, uint64_t lba, uint32_t sector_count, void *buffer);
int usb_mass_storage_write_sectors(storage_device_t *dev, uint64_t lba, uint32_t sector_count, const void *buffer);
int usb_mass_storage_flush(storage_device_t *dev);

// USB HID support (for keyboards, mice, etc.)
int usb_hid_init(usb_device_t *device);
int usb_hid_read_report(usb_device_t *device, void *buffer, uint16_t length);
int usb_hid_set_protocol(usb_device_t *device, uint8_t protocol);
int usb_hid_set_idle(usb_device_t *device, uint8_t idle_rate);
int usb_hid_get_report_descriptor(usb_device_t *device, void *buffer, uint16_t length);

// USB Keyboard specific functions
int usb_keyboard_init(usb_device_t *device);
int usb_keyboard_read_report(usb_device_t *device, usb_keyboard_report_t *report);
int usb_keyboard_process_report(usb_keyboard_report_t *report);
uint8_t usb_keyboard_scancode_to_ascii(uint8_t scancode, bool shift_pressed);

// USB Keyboard polling and integration
void usb_keyboard_poll_all();
void usb_keyboard_integrate_with_system();
bool usb_keyboard_has_input();
char usb_keyboard_get_char();

// USB Mouse specific functions
int usb_mouse_init(usb_device_t *device);
int usb_mouse_read_report(usb_device_t *device, usb_mouse_report_t *report);
int usb_mouse_process_report(usb_mouse_report_t *report);

// USB Mouse polling and integration
void usb_mouse_poll_all();

// Keyboard scancode to ASCII conversion
extern const uint8_t usb_keyboard_scancode_table[128];
extern const uint8_t usb_keyboard_shift_scancode_table[128];

// Debug functions
void usb_print_device_info(usb_device_t *device);
void usb_print_controller_info(usb_controller_t *ctrl); 