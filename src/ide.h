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
#define IDE_CMD_READ_PIO        0x20
#define IDE_CMD_WRITE_PIO       0x30
#define IDE_CMD_READ_PIO_EXT    0x24
#define IDE_CMD_WRITE_PIO_EXT   0x34
#define IDE_CMD_READ_DMA        0xC8
#define IDE_CMD_WRITE_DMA       0xCA
#define IDE_CMD_READ_DMA_EXT    0x25
#define IDE_CMD_WRITE_DMA_EXT   0x35
#define IDE_CMD_IDENTIFY        0xEC
#define IDE_CMD_SET_FEATURES    0xEF

/* ATA READ SECTOR(S) / READ DMA count is 8 bits; 0 means 256 sectors (128 KiB). */
#define IDE_PIO_MAX_SECTORS 256
/* READ/WRITE DMA EXT count is 16 bits. 1024 sectors = 512 KiB. */
#define IDE_DMA_MAX_SECTORS 1024

// IDE Drive selection
#define IDE_DRIVE_MASTER    0xA0
#define IDE_DRIVE_SLAVE     0xB0

/* Bus-master IDE (SFF-8038i) */
#define IDE_BM_CMD          0x00
#define IDE_BM_STATUS       0x02
#define IDE_BM_PRDT         0x04
#define IDE_BM_CMD_START    0x01
#define IDE_BM_CMD_READ     0x08
#define IDE_BM_STATUS_ACT   0x01
#define IDE_BM_STATUS_ERR   0x02
#define IDE_BM_STATUS_IRQ   0x04

typedef struct {
    uint16_t base_port;      // Primary I/O base (e.g., 0x1F0)
    uint16_t ctrl_port;      // Control port (base + 0x206, e.g., 0x1F6)
    uint16_t bm_base;        // Bus-master I/O base (PCI BAR4), 0 if none
    struct pci_device *pci_dev;
    bool initialized;
    bool dma_ok;             // BMIDE verified against a PIO read
    bool lba48;
    uint8_t drive;           // 0 = master, 1 = slave
    uint32_t sectors;        // Number of sectors
    uint8_t *dma_buf;        // physically contiguous bounce, < 4 GiB
    void *prdt;              // PRD table (8-byte entries)
} ide_controller_t;

// Function declarations
void ide_init();
int ide_register_storage();
int ide_read_sectors(storage_device_t *dev, uint64_t lba, uint32_t sector_count, void *buffer);
int ide_write_sectors(storage_device_t *dev, uint64_t lba, uint32_t sector_count, const void *buffer);
int ide_flush(storage_device_t *dev);
void ide_atapi_probe(struct pci_device *dev);
int ide_atapi_read(storage_device_t *dev, uint64_t lba, uint32_t count, void *buffer);
