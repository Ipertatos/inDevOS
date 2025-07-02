#include "ide.h"
#include "pci.h"
#include "storage.h"
#include "registers.h"
#include "mem.h"
#include "utils.h"

static ide_controller_t ide_ctrl = {0};
static uint8_t ide_storage_device_id = 0xFF;

// Helper functions for I/O operations
static uint8_t ide_read_reg(uint8_t reg) {
    return inb(ide_ctrl.base_port + reg);
}

static void ide_write_reg(uint8_t reg, uint8_t value) {
    outb(ide_ctrl.base_port + reg, value);
}

static void ide_read_data(uint16_t *buffer, uint32_t words) {
    for (uint32_t i = 0; i < words; i++) {
        buffer[i] = inw(ide_ctrl.base_port + IDE_REG_DATA);
    }
}

static void ide_write_data(uint16_t *buffer, uint32_t words) {
    for (uint32_t i = 0; i < words; i++) {
        outw(ide_ctrl.base_port + IDE_REG_DATA, buffer[i]);
    }
}

// Wait for IDE controller to be ready
static int ide_wait_ready() {
    int timeout = 10000;
    
    while (timeout--) {
        uint8_t status = ide_read_reg(IDE_REG_STATUS);
        if (!(status & IDE_STATUS_BSY) && (status & IDE_STATUS_RDY)) {
            return 0;
        }
        // Small delay
        for (volatile int i = 0; i < 100; i++);
    }
    
    return -1; // Timeout
}

// Wait for data ready
static int ide_wait_data() {
    int timeout = 10000;
    
    while (timeout--) {
        uint8_t status = ide_read_reg(IDE_REG_STATUS);
        if (status & IDE_STATUS_DRQ) {
            return 0;
        }
        if (status & IDE_STATUS_ERR) {
            return -1;
        }
        // Small delay
        for (volatile int i = 0; i < 100; i++);
    }
    
    return -1; // Timeout
}

// Identify IDE drive to get capacity
static int ide_identify_drive() {
    if (ide_wait_ready() != 0) {
        printf("IDE: Drive not ready for identify{n}");
        return -1;
    }
    
    // Select drive (master)
    ide_write_reg(IDE_REG_DRIVE, IDE_DRIVE_MASTER);
    
    // Send IDENTIFY command
    ide_write_reg(IDE_REG_COMMAND, IDE_CMD_IDENTIFY);
    
    // Wait for data
    if (ide_wait_data() != 0) {
        printf("IDE: IDENTIFY command failed{n}");
        return -1;
    }
    
    // Read 256 words of identification data
    uint16_t identify_data[256];
    ide_read_data(identify_data, 256);
    
    // Get sector count from words 60-61 (LBA28) or 100-103 (LBA48)
    uint32_t sectors = identify_data[60] | (identify_data[61] << 16);
    
    if (sectors == 0) {
        printf("IDE: Drive reports 0 sectors{n}");
        return -1;
    }
    
    ide_ctrl.sectors = sectors;
    uint32_t size_mb = (sectors * 512) / (1024 * 1024);
    
    printf("IDE: Drive capacity: {d} sectors ({d} MB){n}", sectors, size_mb);
    return 0;
}

void ide_init() {
    printf("IDE: Starting IDE initialization{n}");
    struct pci_device* dev = pci_find_device(0x01, 0x01);
    if (dev == NULL) {
        printf("IDE: No IDE controller found (class 01:01){n}");
        return;
    }
    
    printf("IDE: Found IDE controller at bus {d}, device {d}, func {d}{n}", 
           dev->bus, dev->device, dev->function);
    
    ide_ctrl.pci_dev = dev;
    
    // Read BAR0 for primary IDE base address
    uint32_t bar0 = pci_device_read(dev, 0x10, 4);
    uint32_t bar1 = pci_device_read(dev, 0x14, 4);
    
    // IDE controllers often use legacy I/O addresses if BARs are 0 or 1
    if ((bar0 & 0xFFFFFFFC) == 0 || (bar0 & 0xFFFFFFFC) == 1) {
        ide_ctrl.base_port = 0x1F0;  // Primary IDE legacy base
        ide_ctrl.ctrl_port = 0x3F6;  // Primary IDE control
        printf("IDE: Using legacy I/O addresses (0x1F0){n}");
    } else {
        ide_ctrl.base_port = bar0 & 0xFFFFFFFC;
        ide_ctrl.ctrl_port = (bar1 & 0xFFFFFFFC) + 2;
        printf("IDE: Using PCI BAR addresses (0x{x}){n}", ide_ctrl.base_port);
    }
    
    // Enable bus mastering and I/O space
    uint16_t cmd = pci_device_read(dev, 0x04, 2);
    cmd |= 0x05; // Enable I/O space and bus mastering
    pci_device_write(dev, 0x04, 2, cmd);
    
    // Initialize drive
    ide_ctrl.drive = 0; // Master drive
    
    if (ide_identify_drive() != 0) {
        printf("IDE: Failed to identify drive{n}");
        return;
    }
    
    ide_ctrl.initialized = true;
    printf("IDE: Controller initialized successfully{n}");
    
    // Register with storage subsystem
    ide_register_storage();
}

int ide_register_storage() {
    if (!ide_ctrl.initialized) {
        return -1;
    }
    
    // Find a free storage device slot
    extern storage_device_t storage_devices[MAX_STORAGE_DEVICES];
    extern uint8_t storage_device_count;
    
    if (storage_device_count >= MAX_STORAGE_DEVICES) {
        printf("IDE: No free storage device slots{n}");
        return -1;
    }
    
    ide_storage_device_id = storage_device_count;
    storage_device_t *ide_device = &storage_devices[ide_storage_device_id];
    
    ide_device->device_id = ide_storage_device_id;
    ide_device->sector_count = ide_ctrl.sectors;
    ide_device->sector_size = 512;
    ide_device->is_initialized = true;
    ide_device->pci_dev = ide_ctrl.pci_dev;
    ide_device->base_address = ide_ctrl.base_port;
    
    // Simple string copy
    const char *name = "IDE Drive";
    for (int i = 0; i < 31 && name[i] != '\0'; i++) {
        ide_device->name[i] = name[i];
    }
    ide_device->name[31] = '\0';
    
    storage_device_count++;
    
    printf("IDE: Registered as storage device {d}{n}", ide_storage_device_id);
    return 0;
}

int ide_read_sectors(storage_device_t *dev, uint64_t lba, uint32_t sector_count, void *buffer) {
    if (!ide_ctrl.initialized || dev->device_id != ide_storage_device_id) {
        return -1;
    }
    
    if (lba + sector_count > ide_ctrl.sectors) {
        printf("IDE: Read beyond drive capacity{n}");
        return -1;
    }
    
    uint16_t *buf = (uint16_t*)buffer;
    
    for (uint32_t i = 0; i < sector_count; i++) {
        uint32_t current_lba = lba + i;
        
        if (ide_wait_ready() != 0) {
            printf("IDE: Drive not ready for read{n}");
            return -1;
        }
        
        // Select drive and set LBA mode
        ide_write_reg(IDE_REG_DRIVE, IDE_DRIVE_MASTER | 0x40 | ((current_lba >> 24) & 0x0F));
        ide_write_reg(IDE_REG_SECCOUNT, 1);
        ide_write_reg(IDE_REG_LBA_LOW, current_lba & 0xFF);
        ide_write_reg(IDE_REG_LBA_MID, (current_lba >> 8) & 0xFF);
        ide_write_reg(IDE_REG_LBA_HIGH, (current_lba >> 16) & 0xFF);
        
        // Send read command
        ide_write_reg(IDE_REG_COMMAND, IDE_CMD_READ_PIO);
        
        // Wait for data
        if (ide_wait_data() != 0) {
            printf("IDE: Read sector {d} failed{n}", current_lba);
            return -1;
        }
        
        // Read 256 words (512 bytes)
        ide_read_data(buf + (i * 256), 256);
    }
    
    return 0;
}

int ide_write_sectors(storage_device_t *dev, uint64_t lba, uint32_t sector_count, const void *buffer) {
    if (!ide_ctrl.initialized || dev->device_id != ide_storage_device_id) {
        return -1;
    }
    
    if (lba + sector_count > ide_ctrl.sectors) {
        printf("IDE: Write beyond drive capacity{n}");
        return -1;
    }
    
    uint16_t *buf = (uint16_t*)buffer;
    
    for (uint32_t i = 0; i < sector_count; i++) {
        uint32_t current_lba = lba + i;
        
        if (ide_wait_ready() != 0) {
            printf("IDE: Drive not ready for write{n}");
            return -1;
        }
        
        // Select drive and set LBA mode
        ide_write_reg(IDE_REG_DRIVE, IDE_DRIVE_MASTER | 0x40 | ((current_lba >> 24) & 0x0F));
        ide_write_reg(IDE_REG_SECCOUNT, 1);
        ide_write_reg(IDE_REG_LBA_LOW, current_lba & 0xFF);
        ide_write_reg(IDE_REG_LBA_MID, (current_lba >> 8) & 0xFF);
        ide_write_reg(IDE_REG_LBA_HIGH, (current_lba >> 16) & 0xFF);
        
        // Send write command
        ide_write_reg(IDE_REG_COMMAND, IDE_CMD_WRITE_PIO);
        
        // Wait for drive ready
        if (ide_wait_data() != 0) {
            printf("IDE: Write sector {d} failed{n}", current_lba);
            return -1;
        }
        
        // Write 256 words (512 bytes)
        ide_write_data(buf + (i * 256), 256);
    }
    
    return 0;
}

int ide_flush(storage_device_t *dev) {
    // IDE doesn't need explicit flush for PIO mode
    return 0;
} 