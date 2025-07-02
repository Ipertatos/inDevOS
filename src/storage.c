#include "storage.h"
#include "utils.h"
#include "mem.h"
#include "serial.h"

storage_device_t storage_devices[MAX_STORAGE_DEVICES];
uint8_t storage_device_count = 0;

// RAM disk simulation
#define RAMDISK_SIZE_MB 16
#define RAMDISK_SECTORS (RAMDISK_SIZE_MB * 1024 * 1024 / SECTOR_SIZE)
static uint8_t *ramdisk_data = NULL;

// Storage operations for RAM disk
static int ramdisk_read_sectors(storage_device_t *dev, uint64_t lba, uint32_t sector_count, void *buffer) {
    if (!ramdisk_data || lba + sector_count > RAMDISK_SECTORS) {
        printf("storage: Read beyond bounds - LBA {d} + count {d} > max {d}{n}", 
               lba, sector_count, RAMDISK_SECTORS);
        return -1;
    }
    
    uint64_t offset = lba * SECTOR_SIZE;
    uint32_t bytes_to_read = sector_count * SECTOR_SIZE;
    
    // Debug: printf("storage: Reading {d} sectors from LBA {d}{n}", sector_count, lba);
    
    memmove(buffer, ramdisk_data + offset, bytes_to_read);
    return 0;
}

static int ramdisk_write_sectors(storage_device_t *dev, uint64_t lba, uint32_t sector_count, const void *buffer) {
    if (!ramdisk_data || lba + sector_count > RAMDISK_SECTORS) {
        return -1;
    }
    
    uint64_t offset = lba * SECTOR_SIZE;
    uint32_t bytes_to_write = sector_count * SECTOR_SIZE;
    
    memmove(ramdisk_data + offset, buffer, bytes_to_write);
    return 0;
}

static int ramdisk_flush(storage_device_t *dev) {
    // Nothing to flush for RAM disk
    return 0;
}

storage_ops_t storage_ops = {
    .read_sectors = ramdisk_read_sectors,
    .write_sectors = ramdisk_write_sectors,
    .flush = ramdisk_flush
};

void storage_init() {
    DEBUG_SERIAL("storage_init: Before init - device_count=%d\n", storage_device_count);
    
    // Only initialize if not already initialized (preserve existing devices)
    if (storage_device_count == 0) {
        memset(storage_devices, 0, sizeof(storage_devices));
        DEBUG_SERIAL("storage_init: Clearing storage devices array\n");
    } else {
        DEBUG_SERIAL("storage_init: Preserving %d existing devices\n", storage_device_count);
    }
    
    printf("storage: Initializing storage subsystem{n}");
    DEBUG_SERIAL("storage_init: After init - device_count=%d\n", storage_device_count);
}

void storage_init_ramdisk() {
    if (storage_device_count >= MAX_STORAGE_DEVICES) {
        printf("storage: Maximum storage devices reached{n}");
        return;
    }
    
    // Allocate RAM disk
    ramdisk_data = kalloc(RAMDISK_SIZE_MB * 1024 * 1024);
    if (!ramdisk_data) {
        printf("storage: Failed to allocate RAM disk{n}");
        return;
    }
    
    // Clear the RAM disk
    memset(ramdisk_data, 0, RAMDISK_SIZE_MB * 1024 * 1024);
    
    // Create simple FAT32 boot sector
    uint8_t boot_sector[SECTOR_SIZE];
    memset(boot_sector, 0, SECTOR_SIZE);
    
    // FAT32 boot sector data
    boot_sector[0] = 0xEB; // Jump instruction
    boot_sector[1] = 0x58;
    boot_sector[2] = 0x90;
    
    // OEM Name
    memmove(boot_sector + 3, "inDevOS ", 8);
    
    // Bytes per sector (512)
    *(uint16_t*)(boot_sector + 11) = 512;
    
    // Sectors per cluster (8 = 4KB clusters)
    boot_sector[13] = 8;
    
    // Reserved sectors
    *(uint16_t*)(boot_sector + 14) = 32;
    
    // Number of FATs
    boot_sector[16] = 2;
    
    // Root entries (0 for FAT32)
    *(uint16_t*)(boot_sector + 17) = 0;
    
    // Total sectors (small, set to 0)
    *(uint16_t*)(boot_sector + 19) = 0;
    
    // Media descriptor
    boot_sector[21] = 0xF8;
    
    // FAT size 16 (0 for FAT32)
    *(uint16_t*)(boot_sector + 22) = 0;
    
    // Sectors per track
    *(uint16_t*)(boot_sector + 24) = 63;
    
    // Number of heads
    *(uint16_t*)(boot_sector + 26) = 255;
    
    // Hidden sectors
    *(uint32_t*)(boot_sector + 28) = 0;
    
    // Total sectors (32-bit)
    *(uint32_t*)(boot_sector + 32) = RAMDISK_SECTORS;
    
    // FAT32 specific fields
    // FAT size (sectors per FAT)
    *(uint32_t*)(boot_sector + 36) = (RAMDISK_SECTORS / 128); // Rough estimate
    
    // Extended flags
    *(uint16_t*)(boot_sector + 40) = 0;
    
    // Filesystem version
    *(uint16_t*)(boot_sector + 42) = 0;
    
    // Root cluster
    *(uint32_t*)(boot_sector + 44) = 2;
    
    // FS Info sector
    *(uint16_t*)(boot_sector + 48) = 1;
    
    // Backup boot sector
    *(uint16_t*)(boot_sector + 50) = 6;
    
    // Drive number
    boot_sector[64] = 0x80;
    
    // Boot signature
    boot_sector[66] = 0x29;
    
    // Volume ID
    *(uint32_t*)(boot_sector + 67) = 0x12345678;
    
    // Volume label
    memmove(boot_sector + 71, "inDevOS    ", 11);
    
    // FS type
    memmove(boot_sector + 82, "FAT32   ", 8);
    
    // Boot signature
    boot_sector[510] = 0x55;
    boot_sector[511] = 0xAA;
    
    // Write boot sector to RAM disk
    memmove(ramdisk_data, boot_sector, SECTOR_SIZE);
    
    // Initialize FAT table (2 copies)
    uint32_t fat_size_sectors = (RAMDISK_SECTORS / 128);
    uint32_t fat_start_sector = 32; // Reserved sectors
    
    // Clear FAT area
    memset(ramdisk_data + (fat_start_sector * SECTOR_SIZE), 0, fat_size_sectors * SECTOR_SIZE * 2);
    
    // Set up FAT table entries
    uint32_t *fat_table = (uint32_t*)(ramdisk_data + (fat_start_sector * SECTOR_SIZE));
    
    // FAT[0] = media descriptor + EOC marker
    fat_table[0] = 0x0FFFFFF8; // Media descriptor (F8) + 0x0FFFFF
    // FAT[1] = clean shutdown + no errors + EOC
    fat_table[1] = 0x0FFFFFFF; // Clean shutdown + no disk errors + EOC
    // FAT[2] = root directory cluster (end of chain)
    fat_table[2] = 0x0FFFFFFF; // Root directory cluster is end-of-chain
    
    // Copy first FAT to second FAT
    memmove(ramdisk_data + ((fat_start_sector + fat_size_sectors) * SECTOR_SIZE),
            fat_table, fat_size_sectors * SECTOR_SIZE);
    
    // Initialize root directory cluster
    uint32_t data_start_sector = fat_start_sector + (2 * fat_size_sectors);
    uint32_t root_cluster_sector = data_start_sector; // Cluster 2 starts here
    uint32_t sectors_per_cluster = 8;
    
    // Clear root directory cluster (cluster 2)
    memset(ramdisk_data + (root_cluster_sector * SECTOR_SIZE), 0, 
           sectors_per_cluster * SECTOR_SIZE);
    
    printf("storage: FAT32 layout - FAT start: {d}, Data start: {d}, Root at: {d}{n}",
           fat_start_sector, data_start_sector, root_cluster_sector);
    
    // Initialize storage device
    DEBUG_SERIAL("storage_init_ramdisk: Creating ramdisk as device %d\n", storage_device_count);
    storage_device_t *dev = &storage_devices[storage_device_count];
    dev->device_id = storage_device_count;
    storage_device_count++;
    dev->sector_count = RAMDISK_SECTORS;
    dev->sector_size = SECTOR_SIZE;
    dev->is_initialized = true;
    dev->pci_dev = NULL;
    dev->base_address = (uint64_t)ramdisk_data;
    memmove(dev->name, "RAM Disk", 9);
    
    printf("storage: RAM disk initialized ({d} MB, {d} sectors){n}", 
           RAMDISK_SIZE_MB, RAMDISK_SECTORS);
}

storage_device_t* storage_get_device(uint8_t device_id) {
    for (uint8_t i = 0; i < storage_device_count; i++) {
        if (storage_devices[i].device_id == device_id && storage_devices[i].is_initialized) {
            return &storage_devices[i];
        }
    }
    return NULL;
}

int storage_read_sectors(uint8_t device_id, uint64_t lba, uint32_t sector_count, void *buffer) {
    storage_device_t *dev = storage_get_device(device_id);
    if (!dev) {
        return -1;
    }
    
    // Dispatch to appropriate driver based on device type
    if (dev->pci_dev == NULL) {
        // RAM disk
        return storage_ops.read_sectors(dev, lba, sector_count, buffer);
    } else if (dev->pci_dev->class_code == 0x01 && dev->pci_dev->subclass_code == 0x01) {
        // IDE device
        extern int ide_read_sectors(storage_device_t *dev, uint64_t lba, uint32_t sector_count, void *buffer);
        return ide_read_sectors(dev, lba, sector_count, buffer);
    } else if (dev->pci_dev->class_code == 0x01 && dev->pci_dev->subclass_code == 0x06) {
        // SATA device
        extern int ahci_read_sectors(storage_device_t *dev, uint64_t lba, uint32_t sector_count, void *buffer);
        return ahci_read_sectors(dev, lba, sector_count, buffer);
    } else if (dev->pci_dev->class_code == 0x01 && dev->pci_dev->subclass_code == 0x08) {
        // NVMe device
        extern int nvme_read_sectors(storage_device_t *dev, uint64_t lba, uint32_t sector_count, void *buffer);
        return nvme_read_sectors(dev, lba, sector_count, buffer);
    }
    
    return -1; // Unsupported device type
}

int storage_write_sectors(uint8_t device_id, uint64_t lba, uint32_t sector_count, const void *buffer) {
    storage_device_t *dev = storage_get_device(device_id);
    if (!dev) {
        return -1;
    }
    
    // Dispatch to appropriate driver based on device type
    if (dev->pci_dev == NULL) {
        // RAM disk
        return storage_ops.write_sectors(dev, lba, sector_count, buffer);
    } else if (dev->pci_dev->class_code == 0x01 && dev->pci_dev->subclass_code == 0x01) {
        // IDE device
        extern int ide_write_sectors(storage_device_t *dev, uint64_t lba, uint32_t sector_count, const void *buffer);
        return ide_write_sectors(dev, lba, sector_count, buffer);
    } else if (dev->pci_dev->class_code == 0x01 && dev->pci_dev->subclass_code == 0x06) {
        // SATA device
        extern int ahci_write_sectors(storage_device_t *dev, uint64_t lba, uint32_t sector_count, const void *buffer);
        return ahci_write_sectors(dev, lba, sector_count, buffer);
    } else if (dev->pci_dev->class_code == 0x01 && dev->pci_dev->subclass_code == 0x08) {
        // NVMe device
        extern int nvme_write_sectors(storage_device_t *dev, uint64_t lba, uint32_t sector_count, const void *buffer);
        return nvme_write_sectors(dev, lba, sector_count, buffer);
    }
    
    return -1; // Unsupported device type
}

int storage_flush(uint8_t device_id) {
    storage_device_t *dev = storage_get_device(device_id);
    if (!dev) {
        return -1;
    }
    
    // Dispatch to appropriate driver based on device type
    if (dev->pci_dev == NULL) {
        // RAM disk
        return storage_ops.flush(dev);
    } else if (dev->pci_dev->class_code == 0x01 && dev->pci_dev->subclass_code == 0x01) {
        // IDE device
        extern int ide_flush(storage_device_t *dev);
        return ide_flush(dev);
    } else if (dev->pci_dev->class_code == 0x01 && dev->pci_dev->subclass_code == 0x06) {
        // SATA device
        extern int ahci_flush(storage_device_t *dev);
        return ahci_flush(dev);
    } else if (dev->pci_dev->class_code == 0x01 && dev->pci_dev->subclass_code == 0x08) {
        // NVMe device
        extern int nvme_flush(storage_device_t *dev);
        return nvme_flush(dev);
    }
    
    return -1; // Unsupported device type
} 