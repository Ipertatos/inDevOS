#include "usb.h"
/*
 * Legacy USB stack: control transfers below are stubs for non-xHCI paths.
 * The xHCI driver (xhci.c) performs real enumeration for USB3 controllers;
 * BOT/CBW mass-storage I/O is not wired — do not use usb_mass_storage_* for data.
 */
#include "mem.h"
#include "serial.h"
#include "utils.h"
#include "storage.h"
#include "keyboard.h"
#include "atomic.h"
#include "hpet.h"
#include "vconsole.h"
#include "mouse.h"
#include "string.h"

#define MAX_USB_CONTROLLERS 4
#define MAX_USB_DEVICES_PER_CONTROLLER 8

static usb_controller_t usb_controllers[MAX_USB_CONTROLLERS];
static uint8_t usb_controller_count = 0;

// USB HID device list
static usb_hid_device_t *usb_hid_devices = NULL;
static uint8_t usb_hid_device_count = 0;

// USB Keyboard input buffer
#define USB_KEYBOARD_BUFFER_SIZE 256
static char usb_keyboard_buffer[USB_KEYBOARD_BUFFER_SIZE];
static uint32_t usb_keyboard_buffer_head = 0;
static uint32_t usb_keyboard_buffer_tail = 0;
static bool usb_keyboard_has_data = false;
static spinlock_t usb_keyboard_lock = {0};

// USB Keyboard scancode to ASCII conversion tables
const uint8_t usb_keyboard_scancode_table[128] = {
    0,   0,   0,   0,   'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l',
    'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', '1', '2',
    '3', '4', '5', '6', '7', '8', '9', '0', '\n', 0, '\b', '\t', ' ', '-', '=', '[',
    ']', '\\', '#', ';', '\'', '`', ',', '.', '/', 0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0
};

const uint8_t usb_keyboard_shift_scancode_table[128] = {
    0,   0,   0,   0,   'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L',
    'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', '!', '@',
    '#', '$', '%', '^', '&', '*', '(', ')', '\n', 0, '\b', '\t', ' ', '_', '+', '{',
    '}', '|', '~', ':', '"', '~', '<', '>', '?', 0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0
};

// Function to get USB controller count (for external access)
uint8_t usb_get_controller_count() {
    return usb_controller_count;
}

// Function to get USB controller by index (for external access)
usb_controller_t* usb_get_controller(uint8_t index) {
    if (index < usb_controller_count) {
        return &usb_controllers[index];
    }
    return NULL;
}

// Debug function to print USB device information
void usb_print_device_info(usb_device_t *device) {
    if (!device) return;
    
    printf("USB Device: addr=%d, port=%d, vid=0x%04x, pid=0x%04x, class=0x%02x\n",
           device->address, device->port, device->vendor_id, device->product_id, device->device_class);
    
    DEBUG_SERIAL("USB Device: addr=%d, port=%d, vid=0x%04x, pid=0x%04x, class=0x%02x\n",
                 device->address, device->port, device->vendor_id, device->product_id, device->device_class);
}

// Debug function to print USB controller information
void usb_print_controller_info(usb_controller_t *ctrl) {
    if (!ctrl) return;
    
    printf("USB Controller: type=0x%02x, base=0x%lx, devices=%d\n",
           ctrl->controller_type, ctrl->base_address, ctrl->device_count);
    
    DEBUG_SERIAL("USB Controller: type=0x%02x, base=0x%lx, devices=%d\n",
                 ctrl->controller_type, ctrl->base_address, ctrl->device_count);
}

// Initialize a USB controller
int usb_controller_init(usb_controller_t *ctrl) {
    if (!ctrl || !ctrl->pci_dev) {
        DEBUG_SERIAL("USB: Invalid controller or PCI device\n");
        return -1;
    }
    
    printf("USB: Initializing controller at bus %d, device %d, func %d\n",
           ctrl->pci_dev->bus, ctrl->pci_dev->device, ctrl->pci_dev->function);
    
    DEBUG_SERIAL("USB: Initializing controller at bus %d, device %d, func %d\n",
                 ctrl->pci_dev->bus, ctrl->pci_dev->device, ctrl->pci_dev->function);
    
    // Read BAR0 for controller base address
    uint32_t bar0 = pci_device_read(ctrl->pci_dev, 0x10, 4);
    uint32_t bar1 = pci_device_read(ctrl->pci_dev, 0x14, 4);
    
    DEBUG_SERIAL("USB: BAR0=0x%x, BAR1=0x%x\n", bar0, bar1);
    
    // Determine controller type based on programming interface
    uint8_t prog_if = ctrl->pci_dev->prog_if;
    ctrl->controller_type = prog_if;
    
    // Calculate base address
    if (bar0 & 1) {
        // I/O space
        ctrl->base_address = bar0 & ~3;
        DEBUG_SERIAL("USB: Using I/O space at 0x%lx\n", ctrl->base_address);
    } else {
        // Memory space
        ctrl->base_address = bar0 & ~0xF;
        if ((bar0 & 0x6) == 0x4) { // 64-bit BAR
            ctrl->base_address |= ((uint64_t)bar1) << 32;
        }
        
        // Add higher half offset for virtual memory mapping
        extern uint64_t vmm_higher_half_offset;
        ctrl->base_address += vmm_higher_half_offset;
        
        DEBUG_SERIAL("USB: Using memory space at 0x%lx\n", ctrl->base_address);
    }
    
    // Enable bus mastering and memory/I/O space
    uint16_t cmd = pci_device_read(ctrl->pci_dev, 0x04, 2);
    cmd |= 0x06; // Enable memory space and bus mastering
    pci_device_write(ctrl->pci_dev, 0x04, 2, cmd);
    
    // Initialize device list
    ctrl->devices = NULL;
    ctrl->device_count = 0;
    ctrl->max_devices = MAX_USB_DEVICES_PER_CONTROLLER;
    ctrl->initialized = true;
    
    printf("USB: Controller initialized successfully (type=0x%02x)\n", ctrl->controller_type);
    DEBUG_SERIAL("USB: Controller initialized successfully (type=0x%02x)\n", ctrl->controller_type);
    
    return 0;
}

// Legacy UHCI/OHCI/EHCI control transfers are not implemented (use xhci.c).
int usb_send_setup_packet(usb_controller_t *ctrl, uint8_t device_addr, usb_setup_packet_t *setup, void *data) {
    (void)data;
    if (!ctrl || !setup) {
        return -1;
    }
    if (ctrl->controller_type == USB_CONTROLLER_XHCI) {
        return -1; /* xHCI uses xhci.c, not this path */
    }
    DEBUG_SERIAL("USB: legacy control transfer unsupported (type=0x%02x)\n", ctrl->controller_type);
    return -1;
}

// Get device descriptor (placeholder implementation)
int usb_get_device_descriptor(usb_controller_t *ctrl, uint8_t device_addr, usb_device_descriptor_t *desc) {
    if (!ctrl || !desc) {
        return -1;
    }
    
    usb_setup_packet_t setup = {
        .request_type = USB_REQ_DIR_IN | USB_REQ_TYPE_STANDARD,
        .request = USB_REQ_GET_DESCRIPTOR,
        .value = (USB_DESC_DEVICE << 8) | 0,
        .index = 0,
        .length = sizeof(usb_device_descriptor_t)
    };
    
    DEBUG_SERIAL("USB: Getting device descriptor from device %d\n", device_addr);
    (void)setup;
    return -1;
}

// Set device address (placeholder implementation)
int usb_set_device_address(usb_controller_t *ctrl, uint8_t device_addr, uint8_t new_addr) {
    if (!ctrl) {
        return -1;
    }
    
    usb_setup_packet_t setup = {
        .request_type = USB_REQ_DIR_OUT | USB_REQ_TYPE_STANDARD,
        .request = USB_REQ_SET_ADDRESS,
        .value = new_addr,
        .index = 0,
        .length = 0
    };
    
    DEBUG_SERIAL("USB: Setting device address from %d to %d\n", device_addr, new_addr);
    
    return usb_send_setup_packet(ctrl, device_addr, &setup, NULL);
}

// Get configuration descriptor (placeholder implementation)
int usb_get_config_descriptor(usb_controller_t *ctrl, uint8_t device_addr, uint8_t config_index, void *buffer, uint16_t length) {
    if (!ctrl || !buffer) {
        return -1;
    }
    
    usb_setup_packet_t setup = {
        .request_type = USB_REQ_DIR_IN | USB_REQ_TYPE_STANDARD,
        .request = USB_REQ_GET_DESCRIPTOR,
        .value = (USB_DESC_CONFIG << 8) | config_index,
        .index = 0,
        .length = length
    };
    
    DEBUG_SERIAL("USB: Getting config descriptor from device %d\n", device_addr);
    (void)setup;
    (void)buffer;
    (void)length;
    return -1;
}

// Legacy enumeration: xHCI is handled by xhci.c; other host types are not implemented.
int usb_device_enumeration(usb_controller_t *ctrl) {
    if (!ctrl || !ctrl->initialized) {
        return -1;
    }

    if (ctrl->controller_type == USB_CONTROLLER_XHCI) {
        return 0;
    }

    printf("USB: Skipping legacy enumeration on controller type 0x%02x (use xHCI)\n",
           ctrl->controller_type);
    return 0;
}

// USB Mass Storage initialization — not fully implemented
int usb_mass_storage_init(usb_device_t *device) {
    if (!device || device->device_class != USB_CLASS_MASS_STORAGE) {
        return -1;
    }
    printf("USB: mass storage init stub (no BOT transport)\n");
    return -1;
}

// USB Mass Storage read sectors — returns error until BOT protocol is implemented
int usb_mass_storage_read_sectors(storage_device_t *dev, uint64_t lba, uint32_t sector_count, void *buffer) {
    if (!dev || !buffer) {
        return -1;
    }
    (void)lba;
    (void)sector_count;
    printf("USB: mass storage read unsupported (needs BOT over xHCI)\n");
    return -1;
}

// USB Mass Storage write sectors
int usb_mass_storage_write_sectors(storage_device_t *dev, uint64_t lba, uint32_t sector_count, const void *buffer) {
    if (!dev || !buffer) {
        return -1;
    }
    (void)lba;
    (void)sector_count;
    printf("USB: mass storage write unsupported (needs BOT over xHCI)\n");
    return -1;
}

// USB Mass Storage flush
int usb_mass_storage_flush(storage_device_t *dev) {
    if (!dev) {
        return -1;
    }
    return -1;
}

// USB HID initialization
int usb_hid_init(usb_device_t *device) {
    if (!device || device->device_class != USB_CLASS_HID) {
        return -1;
    }
    
    printf("USB: Initializing HID device\n");
    DEBUG_SERIAL("USB: Initializing HID device\n");
    
    // Create HID device structure
    usb_hid_device_t *hid_device = kalloc(sizeof(usb_hid_device_t));
    if (!hid_device) {
        DEBUG_SERIAL("USB: Failed to allocate HID device structure\n");
        return -1;
    }
    
    memset(hid_device, 0, sizeof(usb_hid_device_t));
    hid_device->usb_device = device;
    hid_device->protocol = 0;
    hid_device->idle_rate = 0;
    hid_device->boot_protocol = false;
    
    // Add to HID device list
    hid_device->next = usb_hid_devices;
    usb_hid_devices = hid_device;
    usb_hid_device_count++;
    
    // Initialize based on protocol
    if (device->device_protocol == USB_HID_PROTOCOL_KEYBOARD) {
        return usb_keyboard_init(device);
    } else if (device->device_protocol == USB_HID_PROTOCOL_MOUSE) {
        return usb_mouse_init(device);
    }
    
    return 0;
}

// USB HID read report
int usb_hid_read_report(usb_device_t *device, void *buffer, uint16_t length) {
    if (!device || !buffer) {
        return -1;
    }
    
    USB_DEBUG("USB: HID read report request - length %u\n", length);
    
    // Find HID device
    usb_hid_device_t *hid_device = usb_hid_devices;
    while (hid_device) {
        if (hid_device->usb_device == device) {
            break;
        }
        hid_device = hid_device->next;
    }
    
    if (!hid_device) {
        DEBUG_SERIAL("USB: HID device not found\n");
        return -1;
    }
    
    (void)length;
    (void)buffer;
    return -1;
}

// USB HID set protocol
int usb_hid_set_protocol(usb_device_t *device, uint8_t protocol) {
    if (!device) {
        return -1;
    }
    
    usb_setup_packet_t setup = {
        .request_type = USB_REQ_DIR_OUT | USB_REQ_TYPE_CLASS,
        .request = USB_HID_REQ_SET_PROTOCOL,
        .value = protocol,
        .index = 0,
        .length = 0
    };
    
    DEBUG_SERIAL("USB: Setting HID protocol to %d\n", protocol);
    
    return usb_send_setup_packet(device->controller, device->address, &setup, NULL);
}

// USB HID set idle
int usb_hid_set_idle(usb_device_t *device, uint8_t idle_rate) {
    if (!device) {
        return -1;
    }
    
    usb_setup_packet_t setup = {
        .request_type = USB_REQ_DIR_OUT | USB_REQ_TYPE_CLASS,
        .request = USB_HID_REQ_SET_IDLE,
        .value = idle_rate,
        .index = 0,
        .length = 0
    };
    
    DEBUG_SERIAL("USB: Setting HID idle rate to %d\n", idle_rate);
    
    return usb_send_setup_packet(device->controller, device->address, &setup, NULL);
}

// USB HID get report descriptor
int usb_hid_get_report_descriptor(usb_device_t *device, void *buffer, uint16_t length) {
    if (!device || !buffer) {
        return -1;
    }
    
    usb_setup_packet_t setup = {
        .request_type = USB_REQ_DIR_IN | USB_REQ_TYPE_STANDARD,
        .request = USB_REQ_GET_DESCRIPTOR,
        .value = (USB_DESC_STRING << 8) | 0x22, // HID Report Descriptor
        .index = 0,
        .length = length
    };
    
    DEBUG_SERIAL("USB: Getting HID report descriptor\n");
    
    return usb_send_setup_packet(device->controller, device->address, &setup, buffer);
}

// USB Keyboard initialization
int usb_keyboard_init(usb_device_t *device) {
    if (!device || device->device_protocol != USB_HID_PROTOCOL_KEYBOARD) {
        return -1;
    }
    
    printf("USB: Initializing USB keyboard\n");
    DEBUG_SERIAL("USB: Initializing USB keyboard\n");
    
    // Set boot protocol for keyboard
    if (usb_hid_set_protocol(device, 0) != 0) {
        DEBUG_SERIAL("USB: Failed to set keyboard protocol\n");
        return -1;
    }
    
    // Set idle rate (0 = no idle)
    if (usb_hid_set_idle(device, 0) != 0) {
        DEBUG_SERIAL("USB: Failed to set keyboard idle rate\n");
        return -1;
    }
    
    printf("USB: Keyboard initialized successfully\n");
    DEBUG_SERIAL("USB: Keyboard initialized successfully\n");
    
    return 0;
}

// USB Keyboard read report
int usb_keyboard_read_report(usb_device_t *device, usb_keyboard_report_t *report) {
    if (!device || !report) {
        return -1;
    }
    
    // Read keyboard report (8 bytes for boot protocol)
    return usb_hid_read_report(device, report, sizeof(usb_keyboard_report_t));
}

// USB Keyboard process report
int usb_keyboard_process_report(usb_keyboard_report_t *report) {
    if (!report) {
        return -1;
    }
    
    USB_DEBUG("USB: Processing keyboard report - modifiers: 0x%02x, keys: %02x %02x %02x %02x %02x %02x\n",
                 report->modifier_keys, report->key_codes[0], report->key_codes[1], 
                 report->key_codes[2], report->key_codes[3], report->key_codes[4], report->key_codes[5]);
    
    // Check for modifier keys
    bool ctrl_pressed = (report->modifier_keys & 0x01) != 0;
    bool shift_pressed = (report->modifier_keys & 0x02) != 0;
    bool alt_pressed = (report->modifier_keys & 0x04) != 0;
    bool gui_pressed = (report->modifier_keys & 0x08) != 0;
    
    // Process each key code
    for (int i = 0; i < 6; i++) {
        uint8_t scancode = report->key_codes[i];
        if (scancode == 0) continue; // No key pressed
        
        // Convert scancode to ASCII
        uint8_t ascii = usb_keyboard_scancode_to_ascii(scancode, shift_pressed);
        
        if (ascii != 0) {
            // Send to keyboard subsystem
            // For now, just print the character
            printf("USB Keyboard: scancode=0x%02x, ascii='%c' (0x%02x)\n", 
                   scancode, ascii, ascii);
            
            // In a real implementation, this would call the keyboard subsystem
            // keyboard_handle_input(ascii, ctrl_pressed, shift_pressed, alt_pressed);
        }
    }
    
    return 0;
}

// USB Keyboard scancode to ASCII conversion
uint8_t usb_keyboard_scancode_to_ascii(uint8_t scancode, bool shift_pressed) {
    if (scancode >= 128) {
        return 0; // Invalid scancode
    }
    
    if (shift_pressed) {
        return usb_keyboard_shift_scancode_table[scancode];
    } else {
        return usb_keyboard_scancode_table[scancode];
    }
}

// Main USB initialization function
void usb_init() {
    // Idempotent: may be reached via both the driver_manager USB adapter
    // (auto-discover) and the direct kernel.c boot call.
    static bool usb_subsystem_initialized = false;
    if (usb_subsystem_initialized) {
        return;
    }
    usb_subsystem_initialized = true;

    printf("USB: Starting USB subsystem initialization\n");
    DEBUG_SERIAL("USB: Starting USB subsystem initialization\n");
    
    // Find USB controllers
    for (uint8_t i = 0; i < pci_device_count; i++) {
        struct pci_device *pci_dev = pci_devices[i];
        
        /* Serial-bus / USB host (UHCI/OHCI/EHCI/xHCI). 0x05 is SMBus. */
        if (pci_dev->class_code == 0x0C && pci_dev->subclass_code == 0x03) {
            // Found a USB controller
            if (usb_controller_count >= MAX_USB_CONTROLLERS) {
                printf("USB: Maximum number of controllers reached\n");
                break;
            }
            
            printf("USB: Found controller at bus %d, device %d, func %d\n",
                   pci_dev->bus, pci_dev->device, pci_dev->function);
            
            usb_controller_t *ctrl = &usb_controllers[usb_controller_count];
            memset(ctrl, 0, sizeof(usb_controller_t));
            ctrl->pci_dev = pci_dev;
            
            if (usb_controller_init(ctrl) == 0) {
                usb_controller_count++;
                printf("USB: Controller %d initialized successfully\n", usb_controller_count);
            } else {
                printf("USB: Failed to initialize controller\n");
            }
        }
    }
    
    printf("USB: Found %d USB controller(s)\n", usb_controller_count);
    DEBUG_SERIAL("USB: Found %d USB controller(s)\n", usb_controller_count);
    
    // Enumerate devices on each controller
    for (uint8_t i = 0; i < usb_controller_count; i++) {
        usb_controller_t *ctrl = &usb_controllers[i];
        if (ctrl->initialized) {
            usb_device_enumeration(ctrl);
        }
    }
    
    printf("USB: USB subsystem initialization complete\n");
    DEBUG_SERIAL("USB: USB subsystem initialization complete\n");
}

// Process keyboard report and add characters to buffer
static void usb_keyboard_process_report_to_buffer(usb_keyboard_report_t *report) {
    if (!report) return;
    
    spinlock_lock(&usb_keyboard_lock);
    
    // Check for modifier keys
    bool ctrl_pressed = (report->modifier_keys & 0x01) != 0;
    bool shift_pressed = (report->modifier_keys & 0x02) != 0;
    bool alt_pressed = (report->modifier_keys & 0x04) != 0;
    
    // Process each key code
    for (int i = 0; i < 6; i++) {
        uint8_t scancode = report->key_codes[i];
        if (scancode == 0) continue;
        
        /* Ctrl+C: scancode 0x06 = 'c'.  Route through the per-VC flags
         * (matching the PS/2 time-window logic for double-press force-kill). */
        if (ctrl_pressed && scancode == 0x06) {
            extern void set_terminate_flag(bool);
            extern void set_force_kill_flag(bool);
            extern bool get_terminate_flag(void);
            extern uint64_t hpet_get_ticks(void);
            extern uint64_t hpet_get_frequency(void);

            uint64_t now = hpet_get_ticks();
            vconsole_t *vc = vconsole_get_by_id(vconsole_get_active());
            if (vc->terminate_flag) {
                uint64_t elapsed_ms = now - vc->last_ctrlc_tick;
                if (elapsed_ms < 2000) {
                    set_force_kill_flag(true);
                }
            }
            set_terminate_flag(true);
            vc->last_ctrlc_tick = now;
            continue;
        }

        uint8_t ascii = usb_keyboard_scancode_to_ascii(scancode, shift_pressed);
        
        if (ascii != 0) {
            uint32_t next_head = (usb_keyboard_buffer_head + 1) % USB_KEYBOARD_BUFFER_SIZE;
            if (next_head != usb_keyboard_buffer_tail) {
                usb_keyboard_buffer[usb_keyboard_buffer_head] = ascii;
                usb_keyboard_buffer_head = next_head;
                usb_keyboard_has_data = true;
                
                USB_DEBUG("USB Keyboard: Added '%c' (0x%02x) to buffer\n", ascii, ascii);
            }
        }
    }
    
    spinlock_unlock(&usb_keyboard_lock);
}

// USB Keyboard polling and integration functions
void usb_keyboard_poll_all() {
    // Poll all USB keyboards for input
    uint8_t controller_count = usb_get_controller_count();
    
    for (uint8_t i = 0; i < controller_count; i++) {
        usb_controller_t *ctrl = usb_get_controller(i);
        if (!ctrl) continue;
        
        usb_device_t *device = ctrl->devices;
        while (device) {
            if (device->device_class == USB_CLASS_HID && 
                device->device_protocol == USB_HID_PROTOCOL_KEYBOARD) {
                
                // Read keyboard report
                usb_keyboard_report_t report;
                if (usb_keyboard_read_report(device, &report) == 0) {
                    // Process the report and add to buffer
                    usb_keyboard_process_report_to_buffer(&report);
                }
            }
            device = device->next;
        }
    }
}

// Check if USB keyboard has input
bool usb_keyboard_has_input() {
    spinlock_lock(&usb_keyboard_lock);
    bool has_input = usb_keyboard_has_data && (usb_keyboard_buffer_head != usb_keyboard_buffer_tail);
    spinlock_unlock(&usb_keyboard_lock);
    return has_input;
}

// Get a character from USB keyboard buffer
char usb_keyboard_get_char() {
    spinlock_lock(&usb_keyboard_lock);
    
    if (!usb_keyboard_has_data || usb_keyboard_buffer_head == usb_keyboard_buffer_tail) {
        spinlock_unlock(&usb_keyboard_lock);
        return 0; // No data available
    }
    
    char c = usb_keyboard_buffer[usb_keyboard_buffer_tail];
    usb_keyboard_buffer_tail = (usb_keyboard_buffer_tail + 1) % USB_KEYBOARD_BUFFER_SIZE;
    
    if (usb_keyboard_buffer_head == usb_keyboard_buffer_tail) {
        usb_keyboard_has_data = false;
    }
    
    spinlock_unlock(&usb_keyboard_lock);
    return c;
}

// Integrate USB keyboard with the system
void usb_keyboard_integrate_with_system() {
    static uint64_t last_poll_time = 0;
    uint64_t current_time = hpet_get_ticks();
    
    // Rate limit polling to every 5ms (200Hz) to reduce spam but maintain responsiveness
    if (current_time - last_poll_time < 5000) {
        return;
    }
    
    last_poll_time = current_time;
    
    // This function can be called periodically to poll USB keyboards
    // and integrate with the existing keyboard system
    usb_keyboard_poll_all();
}

// USB Mouse initialization
int usb_mouse_init(usb_device_t *device) {
    if (!device || device->device_protocol != USB_HID_PROTOCOL_MOUSE) {
        return -1;
    }
    
    printf("USB: Initializing USB mouse\n");
    DEBUG_SERIAL("USB: Initializing USB mouse\n");
    
    // Set boot protocol for mouse
    if (usb_hid_set_protocol(device, 0) != 0) {
        DEBUG_SERIAL("USB: Failed to set mouse protocol\n");
        return -1;
    }
    
    // Set idle rate (0 = no idle)
    if (usb_hid_set_idle(device, 0) != 0) {
        DEBUG_SERIAL("USB: Failed to set mouse idle rate\n");
        return -1;
    }
    
    printf("USB: Mouse initialized successfully\n");
    DEBUG_SERIAL("USB: Mouse initialized successfully\n");
    
    return 0;
}

// USB Mouse read report
int usb_mouse_read_report(usb_device_t *device, usb_mouse_report_t *report) {
    if (!device || !report) {
        return -1;
    }
    
    // Read mouse report (3 bytes for boot protocol)
    return usb_hid_read_report(device, report, sizeof(usb_mouse_report_t));
}

// USB Mouse process report
int usb_mouse_process_report(usb_mouse_report_t *report) {
    if (!report) {
        return -1;
    }
    
    USB_DEBUG("USB: Processing mouse report - buttons: 0x%02x, dx: %d, dy: %d\n",
              report->buttons, report->x_movement, report->y_movement);

    mouse_feed_usb_relative(report->x_movement, report->y_movement, report->buttons);
    return 0;
}

// USB Mouse polling - check all USB mice for input
void usb_mouse_poll_all() {
    // Poll all USB mice for input
    uint8_t controller_count = usb_get_controller_count();
    
    for (uint8_t i = 0; i < controller_count; i++) {
        usb_controller_t *ctrl = usb_get_controller(i);
        if (!ctrl) continue;
        
        usb_device_t *device = ctrl->devices;
        while (device) {
            if (device->device_class == USB_CLASS_HID && 
                device->device_protocol == USB_HID_PROTOCOL_MOUSE) {
                
                // Read mouse report
                usb_mouse_report_t report;
                if (usb_mouse_read_report(device, &report) == 0) {
                    // Process the report
                    usb_mouse_process_report(&report);
                }
            }
            device = device->next;
        }
    }
} 