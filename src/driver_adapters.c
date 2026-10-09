#include "driver_adapters.h"
/*
 * Adapter deinit/disable/reset: intentionally left as no-ops where the underlying
 * driver (nvme, ahci, …) does not yet support safe teardown — hardware stays active.
 */
#include "driver_manager.h"
#include "nvme.h"
#include "ahci.h"
#include "ide.h"
#include "e1000.h"
#include "rtl8139.h"
#include "amd_gpu.h"
#include "usb.h"
#include "mem.h"
#include "string.h"
#include "serial.h"
#include "rtl810x.h"
#include "vmware_svga.h"
#include "pcnet.h"

// Global flags to track driver states
static bool nvme_initialized = false;
static bool ahci_initialized = false;
static bool ide_initialized = false;
static bool e1000_initialized = false;
static bool rtl8139_initialized = false;
static bool amd_gpu_initialized = false;
static bool vmware_svga_initialized = false;
static bool usb_initialized = false;
static bool pcnet_initialized = false;

// NVMe driver adapter
int nvme_driver_adapter_init(struct pci_device *dev) {
    (void)dev; // NVMe init doesn't take PCI device parameter
    
    if (nvme_initialized) {
        return 0; // Already initialized
    }
    
    nvme_init();
    nvme_initialized = true;
    return 0;
}

int nvme_driver_adapter_deinit(void) {
    if (!nvme_initialized) {
        return 0;
    }
    
    // TODO: Implement NVMe deinit
    nvme_initialized = false;
    return 0;
}

int nvme_driver_adapter_probe(struct pci_device *dev) {
    // Check if this is an NVMe device
    if (dev->class_code == 0x01 && dev->subclass_code == 0x08) {
        return 0; // NVMe device found
    }
    return -1; // Not an NVMe device
}

int nvme_driver_adapter_enable(void) {
    if (!nvme_initialized) {
        return -1;
    }
    // NVMe is enabled by default after init
    return 0;
}

int nvme_driver_adapter_disable(void) {
    if (!nvme_initialized) {
        return 0;
    }
    // TODO: Implement NVMe disable
    return 0;
}

int nvme_driver_adapter_reset(void) {
    if (!nvme_initialized) {
        return -1;
    }
    // TODO: Implement NVMe reset
    return 0;
}

void nvme_driver_adapter_status(void) {
    printf("NVMe Driver Status:\n");
    printf("  Initialized: %s\n", nvme_initialized ? "Yes" : "No");
    // TODO: Add more status information
}

// AHCI driver adapter
int ahci_driver_adapter_init(struct pci_device *dev) {
    (void)dev; // AHCI init doesn't take PCI device parameter
    
    if (ahci_initialized) {
        return 0; // Already initialized
    }
    
    ahci_init();
    ahci_initialized = true;
    return 0;
}

int ahci_driver_adapter_deinit(void) {
    if (!ahci_initialized) {
        return 0;
    }
    
    // TODO: Implement AHCI deinit
    ahci_initialized = false;
    return 0;
}

int ahci_driver_adapter_probe(struct pci_device *dev) {
    // Check if this is an AHCI device
    if (dev->class_code == 0x01 && dev->subclass_code == 0x06) {
        return 0; // AHCI device found
    }
    return -1; // Not an AHCI device
}

int ahci_driver_adapter_enable(void) {
    if (!ahci_initialized) {
        return -1;
    }
    // AHCI is enabled by default after init
    return 0;
}

int ahci_driver_adapter_disable(void) {
    if (!ahci_initialized) {
        return 0;
    }
    // TODO: Implement AHCI disable
    return 0;
}

int ahci_driver_adapter_reset(void) {
    if (!ahci_initialized) {
        return -1;
    }
    // TODO: Implement AHCI reset
    return 0;
}

void ahci_driver_adapter_status(void) {
    printf("AHCI Driver Status:\n");
    printf("  Initialized: %s\n", ahci_initialized ? "Yes" : "No");
    // TODO: Add more status information
}

// IDE driver adapter
int ide_driver_adapter_init(struct pci_device *dev) {
    (void)dev; // IDE init doesn't take PCI device parameter
    
    if (ide_initialized) {
        return 0; // Already initialized
    }
    
    ide_init();
    ide_initialized = true;
    return 0;
}

int ide_driver_adapter_deinit(void) {
    if (!ide_initialized) {
        return 0;
    }
    
    // TODO: Implement IDE deinit
    ide_initialized = false;
    return 0;
}

int ide_driver_adapter_probe(struct pci_device *dev) {
    // Check if this is an IDE device
    if (dev->class_code == 0x01 && dev->subclass_code == 0x01) {
        return 0; // IDE device found
    }
    return -1; // Not an IDE device
}

int ide_driver_adapter_enable(void) {
    if (!ide_initialized) {
        return -1;
    }
    // IDE is enabled by default after init
    return 0;
}

int ide_driver_adapter_disable(void) {
    if (!ide_initialized) {
        return 0;
    }
    // TODO: Implement IDE disable
    return 0;
}

int ide_driver_adapter_reset(void) {
    if (!ide_initialized) {
        return -1;
    }
    // TODO: Implement IDE reset
    return 0;
}

void ide_driver_adapter_status(void) {
    printf("IDE Driver Status:\n");
    printf("  Initialized: %s\n", ide_initialized ? "Yes" : "No");
    // TODO: Add more status information
}

// E1000 driver adapter
int e1000_driver_adapter_init(struct pci_device *dev) {
    (void)dev; // E1000 init doesn't take PCI device parameter
    
    if (e1000_initialized) {
        return 0; // Already initialized
    }
    
    e1000_init();
    e1000_initialized = true;
    return 0;
}

int e1000_driver_adapter_deinit(void) {
    if (!e1000_initialized) {
        return 0;
    }
    
    // TODO: Implement E1000 deinit
    e1000_initialized = false;
    return 0;
}

int e1000_driver_adapter_probe(struct pci_device *dev) {
    // Check if this is an E1000 device
    if (dev->vendor_id == 0x8086 && 
        (dev->device_id == 0x100E || dev->device_id == 0x100F || dev->device_id == 0x153A)) {
        return 0; // E1000 device found
    }
    return -1; // Not an E1000 device
}

int e1000_driver_adapter_enable(void) {
    if (!e1000_initialized) {
        return -1;
    }
    // E1000 is enabled by default after init
    return 0;
}

int e1000_driver_adapter_disable(void) {
    if (!e1000_initialized) {
        return 0;
    }
    // TODO: Implement E1000 disable
    return 0;
}

int e1000_driver_adapter_reset(void) {
    if (!e1000_initialized) {
        return -1;
    }
    // TODO: Implement E1000 reset
    return 0;
}

void e1000_driver_adapter_status(void) {
    printf("E1000 Driver Status:\n");
    printf("  Initialized: %s\n", e1000_initialized ? "Yes" : "No");
    // TODO: Add more status information
}

// RTL8139 driver adapter
int rtl8139_driver_adapter_init(struct pci_device *dev) {
    (void)dev; // RTL8139 init doesn't take PCI device parameter
    
    if (rtl8139_initialized) {
        return 0; // Already initialized
    }
    
    rtl8139_init();
    rtl8139_initialized = true;
    return 0;
}

int rtl8139_driver_adapter_deinit(void) {
    if (!rtl8139_initialized) {
        return 0;
    }
    
    // TODO: Implement RTL8139 deinit
    rtl8139_initialized = false;
    return 0;
}

int rtl8139_driver_adapter_probe(struct pci_device *dev) {
    // Check if this is an RTL8139 device
    if (dev->vendor_id == 0x10EC && dev->device_id == 0x8139) {
        return 0; // RTL8139 device found
    }
    return -1; // Not an RTL8139 device
}

int rtl8139_driver_adapter_enable(void) {
    if (!rtl8139_initialized) {
        return -1;
    }
    // RTL8139 is enabled by default after init
    return 0;
}

int rtl8139_driver_adapter_disable(void) {
    if (!rtl8139_initialized) {
        return 0;
    }
    // TODO: Implement RTL8139 disable
    return 0;
}

int rtl8139_driver_adapter_reset(void) {
    if (!rtl8139_initialized) {
        return -1;
    }
    // TODO: Implement RTL8139 reset
    return 0;
}

void rtl8139_driver_adapter_status(void) {
    printf("RTL8139 Driver Status:\n");
    printf("  Initialized: %s\n", rtl8139_initialized ? "Yes" : "No");
    // TODO: Add more status information
}

// RTL810xE driver adapter
static bool rtl810x_initialized = false;

int rtl810x_driver_adapter_init(struct pci_device *dev) {
    (void)dev;
    if (rtl810x_initialized) return 0;
    rtl810x_init();
    rtl810x_initialized = rtl810x_dev.initialized;
    return 0;
}

int rtl810x_driver_adapter_deinit(void) {
    if (!rtl810x_initialized) return 0;
    // TODO: add proper shutdown
    rtl810x_initialized = false;
    return 0;
}

int rtl810x_driver_adapter_probe(struct pci_device *dev) {
    if (dev->vendor_id == 0x10EC && dev->device_id == 0x8136) return 0;
    return -1;
}

int rtl810x_driver_adapter_enable(void) {
    if (!rtl810x_initialized) return -1;
    return 0;
}

int rtl810x_driver_adapter_disable(void) {
    if (!rtl810x_initialized) return 0;
    return 0;
}

int rtl810x_driver_adapter_reset(void) {
    if (!rtl810x_initialized) return -1;
    return 0;
}

void rtl810x_driver_adapter_status(void) {
    printf("RTL810xE Driver Status:\n");
    printf("  Initialized: %s\n", rtl810x_initialized ? "Yes" : "No");
}

// AMD GPU driver adapter
int amd_gpu_driver_adapter_init(struct pci_device *dev) {
    (void)dev; // AMD GPU init doesn't take PCI device parameter
    
    if (amd_gpu_initialized) {
        return 0; // Already initialized
    }
    
    amd_gpu_init();
    amd_gpu_initialized = true;
    return 0;
}

int amd_gpu_driver_adapter_deinit(void) {
    if (!amd_gpu_initialized) {
        return 0;
    }
    
    // TODO: Implement AMD GPU deinit
    amd_gpu_initialized = false;
    return 0;
}

int amd_gpu_driver_adapter_probe(struct pci_device *dev) {
#if !ENABLE_AMD_GPU
    (void)dev;
    return -1; /* driver compiled out: never bind to AMD VGA devices */
#endif
    // Check if this is an AMD GPU device
    if (dev->vendor_id == 0x1002 && dev->class_code == 0x03 && dev->subclass_code == 0x00) {
        return 0; // AMD GPU device found
    }
    return -1; // Not an AMD GPU device
}

int amd_gpu_driver_adapter_enable(void) {
    if (!amd_gpu_initialized) {
        return -1;
    }
    // AMD GPU is enabled by default after init
    return 0;
}

int amd_gpu_driver_adapter_disable(void) {
    if (!amd_gpu_initialized) {
        return 0;
    }
    // TODO: Implement AMD GPU disable
    return 0;
}

int amd_gpu_driver_adapter_reset(void) {
    if (!amd_gpu_initialized) {
        return -1;
    }
    // TODO: Implement AMD GPU reset
    return 0;
}

void amd_gpu_driver_adapter_status(void) {
    printf("AMD GPU Driver Status:\n");
    printf("  Initialized: %s\n", amd_gpu_initialized ? "Yes" : "No");
    // TODO: Add more status information
}

// USB driver adapter
int usb_driver_adapter_init(struct pci_device *dev) {
    (void)dev; // USB init doesn't take PCI device parameter
    
    if (usb_initialized) {
        return 0; // Already initialized
    }
    
    usb_init();
    usb_initialized = true;
    return 0;
}

int usb_driver_adapter_deinit(void) {
    if (!usb_initialized) {
        return 0;
    }
    
    // TODO: Implement USB deinit
    usb_initialized = false;
    return 0;
}

int usb_driver_adapter_probe(struct pci_device *dev) {
    // Check if this is a USB controller device
    if (dev->class_code == 0x0C && dev->subclass_code == 0x03) {
        return 0; // USB controller device found
    }
    return -1; // Not a USB controller device
}

int usb_driver_adapter_enable(void) {
    if (!usb_initialized) {
        return -1;
    }
    // USB is enabled by default after init
    return 0;
}

int usb_driver_adapter_disable(void) {
    if (!usb_initialized) {
        return 0;
    }
    // TODO: Implement USB disable
    return 0;
}

int usb_driver_adapter_reset(void) {
    if (!usb_initialized) {
        return -1;
    }
    // TODO: Implement USB reset
    return 0;
}

void usb_driver_adapter_status(void) {
    printf("USB Driver Status:\n");
    printf("  Initialized: %s\n", usb_initialized ? "Yes" : "No");
    // TODO: Add more status information
}

// PCnet driver adapter
int pcnet_driver_adapter_init(struct pci_device *dev) {
    (void)dev;
    if (pcnet_initialized) return 0;
    pcnet_init();
    pcnet_initialized = pcnet_dev.initialized;
    return 0;
}

int pcnet_driver_adapter_deinit(void) {
    if (!pcnet_initialized) return 0;
    pcnet_initialized = false;
    return 0;
}

int pcnet_driver_adapter_probe(struct pci_device *dev) {
    if (dev->vendor_id == PCNET_VENDOR_ID && dev->device_id == PCNET_DEVICE_ID) return 0;
    return -1;
}

int pcnet_driver_adapter_enable(void) {
    if (!pcnet_initialized) return -1;
    return 0;
}

int pcnet_driver_adapter_disable(void) {
    if (!pcnet_initialized) return 0;
    return 0;
}

int pcnet_driver_adapter_reset(void) {
    if (!pcnet_initialized) return -1;
    return 0;
}

void pcnet_driver_adapter_status(void) {
    printf("PCnet Driver Status:\n");
    printf("  Initialized: %s\n", pcnet_initialized ? "Yes" : "No");
}

// Initialize all driver adapters
void driver_adapters_init(void) {
    DEBUG_SERIAL("Driver adapters: Initializing\n");
    
    // Reset all initialization flags
    nvme_initialized = false;
    ahci_initialized = false;
    ide_initialized = false;
    e1000_initialized = false;
    rtl8139_initialized = false;
    amd_gpu_initialized = false;
    usb_initialized = false;
    pcnet_initialized = false;
    
    printf("Driver adapters initialized\n");
    DEBUG_SERIAL("Driver adapters: Initialization complete\n");
}

// Register all driver adapters with the driver manager
int driver_adapters_register_all(void) {
    DEBUG_SERIAL("Driver adapters: Registering all adapters\n");
    
    // NVMe driver registration
    driver_registration_t nvme_reg = {
        .info = {
            .name = "NVMe Driver",
            .description = "NVMe storage controller driver",
            .type = DRIVER_TYPE_STORAGE,
            .vendor_id = 0x8086,  // Intel
            .device_id = 0x5845,  // NVMe controller
            .class_code = 0x01,   // Mass storage
            .subclass_code = 0x08, // NVMe
            .prog_if = 0x02,
            .version = 0x0100,
            .supports_hotplug = false,
            .requires_interrupts = true
        },
        .ops = {
            .init = nvme_driver_adapter_init,
            .deinit = nvme_driver_adapter_deinit,
            .probe = nvme_driver_adapter_probe,
            .enable = nvme_driver_adapter_enable,
            .disable = nvme_driver_adapter_disable,
            .reset = nvme_driver_adapter_reset,
            .suspend = NULL,
            .resume = NULL,
            .status = nvme_driver_adapter_status
        },
        .private_data_size = 0
    };
    
    // AHCI driver registration
    driver_registration_t ahci_reg = {
        .info = {
            .name = "AHCI Driver",
            .description = "AHCI SATA controller driver",
            .type = DRIVER_TYPE_STORAGE,
            .vendor_id = 0x8086,  // Intel
            .device_id = 0x2922,  // ICH9 AHCI
            .class_code = 0x01,   // Mass storage
            .subclass_code = 0x06, // SATA
            .prog_if = 0x01,
            .version = 0x0100,
            .supports_hotplug = true,
            .requires_interrupts = true
        },
        .ops = {
            .init = ahci_driver_adapter_init,
            .deinit = ahci_driver_adapter_deinit,
            .probe = ahci_driver_adapter_probe,
            .enable = ahci_driver_adapter_enable,
            .disable = ahci_driver_adapter_disable,
            .reset = ahci_driver_adapter_reset,
            .suspend = NULL,
            .resume = NULL,
            .status = ahci_driver_adapter_status
        },
        .private_data_size = 0
    };
    
    // IDE driver registration
    driver_registration_t ide_reg = {
        .info = {
            .name = "IDE Driver",
            .description = "IDE controller driver",
            .type = DRIVER_TYPE_STORAGE,
            .vendor_id = 0x8086,  // Intel
            .device_id = 0x7010,  // PIIX4 IDE
            .class_code = 0x01,   // Mass storage
            .subclass_code = 0x01, // IDE
            .prog_if = 0x8A,
            .version = 0x0100,
            .supports_hotplug = false,
            .requires_interrupts = true
        },
        .ops = {
            .init = ide_driver_adapter_init,
            .deinit = ide_driver_adapter_deinit,
            .probe = ide_driver_adapter_probe,
            .enable = ide_driver_adapter_enable,
            .disable = ide_driver_adapter_disable,
            .reset = ide_driver_adapter_reset,
            .suspend = NULL,
            .resume = NULL,
            .status = ide_driver_adapter_status
        },
        .private_data_size = 0
    };
    
    // E1000 driver registration
    driver_registration_t e1000_reg = {
        .info = {
            .name = "E1000 Driver",
            .description = "Intel E1000 network adapter driver",
            .type = DRIVER_TYPE_NETWORK,
            .vendor_id = 0x8086,  // Intel
            .device_id = 0x100E,  // 82540EM
            .class_code = 0x02,   // Network controller
            .subclass_code = 0x00, // Ethernet
            .prog_if = 0x00,
            .version = 0x0100,
            .supports_hotplug = false,
            .requires_interrupts = true
        },
        .ops = {
            .init = e1000_driver_adapter_init,
            .deinit = e1000_driver_adapter_deinit,
            .probe = e1000_driver_adapter_probe,
            .enable = e1000_driver_adapter_enable,
            .disable = e1000_driver_adapter_disable,
            .reset = e1000_driver_adapter_reset,
            .suspend = NULL,
            .resume = NULL,
            .status = e1000_driver_adapter_status
        },
        .private_data_size = 0
    };
    
    // RTL8139 driver registration
    driver_registration_t rtl8139_reg = {
        .info = {
            .name = "RTL8139 Driver",
            .description = "Realtek RTL8139 network adapter driver",
            .type = DRIVER_TYPE_NETWORK,
            .vendor_id = 0x10EC,  // Realtek
            .device_id = 0x8139,  // RTL8139
            .class_code = 0x02,   // Network controller
            .subclass_code = 0x00, // Ethernet
            .prog_if = 0x00,
            .version = 0x0100,
            .supports_hotplug = false,
            .requires_interrupts = true
        },
        .ops = {
            .init = rtl8139_driver_adapter_init,
            .deinit = rtl8139_driver_adapter_deinit,
            .probe = rtl8139_driver_adapter_probe,
            .enable = rtl8139_driver_adapter_enable,
            .disable = rtl8139_driver_adapter_disable,
            .reset = rtl8139_driver_adapter_reset,
            .suspend = NULL,
            .resume = NULL,
            .status = rtl8139_driver_adapter_status
        },
        .private_data_size = 0
    };
    
    // RTL810xE driver registration
    extern int rtl810x_driver_adapter_init(struct pci_device *dev);
    extern int rtl810x_driver_adapter_deinit(void);
    extern int rtl810x_driver_adapter_probe(struct pci_device *dev);
    extern int rtl810x_driver_adapter_enable(void);
    extern int rtl810x_driver_adapter_disable(void);
    extern int rtl810x_driver_adapter_reset(void);
    extern void rtl810x_driver_adapter_status(void);
    driver_registration_t rtl810x_reg = {
        .info = {
            .name = "RTL810xE Driver",
            .description = "Realtek RTL810xE PCIe Fast Ethernet driver",
            .type = DRIVER_TYPE_NETWORK,
            .vendor_id = 0x10EC,
            .device_id = 0x8136,
            .class_code = 0x02,
            .subclass_code = 0x00,
            .prog_if = 0x00,
            .version = 0x0100,
            .supports_hotplug = false,
            .requires_interrupts = true
        },
        .ops = {
            .init = rtl810x_driver_adapter_init,
            .deinit = rtl810x_driver_adapter_deinit,
            .probe = rtl810x_driver_adapter_probe,
            .enable = rtl810x_driver_adapter_enable,
            .disable = rtl810x_driver_adapter_disable,
            .reset = rtl810x_driver_adapter_reset,
            .suspend = NULL,
            .resume = NULL,
            .status = rtl810x_driver_adapter_status
        },
        .private_data_size = 0
    };
    
    // AMD GPU driver registration
    driver_registration_t amd_gpu_reg = {
        .info = {
            .name = "AMD GPU Driver",
            .description = "AMD GPU driver",
            .type = DRIVER_TYPE_GPU,
            .vendor_id = 0x1002,  // AMD
            .device_id = 0x0000,  // Any AMD device
            .class_code = 0x03,   // Display controller
            .subclass_code = 0x00, // VGA
            .prog_if = 0x00,
            .version = 0x0100,
            .supports_hotplug = false,
            .requires_interrupts = true
        },
        .ops = {
            .init = amd_gpu_driver_adapter_init,
            .deinit = amd_gpu_driver_adapter_deinit,
            .probe = amd_gpu_driver_adapter_probe,
            .enable = amd_gpu_driver_adapter_enable,
            .disable = amd_gpu_driver_adapter_disable,
            .reset = amd_gpu_driver_adapter_reset,
            .suspend = NULL,
            .resume = NULL,
            .status = amd_gpu_driver_adapter_status
        },
        .private_data_size = 0
    };
    
    // VMware SVGA driver registration
    driver_registration_t vmware_svga_reg = {
        .info = {
            .name = "VMware SVGA-II Driver",
            .description = "VMware SVGA-II virtual graphics adapter driver",
            .type = DRIVER_TYPE_GPU,
            .vendor_id = VMWARE_VENDOR_ID,  // VMware
            .device_id = VMWARE_SVGA_DEVICE_ID,  // SVGA-II
            .class_code = VMWARE_SVGA_CLASS,   // Display controller
            .subclass_code = VMWARE_SVGA_SUBCLASS, // VGA
            .prog_if = 0x00,
            .version = 0x0100,
            .supports_hotplug = false,
            .requires_interrupts = false
        },
        .ops = {
            .init = vmware_svga_driver_adapter_init,
            .deinit = vmware_svga_driver_adapter_deinit,
            .probe = vmware_svga_driver_adapter_probe,
            .enable = vmware_svga_driver_adapter_enable,
            .disable = vmware_svga_driver_adapter_disable,
            .reset = vmware_svga_driver_adapter_reset,
            .suspend = NULL,
            .resume = NULL,
            .status = vmware_svga_driver_adapter_status
        },
        .private_data_size = 0
    };
    
    // USB driver registration
    driver_registration_t usb_reg = {
        .info = {
            .name = "USB Controller Driver",
            .description = "USB host controller driver",
            .type = DRIVER_TYPE_USB,
            .vendor_id = 0x8086,  // Intel
            .device_id = 0x0000,  // Any USB controller
            .class_code = 0x0C,   // Serial bus controller
            .subclass_code = 0x03, // USB
            .prog_if = 0x00,
            .version = 0x0100,
            .supports_hotplug = true,
            .requires_interrupts = true
        },
        .ops = {
            .init = usb_driver_adapter_init,
            .deinit = usb_driver_adapter_deinit,
            .probe = usb_driver_adapter_probe,
            .enable = usb_driver_adapter_enable,
            .disable = usb_driver_adapter_disable,
            .reset = usb_driver_adapter_reset,
            .suspend = NULL,
            .resume = NULL,
            .status = usb_driver_adapter_status
        },
        .private_data_size = 0
    };

    // PCnet driver registration
    driver_registration_t pcnet_reg = {
        .info = {
            .name = "AMD PCnet Driver",
            .description = "AMD PCnet-PCI II network adapter driver",
            .type = DRIVER_TYPE_NETWORK,
            .vendor_id = PCNET_VENDOR_ID,
            .device_id = PCNET_DEVICE_ID,
            .class_code = 0x02,
            .subclass_code = 0x00,
            .prog_if = 0x00,
            .version = 0x0100,
            .supports_hotplug = false,
            .requires_interrupts = true
        },
        .ops = {
            .init = pcnet_driver_adapter_init,
            .deinit = pcnet_driver_adapter_deinit,
            .probe = pcnet_driver_adapter_probe,
            .enable = pcnet_driver_adapter_enable,
            .disable = pcnet_driver_adapter_disable,
            .reset = pcnet_driver_adapter_reset,
            .suspend = NULL,
            .resume = NULL,
            .status = pcnet_driver_adapter_status
        },
        .private_data_size = 0
    };
    
    // Register all drivers
    int registered_count = 0;
    
    if (driver_register(&nvme_reg) >= 0) registered_count++;
    if (driver_register(&ahci_reg) >= 0) registered_count++;
    if (driver_register(&ide_reg) >= 0) registered_count++;
    if (driver_register(&e1000_reg) >= 0) registered_count++;
    if (driver_register(&rtl8139_reg) >= 0) registered_count++;
    if (driver_register(&rtl810x_reg) >= 0) registered_count++;
    if (driver_register(&amd_gpu_reg) >= 0) registered_count++;
    if (driver_register(&vmware_svga_reg) >= 0) registered_count++;
    if (driver_register(&usb_reg) >= 0) registered_count++;
    if (driver_register(&pcnet_reg) >= 0) registered_count++;
    
    printf("Driver adapters: Registered %d drivers\n", registered_count);
    DEBUG_SERIAL("Driver adapters: Registration complete, %d drivers registered\n", registered_count);
    
    return registered_count;
} 