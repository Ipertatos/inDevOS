#include "driver_manager.h"
#include "driver_adapters.h"
#include "kmod.h"
#include "mem.h"
#include "string.h"
#include "serial.h"
#include "cmd.h"
#include "fat32.h"
#include "storage.h"
#include "utils.h"
#include "atomic.h"
#include "ktime.h"
#include <stddef.h>

/* prog_if in registration tables: 0 means "ignore prog_if when matching" */
static int pci_prog_if_matches(uint8_t reg_prog_if, uint8_t dev_prog_if)
{
    if (reg_prog_if == 0)
        return 1;
    return reg_prog_if == dev_prog_if;
}

static const driver_registration_t *find_storage_registration(struct pci_device *dev)
{
    for (int i = 0; storage_drivers[i].info.name[0]; i++) {
        const driver_registration_t *r = &storage_drivers[i];

        if (r->info.vendor_id == dev->vendor_id &&
            r->info.device_id == dev->device_id &&
            r->info.class_code == dev->class_code &&
            r->info.subclass_code == dev->subclass_code &&
            pci_prog_if_matches(r->info.prog_if, dev->prog_if)) {
            return r;
        }
        /* Any NVMe (class 01 / subclass 08) */
        if (dev->class_code == 0x01 && dev->subclass_code == 0x08 &&
            r->info.class_code == 0x01 && r->info.subclass_code == 0x08) {
            return r;
        }
        /* AHCI — SATA mode, subclass 06, typically prog_if 01 */
        if (dev->class_code == 0x01 && dev->subclass_code == 0x06 &&
            dev->prog_if == 0x01 && r->info.subclass_code == 0x06) {
            return r;
        }
        /* IDE legacy (subclass 01) */
        if (dev->class_code == 0x01 && dev->subclass_code == 0x01 &&
            r->info.subclass_code == 0x01) {
            return r;
        }
    }
    return NULL;
}

static const driver_registration_t *find_network_registration(struct pci_device *dev)
{
    for (int i = 0; network_drivers[i].info.name[0]; i++) {
        const driver_registration_t *r = &network_drivers[i];

        if (r->info.vendor_id == dev->vendor_id &&
            r->info.device_id == dev->device_id &&
            r->info.class_code == dev->class_code &&
            r->info.subclass_code == dev->subclass_code &&
            pci_prog_if_matches(r->info.prog_if, dev->prog_if)) {
            return r;
        }
    }
    return NULL;
}

static const driver_registration_t *find_gpu_registration(struct pci_device *dev)
{
    for (int i = 0; gpu_drivers[i].info.name[0]; i++) {
        const driver_registration_t *r = &gpu_drivers[i];

        if (r->info.vendor_id == dev->vendor_id &&
            r->info.class_code == dev->class_code &&
            r->info.subclass_code == dev->subclass_code) {
            if (r->info.device_id == 0 || r->info.device_id == dev->device_id)
                return r;
        }
    }
    return NULL;
}

static const driver_registration_t *find_usb_registration(struct pci_device *dev)
{
    if (dev->class_code != 0x0C || dev->subclass_code != 0x03)
        return NULL;

    if (usb_drivers[0].info.name[0])
        return &usb_drivers[0];
    return NULL;
}

// Global driver manager instance
driver_manager_t global_driver_manager = {0};

// Predefined driver registrations
const driver_registration_t storage_drivers[] = {
    {
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
    },
    {
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
    },
    {
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
    },
    { .private_data_size = 0 } // End marker (all-zero entry)
};

const driver_registration_t network_drivers[] = {
    {
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
    },
    {
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
    },
    {
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
    },
    {
        .info = {
            .name = "AMD PCnet Driver",
            .description = "AMD PCnet-PCI II network adapter driver",
            .type = DRIVER_TYPE_NETWORK,
            .vendor_id = 0x1022,  // AMD
            .device_id = 0x2000,  // PCnet-PCI II
            .class_code = 0x02,   // Network controller
            .subclass_code = 0x00, // Ethernet
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
    },
    { .private_data_size = 0 } // End marker (all-zero entry)
};

const driver_registration_t gpu_drivers[] = {
    {
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
    },
    { .private_data_size = 0 } // End marker (all-zero entry)
};

const driver_registration_t usb_drivers[] = {
    {
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
    },
    { .private_data_size = 0 } // End marker (all-zero entry)
};

// Initialize the driver manager
void driver_manager_init(void) {
    DEBUG_SERIAL("Driver manager: Initializing\n");
    
    memset(&global_driver_manager, 0, sizeof(driver_manager_t));
    global_driver_manager.next_driver_id = 1;
    global_driver_manager.initialized = true;
    
    printf("Driver manager initialized\n");
    DEBUG_SERIAL("Driver manager: Initialization complete\n");
}

// Register a new driver
int driver_register(const driver_registration_t *reg) {
    if (!global_driver_manager.initialized) {
        printf("Driver manager not initialized\n");
        return -1;
    }
    
    if (!reg || !reg->info.name[0]) {
        printf("Invalid driver registration\n");
        return -1;
    }
    
    klock(&global_driver_manager.lock);
    
    // Check if driver already exists
    driver_instance_t *existing = driver_find_by_name(reg->info.name);
    if (existing) {
        printf("Driver '%s' already registered\n", reg->info.name);
        kunlock(&global_driver_manager.lock);
        return -1;
    }
    
    // Allocate new driver instance
    driver_instance_t *driver = kalloc(sizeof(driver_instance_t));
    if (!driver) {
        printf("Failed to allocate driver instance\n");
        kunlock(&global_driver_manager.lock);
        return -1;
    }
    
    // Initialize driver instance
    memset(driver, 0, sizeof(driver_instance_t));
    driver->id = global_driver_manager.next_driver_id++;
    driver->info = reg->info;
    driver->ops = reg->ops;
    driver->state = DRIVER_STATE_UNLOADED;
    driver->load_time = 0;
    driver->error_count = 0;
    
    // Allocate private data if needed
    if (reg->private_data_size > 0) {
        driver->private_data = kalloc(reg->private_data_size);
        if (!driver->private_data) {
            printf("Failed to allocate private data for driver '%s'\n", reg->info.name);
            kfree(driver);
            kunlock(&global_driver_manager.lock);
            return -1;
        }
        memset(driver->private_data, 0, reg->private_data_size);
    }
    
    // Add to driver list
    driver->next = global_driver_manager.drivers;
    global_driver_manager.drivers = driver;
    global_driver_manager.driver_count++;
    
    kunlock(&global_driver_manager.lock);
    
    printf("Driver '%s' registered (ID: %d)\n", reg->info.name, driver->id);
    return driver->id;
}

// Unregister a driver
int driver_unregister(uint32_t driver_id) {
    if (!global_driver_manager.initialized) {
        return -1;
    }
    
    klock(&global_driver_manager.lock);
    
    driver_instance_t *prev = NULL;
    driver_instance_t *driver = global_driver_manager.drivers;
    
    while (driver) {
        if (driver->id == driver_id) {
            // Remove from list
            if (prev) {
                prev->next = driver->next;
            } else {
                global_driver_manager.drivers = driver->next;
            }
            
            // Free private data
            if (driver->private_data) {
                kfree(driver->private_data);
            }
            
            // Free driver instance
            kfree(driver);
            global_driver_manager.driver_count--;
            
            kunlock(&global_driver_manager.lock);
            printf("Driver ID %d unregistered\n", driver_id);
            return 0;
        }
        prev = driver;
        driver = driver->next;
    }
    
    kunlock(&global_driver_manager.lock);
    printf("Driver ID %d not found\n", driver_id);
    return -1;
}

// Find driver by ID
driver_instance_t* driver_find_by_id(uint32_t driver_id) {
    driver_instance_t *driver = global_driver_manager.drivers;
    while (driver) {
        if (driver->id == driver_id) {
            return driver;
        }
        driver = driver->next;
    }
    return NULL;
}

// Find driver by name
driver_instance_t* driver_find_by_name(const char *name) {
    if (!name) return NULL;
    
    driver_instance_t *driver = global_driver_manager.drivers;
    while (driver) {
        if (kstrcmp(driver->info.name, name) == 0) {
            return driver;
        }
        driver = driver->next;
    }
    return NULL;
}

// Find driver by PCI device
driver_instance_t* driver_find_by_pci(struct pci_device *dev) {
    if (!dev) return NULL;
    
    driver_instance_t *driver = global_driver_manager.drivers;
    while (driver) {
        if (driver->pci_dev == dev) {
            return driver;
        }
        driver = driver->next;
    }
    return NULL;
}

// Find driver by type
driver_instance_t* driver_find_by_type(driver_type_t type) {
    driver_instance_t *driver = global_driver_manager.drivers;
    while (driver) {
        if (driver->info.type == type) {
            return driver;
        }
        driver = driver->next;
    }
    return NULL;
}

// Enable a driver
int driver_enable(uint32_t driver_id) {
    driver_instance_t *driver = driver_find_by_id(driver_id);
    if (!driver) {
        printf("Driver ID %d not found\n", driver_id);
        return -1;
    }
    
    if (driver->state == DRIVER_STATE_RUNNING) {
        printf("Driver '%s' already running\n", driver->info.name);
        return 0;
    }
    
    if (driver->ops.enable) {
        int result = driver->ops.enable();
        if (result == 0) {
            driver->state = DRIVER_STATE_RUNNING;
            printf("Driver '%s' enabled\n", driver->info.name);
            return 0;
        } else {
            driver->state = DRIVER_STATE_ERROR;
            kstrncpy(driver->last_error, "Enable operation failed", sizeof(driver->last_error));
            driver->error_count++;
            printf("Failed to enable driver '%s'\n", driver->info.name);
            return result;
        }
    }
    
    printf("Driver '%s' has no enable operation\n", driver->info.name);
    return -1;
}

// Disable a driver
int driver_disable(uint32_t driver_id) {
    driver_instance_t *driver = driver_find_by_id(driver_id);
    if (!driver) {
        printf("Driver ID %d not found\n", driver_id);
        return -1;
    }
    
    if (driver->state != DRIVER_STATE_RUNNING) {
        printf("Driver '%s' not running\n", driver->info.name);
        return 0;
    }
    
    if (driver->ops.disable) {
        int result = driver->ops.disable();
        if (result == 0) {
            driver->state = DRIVER_STATE_INITIALIZED;
            printf("Driver '%s' disabled\n", driver->info.name);
            return 0;
        } else {
            printf("Failed to disable driver '%s'\n", driver->info.name);
            return result;
        }
    }
    
    printf("Driver '%s' has no disable operation\n", driver->info.name);
    return -1;
}

// Reset a driver
int driver_reset(uint32_t driver_id) {
    driver_instance_t *driver = driver_find_by_id(driver_id);
    if (!driver) {
        printf("Driver ID %d not found\n", driver_id);
        return -1;
    }
    
    if (driver->ops.reset) {
        int result = driver->ops.reset();
        if (result == 0) {
            driver->state = DRIVER_STATE_INITIALIZED;
            driver->error_count = 0;
            memset(driver->last_error, 0, sizeof(driver->last_error));
            printf("Driver '%s' reset\n", driver->info.name);
            return 0;
        } else {
            printf("Failed to reset driver '%s'\n", driver->info.name);
            return result;
        }
    }
    
    printf("Driver '%s' has no reset operation\n", driver->info.name);
    return -1;
}

// List all drivers
void driver_list_all(void) {
    printf("\nDriver Manager - Registered Drivers:\n");
    printf("=====================================\n");
    
    if (global_driver_manager.driver_count == 0) {
        printf("No drivers registered\n");
        return;
    }
    
    driver_instance_t *driver = global_driver_manager.drivers;
    while (driver) {
        const char *type_str = "Unknown";
        const char *state_str = "Unknown";
        
        switch (driver->info.type) {
            case DRIVER_TYPE_STORAGE: type_str = "Storage"; break;
            case DRIVER_TYPE_NETWORK: type_str = "Network"; break;
            case DRIVER_TYPE_GPU: type_str = "GPU"; break;
            case DRIVER_TYPE_USB: type_str = "USB"; break;
            case DRIVER_TYPE_AUDIO: type_str = "Audio"; break;
            case DRIVER_TYPE_INPUT: type_str = "Input"; break;
            default: type_str = "Unknown"; break;
        }
        
        switch (driver->state) {
            case DRIVER_STATE_UNLOADED: state_str = "Unloaded"; break;
            case DRIVER_STATE_LOADED: state_str = "Loaded"; break;
            case DRIVER_STATE_INITIALIZED: state_str = "Initialized"; break;
            case DRIVER_STATE_RUNNING: state_str = "Running"; break;
            case DRIVER_STATE_ERROR: state_str = "Error"; break;
            case DRIVER_STATE_DISABLED: state_str = "Disabled"; break;
            default: state_str = "Unknown"; break;
        }
        
        printf("ID: %d | %s | %s | %s | %04x:%04x\n",
               driver->id, driver->info.name, type_str, state_str,
               driver->info.vendor_id, driver->info.device_id);
        
        if (driver->error_count > 0) {
            printf("    Errors: %d | Last: %s\n", driver->error_count, driver->last_error);
        }
        
        driver = driver->next;
    }
    printf("\n");
}

// Show driver status
void driver_status(uint32_t driver_id) {
    driver_instance_t *driver = driver_find_by_id(driver_id);
    if (!driver) {
        printf("Driver ID %d not found\n", driver_id);
        return;
    }
    
    printf("\nDriver Status - %s (ID: %d)\n", driver->info.name, driver->id);
    printf("==============================\n");
    printf("Description: %s\n", driver->info.description);
    printf("Type: %d\n", driver->info.type);
    printf("Vendor/Device: %04x:%04x\n", driver->info.vendor_id, driver->info.device_id);
    printf("Class: %02x:%02x:%02x\n", driver->info.class_code, driver->info.subclass_code, driver->info.prog_if);
    printf("Version: %08x\n", driver->info.version);
    printf("State: %d\n", driver->state);
    printf("PCI Device: %p\n", driver->pci_dev);
    printf("Load Time: %llu\n", (unsigned long long)driver->load_time);
    printf("Error Count: %d\n", driver->error_count);
    
    if (driver->error_count > 0) {
        printf("Last Error: %s\n", driver->last_error);
    }
    
    if (driver->ops.status) {
        printf("\nDriver-specific status:\n");
        driver->ops.status();
    }
    printf("\n");
}

// Auto-discover and load matching drivers
int driver_auto_discover(void) {
    printf("Driver manager: Auto-discovering devices...\n");
    
    int loaded_count = 0;
    
    // Scan all PCI devices and try to load matching drivers
    for (int i = 0; i < pci_device_count; i++) {
        struct pci_device *dev = pci_devices[i];
        if (driver_load_matching(dev) == 0) {
            loaded_count++;
        }
    }
    
    printf("Driver manager: Auto-discovery complete, loaded %d drivers\n", loaded_count);
    return loaded_count;
}

// Load driver matching a PCI device
int driver_load_matching(struct pci_device *dev) {
    if (!dev) return -1;

    if (driver_find_by_pci(dev))
        return 0;

    const driver_registration_t *reg = find_storage_registration(dev);
    if (!reg)
        reg = find_network_registration(dev);
    if (!reg)
        reg = find_gpu_registration(dev);
    if (!reg)
        reg = find_usb_registration(dev);

    if (!reg)
        return -1;

    /* RTL810xE driver is still a stub — do not bind via auto-discover */
    if (kstrcmp(reg->info.name, "RTL810xE Driver") == 0) {
        return -1;
    }

    /* Resolve pre-registered template from driver_adapters_register_all() */
    driver_instance_t *driver = driver_find_by_name(reg->info.name);
    if (!driver) {
        int driver_id = driver_register(reg);
        if (driver_id < 0)
            return -1;
        driver = driver_find_by_id((uint32_t)driver_id);
        if (!driver)
            return -1;
    }

    if (driver->pci_dev == dev) {
        if (driver->state == DRIVER_STATE_INITIALIZED ||
            driver->state == DRIVER_STATE_RUNNING)
            return 0;
    } else if (driver->pci_dev != NULL) {
        printf("Driver '%s' already bound to another PCI function; skipping %04x:%04x\n",
               reg->info.name, dev->vendor_id, dev->device_id);
        return -1;
    }

    driver->pci_dev = dev;
    driver->state = DRIVER_STATE_LOADED;
    driver->load_time = ktime_now_ms();

    printf("Driver '%s' associated with PCI %04x:%04x (bus %u dev %u fn %u)\n",
           reg->info.name, dev->vendor_id, dev->device_id,
           (unsigned)dev->bus, (unsigned)dev->device, (unsigned)dev->function);

    if (!driver->ops.init)
        return 0;

    int result = driver->ops.init(dev);
    if (result == 0) {
        driver->state = DRIVER_STATE_INITIALIZED;
        printf("Driver '%s' initialized successfully\n", reg->info.name);
        return 0;
    }

    driver->state = DRIVER_STATE_ERROR;
    kstrncpy(driver->last_error, "Initialization failed", sizeof(driver->last_error));
    printf("Failed to initialize driver '%s'\n", reg->info.name);
    return -1;
}

// Load driver from disk — delegates to the kmod ELF loader
int driver_load_from_disk(const char *filename) {
    if (!filename || !*filename) {
        printf("Driver manager: load-from-disk requires a filename\n");
        return -1;
    }
    return kmod_load(filename);
}

// Save driver to disk (not implemented; use kmod for persistence story)
int driver_save_to_disk(uint32_t driver_id, const char *filename) {
    (void)driver_id;
    (void)filename;
    printf("Driver manager: save-to-disk not implemented.\n");
    return -1;
}

// Load driver from memory (not implemented)
int driver_load_from_memory(const void *data, size_t size) {
    (void)data;
    (void)size;
    printf("Driver manager: load-from-memory not implemented — use kmod.\n");
    return -1;
}

// Unload a driver
int driver_unload(uint32_t driver_id) {
    driver_instance_t *driver = driver_find_by_id(driver_id);
    if (!driver) {
        printf("Driver ID %d not found\n", driver_id);
        return -1;
    }
    
    // Disable driver if running
    if (driver->state == DRIVER_STATE_RUNNING) {
        driver_disable(driver_id);
    }
    
    // Deinitialize if initialized
    if (driver->state == DRIVER_STATE_INITIALIZED && driver->ops.deinit) {
        driver->ops.deinit();
    }
    
    // Unregister the driver
    return driver_unregister(driver_id);
}

// Probe all drivers
int driver_probe_all(void) {
    printf("Driver manager: Probing all drivers...\n");
    
    int probed_count = 0;
    driver_instance_t *driver = global_driver_manager.drivers;
    
    while (driver) {
        if (driver->ops.probe && driver->pci_dev) {
            int result = driver->ops.probe(driver->pci_dev);
            if (result == 0) {
                probed_count++;
                printf("Driver '%s' probed successfully\n", driver->info.name);
            } else {
                printf("Driver '%s' probe failed\n", driver->info.name);
            }
        }
        driver = driver->next;
    }
    
    printf("Driver manager: Probe complete, %d drivers probed\n", probed_count);
    return probed_count;
} 