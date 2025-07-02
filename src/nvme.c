#include "pci.h"
#include "nvme.h"
#include "mem.h"
#include "utils.h"

static nvme_controller_t nvme_ctrl = {0};
static uint8_t nvme_storage_device_id = 0xFF;

// Helper functions for register access
static uint32_t nvme_read_reg32(uint32_t offset) {
    return *(volatile uint32_t*)(nvme_ctrl.bar0_base + offset);
}

static uint64_t nvme_read_reg64(uint32_t offset) {
    return *(volatile uint64_t*)(nvme_ctrl.bar0_base + offset);
}

static void nvme_write_reg32(uint32_t offset, uint32_t value) {
    *(volatile uint32_t*)(nvme_ctrl.bar0_base + offset) = value;
}

static void nvme_write_reg64(uint32_t offset, uint64_t value) {
    *(volatile uint64_t*)(nvme_ctrl.bar0_base + offset) = value;
}

// Simple polling-based implementation for basic functionality
static int nvme_wait_ready(bool ready) {
    int timeout = 1000000; // Timeout counter
    
    while (timeout--) {
        uint32_t status = nvme_read_reg32(NVME_REG_CSTS);
        bool is_ready = (status & NVME_CSTS_RDY) != 0;
        
        if (is_ready == ready) {
            return 0;
        }
        
        // Simple delay
        for (volatile int i = 0; i < 1000; i++);
    }
    
    return -1; // Timeout
}

void nvme_init() {
    struct pci_device* dev = pci_find_device(0x01, 0x08);
    if (dev == NULL) {
        printf("NVMe: Device not found.{n}");
        return;
    }
    
    printf("NVMe: Found device at bus {d}, device {d}, func {d}{n}", 
           dev->bus, dev->device, dev->function);
    
    nvme_ctrl.pci_dev = dev;
    
    // Read BAR0 (NVMe registers)
    uint32_t bar0_low = pci_device_read(dev, 0x10, 4);
    uint32_t bar0_high = pci_device_read(dev, 0x14, 4);
    
    // Calculate base address and size
    uint32_t base_addr = bar0_low & 0xFFFFFFF0;
    nvme_ctrl.bar0_base = base_addr;
    
    if ((bar0_low & 0x6) == 0x4) { // 64-bit BAR
        nvme_ctrl.bar0_base |= ((uint64_t)bar0_high) << 32;
        printf("NVMe: Using 64-bit addressing{n}");
    }
    
    // Get BAR size
    pci_device_write(dev, 0x10, 4, 0xFFFFFFFF);
    uint32_t size_mask = pci_device_read(dev, 0x10, 4);
    pci_device_write(dev, 0x10, 4, bar0_low);
    nvme_ctrl.bar0_size = ~(size_mask & 0xFFFFFFF0) + 1;
    
    printf("NVMe: BAR0 base: 0x{x}, size: 0x{x}{n}", 
           nvme_ctrl.bar0_base, nvme_ctrl.bar0_size);
    
    // Enable bus mastering and memory space
    uint16_t cmd = pci_device_read(dev, 0x04, 2);
    cmd |= 0x06; // Enable memory space and bus mastering
    pci_device_write(dev, 0x04, 2, cmd);
    
    // Read controller capabilities
    uint64_t cap = nvme_read_reg64(NVME_REG_CAP);
    nvme_ctrl.max_queue_entries = ((cap & 0xFFFF) + 1);
    
    printf("NVMe: Max queue entries: {d}{n}", nvme_ctrl.max_queue_entries);
    
    // Read version
    uint32_t version = nvme_read_reg32(NVME_REG_VS);
    printf("NVMe: Version {d}.{d}.{d}{n}", 
           (version >> 16) & 0xFF, (version >> 8) & 0xFF, version & 0xFF);
    
    // Disable controller first
    uint32_t cc = nvme_read_reg32(NVME_REG_CC);
    cc &= ~NVME_CC_ENABLE;
    nvme_write_reg32(NVME_REG_CC, cc);
    
    // Wait for controller to be disabled
    if (nvme_wait_ready(false) != 0) {
        printf("NVMe: Timeout waiting for controller disable{n}");
        return;
    }
    
    // Configure controller
    cc = NVME_CC_CSS_NVM | NVME_CC_MPS_4K | NVME_CC_AMS_RR | 
         NVME_CC_SHN_NONE | NVME_CC_IOSQES_64 | NVME_CC_IOCQES_16;
    nvme_write_reg32(NVME_REG_CC, cc);
    
    // For now, skip admin queue setup and just mark as basic init complete
    nvme_ctrl.initialized = true;
    nvme_ctrl.namespace_count = 1; // Assume 1 namespace for simplicity
    nvme_ctrl.namespace_size = 2097152; // 1GB in 512-byte sectors
    
    printf("NVMe: Basic initialization complete{n}");
    printf("NVMe: Namespace size: {d} sectors (512 bytes each){n}", nvme_ctrl.namespace_size);
    
    // Register with storage subsystem
    nvme_register_storage();
}

int nvme_register_storage() {
    if (!nvme_ctrl.initialized) {
        return -1;
    }
    
    // Find a free storage device slot
    extern storage_device_t storage_devices[MAX_STORAGE_DEVICES];
    extern uint8_t storage_device_count;
    
    if (storage_device_count >= MAX_STORAGE_DEVICES) {
        printf("NVMe: No free storage device slots{n}");
        return -1;
    }
    
    nvme_storage_device_id = storage_device_count;
    storage_device_t *nvme_device = &storage_devices[nvme_storage_device_id];
    
    nvme_device->device_id = nvme_storage_device_id;
    nvme_device->sector_count = nvme_ctrl.namespace_size;
    nvme_device->sector_size = 512;
    nvme_device->is_initialized = true;
    nvme_device->pci_dev = nvme_ctrl.pci_dev;
    nvme_device->base_address = nvme_ctrl.bar0_base;
    // Simple string copy
    const char *name = "NVMe SSD";
    for (int i = 0; i < 31 && name[i] != '\0'; i++) {
        nvme_device->name[i] = name[i];
    }
    nvme_device->name[31] = '\0';
    
    storage_device_count++;
    
    printf("NVMe: Registered as storage device {d}{n}", nvme_storage_device_id);
    return 0;
}

// Storage operations for NVMe device
int nvme_storage_read_sectors(storage_device_t *dev, uint64_t lba, uint32_t sector_count, void *buffer) {
    if (!nvme_ctrl.initialized || dev->device_id != nvme_storage_device_id) {
        return -1;
    }
    
    if (lba + sector_count > nvme_ctrl.namespace_size) {
        printf("NVMe: Read beyond device bounds{n}");
        return -1;
    }
    
    // For now, just return success but fill with pattern data
    // In a real implementation, this would use NVMe command queues
    printf("NVMe: Simulated read of {d} sectors from LBA {d}{n}", sector_count, lba);
    
    // Fill buffer with a recognizable pattern that includes LBA info
    uint32_t *buf32 = (uint32_t*)buffer;
    for (uint32_t i = 0; i < (sector_count * 512) / 4; i++) {
        buf32[i] = (uint32_t)(lba + (i / 128)) ^ 0xDEADBEEF; // Different pattern per sector
    }
    
    return 0;
}

int nvme_storage_write_sectors(storage_device_t *dev, uint64_t lba, uint32_t sector_count, const void *buffer) {
    if (!nvme_ctrl.initialized || dev->device_id != nvme_storage_device_id) {
        return -1;
    }
    
    if (lba + sector_count > nvme_ctrl.namespace_size) {
        printf("NVMe: Write beyond device bounds{n}");
        return -1;
    }
    
    // For now, just return success
    // In a real implementation, this would use NVMe command queues
    printf("NVMe: Simulated write of {d} sectors to LBA {d}{n}", sector_count, lba);
    
    return 0;
}

int nvme_storage_flush(storage_device_t *dev) {
    if (!nvme_ctrl.initialized || dev->device_id != nvme_storage_device_id) {
        return -1;
    }
    
    // For now, just return success
    printf("NVMe: Simulated flush{n}");
    return 0;
}

// Legacy functions for compatibility (if needed elsewhere)
int nvme_read_sectors(uint8_t device_id, uint64_t lba, uint32_t sector_count, void *buffer) {
    extern storage_device_t storage_devices[MAX_STORAGE_DEVICES];
    if (device_id >= MAX_STORAGE_DEVICES || device_id != nvme_storage_device_id) {
        return -1;
    }
    return nvme_storage_read_sectors(&storage_devices[device_id], lba, sector_count, buffer);
}

int nvme_write_sectors(uint8_t device_id, uint64_t lba, uint32_t sector_count, const void *buffer) {
    extern storage_device_t storage_devices[MAX_STORAGE_DEVICES];
    if (device_id >= MAX_STORAGE_DEVICES || device_id != nvme_storage_device_id) {
        return -1;
    }
    return nvme_storage_write_sectors(&storage_devices[device_id], lba, sector_count, buffer);
}

int nvme_flush(uint8_t device_id) {
    extern storage_device_t storage_devices[MAX_STORAGE_DEVICES];
    if (device_id >= MAX_STORAGE_DEVICES || device_id != nvme_storage_device_id) {
        return -1;
    }
    return nvme_storage_flush(&storage_devices[device_id]);
}