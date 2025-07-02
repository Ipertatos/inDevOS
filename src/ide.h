#pragma once

#include "types.h"
#include "storage.h"
#include "pci.h"

// IDE Register offsets
#define IDE_REG_DATA        0x00
#define IDE_REG_ERROR       0x01
#define IDE_REG_FEATURES    0x01
#define IDE_REG_SECCOUNT    0x02
#define IDE_REG_LBA_LOW     0x03
#define IDE_REG_LBA_MID     0x04
#define IDE_REG_LBA_HIGH    0x05
#define IDE_REG_DRIVE       0x06
#define IDE_REG_STATUS      0x07
#define IDE_REG_COMMAND     0x07

// IDE Status bits
#define IDE_STATUS_ERR      0x01
#define IDE_STATUS_DRQ      0x08
#define IDE_STATUS_SRV      0x10
#define IDE_STATUS_DF       0x20
#define IDE_STATUS_RDY      0x40
#define IDE_STATUS_BSY      0x80

// IDE Commands
#define IDE_CMD_READ_PIO    0x20
#define IDE_CMD_WRITE_PIO   0x30
#define IDE_CMD_IDENTIFY    0xEC

// IDE Drive selection
#define IDE_DRIVE_MASTER    0xA0
#define IDE_DRIVE_SLAVE     0xB0

typedef struct {
    uint16_t base_port;      // Primary I/O base (e.g., 0x1F0)
    uint16_t ctrl_port;      // Control port (base + 0x206, e.g., 0x1F6)
    struct pci_device *pci_dev;
    bool initialized;
    uint8_t drive;           // 0 = master, 1 = slave
    uint32_t sectors;        // Number of sectors
} ide_controller_t;

// Function declarations
void ide_init();
int ide_register_storage();
int ide_read_sectors(storage_device_t *dev, uint64_t lba, uint32_t sector_count, void *buffer);
int ide_write_sectors(storage_device_t *dev, uint64_t lba, uint32_t sector_count, const void *buffer);
int ide_flush(storage_device_t *dev); 