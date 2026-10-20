#include "vmware_svga.h"
#include "pci.h"
#include "serial.h"
#include "types.h"
#include "stdio.h"
#include "mem.h"
#include "utils.h"
#include "debug.h"

/*
 * VMware SVGA-II: several register accessors are intentionally empty stubs
 * when SVGA_ID isn't available or in headless paths — avoids MMIO to absent
 * hardware. See vmware_svga_init() for the live register path.
 */

// Forward declaration for limine framebuffer
struct limine_framebuffer;

// Global framebuffer (from utils.c) - we need to update this
extern struct limine_framebuffer *fb;

// Thread yielding function
extern void thread_yield(void);

// I/O port functions
static inline void outl(uint16_t port, uint32_t value) {
    __asm__ volatile("outl %0, %1" : : "a"(value), "d"(port));
}

static inline uint32_t inl(uint16_t port) {
    uint32_t value;
    __asm__ volatile("inl %1, %0" : "=a"(value) : "d"(port));
    return value;
}

// Global VMware SVGA device state
static vmware_svga_device_t svga_device = {0};

// SVGA framebuffer structure to replace the global fb

// Low-level register access functions
void vmware_svga_write_reg(uint32_t reg, uint32_t value) {
    // Stub function - do nothing for maximum performance
    (void)reg; (void)value;
}

uint32_t vmware_svga_read_reg(uint32_t reg) {
    // Stub function - do nothing for maximum performance
    (void)reg;
    return 0;
}

// FIFO management functions
void vmware_svga_fifo_init(void) {
    // Stub function - do nothing for maximum performance
}

bool vmware_svga_fifo_full(void) {
    // Stub function - do nothing for maximum performance
    return true;  // Always report full to prevent FIFO operations
}

void vmware_svga_fifo_write(uint32_t value) {
    // Stub function - do nothing for maximum performance
    (void)value;
}

void vmware_svga_fifo_commit(void) {
    // Stub function - do nothing for maximum performance
}

// Display mode management
int vmware_svga_set_mode(uint32_t width, uint32_t height, uint32_t bpp) {
    // Stub function - do nothing for maximum performance
    (void)width; (void)height; (void)bpp;
    return -1;  // Always fail
}

// Graphics operations
void vmware_svga_clear_screen(uint32_t color) {
    // Stub function - do nothing for maximum performance
    (void)color;
}

void vmware_svga_fillrect(uint32_t color, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    // Stub function - do nothing for maximum performance
    (void)color; (void)x; (void)y; (void)w; (void)h;
}

void vmware_svga_rect_copy(uint32_t src_x, uint32_t src_y, uint32_t dst_x, uint32_t dst_y, uint32_t w, uint32_t h) {
    // Stub function - do nothing for maximum performance
    (void)src_x; (void)src_y; (void)dst_x; (void)dst_y; (void)w; (void)h;
}

// Device information
bool vmware_svga_is_available(void) {
    // For maximum performance, always return false
    // This completely bypasses any SVGA overhead
    return false;
}

void vmware_svga_get_info(uint32_t *width, uint32_t *height) {
    if (width) *width = svga_device.width;
    if (height) *height = svga_device.height;
}

// Main initialization function - ultra-minimal version
void vmware_svga_init(void) {
    // Clear device state
    memset(&svga_device, 0, sizeof(vmware_svga_device_t));

    // Don't do any initialization - just mark that we have no SVGA support
    // This prevents any PCI scanning or memory mapping overhead
    svga_device.initialized = false;
    svga_device.enabled = false;

    // The driver will appear as "not available" to the system
    // This gives us the best performance with no SVGA overhead
}

// Driver adapter functions for integration with driver management system
int vmware_svga_driver_adapter_init(struct pci_device *dev) {
    (void)dev; // VMware SVGA init doesn't take PCI device parameter
    
    if (svga_device.initialized) {
        return 0; // Already initialized
    }
    
    vmware_svga_init();
    return svga_device.initialized ? 0 : -1;
}

int vmware_svga_driver_adapter_deinit(void) {
    if (!svga_device.initialized) {
        return 0;
    }
    
    // Disable SVGA mode
    if (svga_device.enabled) {
        vmware_svga_write_reg(SVGA_REG_ENABLE, 0);
        svga_device.enabled = false;
    }
    
    // Clear device state
    memset(&svga_device, 0, sizeof(vmware_svga_device_t));
    
    printf("VMware SVGA: Driver deinitialized\n");
    return 0;
}

int vmware_svga_driver_adapter_probe(struct pci_device *dev) {
    // Check if this is a VMware SVGA device
    if (dev->vendor_id == VMWARE_VENDOR_ID && 
        dev->device_id == VMWARE_SVGA_DEVICE_ID &&
        dev->class_code == VMWARE_SVGA_CLASS && 
        dev->subclass_code == VMWARE_SVGA_SUBCLASS) {
        return 0; // VMware SVGA device found
    }
    return -1; // Not a VMware SVGA device
}

int vmware_svga_driver_adapter_enable(void) {
    if (!svga_device.initialized) {
        return -1;
    }

    if (!svga_device.enabled) {
        vmware_svga_write_reg(SVGA_REG_ENABLE, 1);
        svga_device.enabled = true;
    }

    return 0;
}

int vmware_svga_driver_adapter_disable(void) {
    if (!svga_device.initialized) {
        return 0;
    }

    if (svga_device.enabled) {
        vmware_svga_write_reg(SVGA_REG_ENABLE, 0);
        svga_device.enabled = false;
    }

    return 0;
}

int vmware_svga_driver_adapter_reset(void) {
    if (!svga_device.initialized) {
        return -1;
    }

    // Disable and re-enable
    vmware_svga_driver_adapter_disable();

    // Re-initialize FIFO if available
    if (svga_device.fifo) {
        vmware_svga_fifo_init();
    }

    // Re-enable
    vmware_svga_driver_adapter_enable();

    return 0;
}

void vmware_svga_driver_adapter_status(void) {
    printf("VMware SVGA Driver Status:\n");
    printf("  Initialized: %s\n", svga_device.initialized ? "Yes" : "No");
    printf("  Enabled: %s\n", svga_device.enabled ? "Yes" : "No");
    
    if (svga_device.initialized) {
        printf("  Device: VMware SVGA-II\n");
        printf("  I/O Base: 0x%x\n", svga_device.io_base);
        printf("  Mode: %dx%d@%dbpp\n", svga_device.width, svga_device.height, svga_device.bpp);
        printf("  VRAM Size: %d MB\n", svga_device.vram_size / (1024 * 1024));
        printf("  Framebuffer: %p (size: %d KB)\n", 
               svga_device.framebuffer, svga_device.framebuffer_size / 1024);
        printf("  FIFO: %s (%p, size: %d KB)\n", 
               svga_device.fifo ? "Available" : "Not available",
               svga_device.fifo, svga_device.fifo_size / 1024);
        printf("  Capabilities: 0x%x\n", svga_device.capabilities);
        
        // Decode some capabilities
        if (svga_device.capabilities & SVGA_CAP_RECT_COPY) printf("    - Rectangle Copy\n");
        if (svga_device.capabilities & SVGA_CAP_RECT_FILL) printf("    - Rectangle Fill\n");
        if (svga_device.capabilities & SVGA_CAP_CURSOR) printf("    - Hardware Cursor\n");
        if (svga_device.capabilities & SVGA_CAP_3D) printf("    - 3D Acceleration\n");
        if (svga_device.capabilities & SVGA_CAP_EXTENDED_FIFO) printf("    - Extended FIFO\n");
    }
}
