#pragma once

#include "types.h"
#include "storage.h"
#include "pci.h"

// AHCI Register offsets
#define AHCI_REG_CAP        0x00    // Host Capabilities
#define AHCI_REG_GHC        0x04    // Global Host Control
#define AHCI_REG_IS         0x08    // Interrupt Status
#define AHCI_REG_PI         0x0C    // Ports Implemented
#define AHCI_REG_VS         0x10    // Version
#define AHCI_REG_PORTS      0x100   // Port registers start

// Global Host Control bits
#define AHCI_GHC_AHCI_EN    (1 << 31)  // AHCI Enable
#define AHCI_GHC_MRSM       (1 << 2)   // MSI Revert to Single Message
#define AHCI_GHC_IE         (1 << 1)   // Interrupt Enable
#define AHCI_GHC_HR         (1 << 0)   // HBA Reset

// Port register offsets (relative to port base)
#define AHCI_PORT_CLB       0x00    // Command List Base Address
#define AHCI_PORT_CLBU      0x04    // Command List Base Address Upper
#define AHCI_PORT_FB        0x08    // FIS Base Address
#define AHCI_PORT_FBU       0x0C    // FIS Base Address Upper
#define AHCI_PORT_IS        0x10    // Interrupt Status
#define AHCI_PORT_IE        0x14    // Interrupt Enable
#define AHCI_PORT_CMD       0x18    // Command and Status
#define AHCI_PORT_TFD       0x20    // Task File Data
#define AHCI_PORT_SIG       0x24    // Signature
#define AHCI_PORT_SSTS      0x28    // Serial ATA Status
#define AHCI_PORT_SCTL      0x2C    // Serial ATA Control
#define AHCI_PORT_SERR      0x30    // Serial ATA Error
#define AHCI_PORT_SACT      0x34    // Serial ATA Active
#define AHCI_PORT_CI        0x38    // Command Issue

// Port Interrupt Status bits
#define AHCI_PORT_IS_TFES   (1 << 30)  // Task File Error Status
// Port Command and Status bits
#define AHCI_PORT_CMD_ST    (1 << 0)   // Start
#define AHCI_PORT_CMD_SUD   (1 << 1)   // Spin-up Device
#define AHCI_PORT_CMD_POD   (1 << 2)   // Power On Device
#define AHCI_PORT_CMD_CLO   (1 << 3)   // Command List Override
#define AHCI_PORT_CMD_FRE   (1 << 4)   // FIS Receive Enable
#define AHCI_PORT_CMD_FR    (1 << 14)  // FIS Receive Running
#define AHCI_PORT_CMD_CR    (1 << 15)  // Command List Running

// SATA Status bits
#define AHCI_PORT_SSTS_DET_MASK     0x0F
#define AHCI_PORT_SSTS_DET_PRESENT  0x03

// ATA Commands
#define ATA_CMD_READ_DMA_EXT    0x25
#define ATA_CMD_WRITE_DMA_EXT   0x35
#define ATA_CMD_IDENTIFY        0xEC
#define ATA_CMD_PACKET          0xA0
#define ATA_CMD_IDENTIFY_PACKET 0xA1
#define ATA_CMD_FLUSH_CACHE_EXT 0xEA

// FIS Types
#define FIS_TYPE_REG_H2D    0x27    // Register FIS - Host to Device
#define FIS_TYPE_REG_D2H    0x34    // Register FIS - Device to Host
#define FIS_TYPE_DMA_ACT    0x39    // DMA Activate FIS
#define FIS_TYPE_DMA_SETUP  0x41    // DMA Setup FIS
#define FIS_TYPE_DATA       0x46    // Data FIS
#define FIS_TYPE_BIST       0x58    // BIST Activate FIS
#define FIS_TYPE_PIO_SETUP  0x5F    // PIO Setup FIS
#define FIS_TYPE_DEV_BITS   0xA1    // Set Device Bits FIS

// Command Header
typedef struct {
    uint8_t cfl:5;      // Command FIS length in DWORDS
    uint8_t a:1;        // ATAPI
    uint8_t w:1;        // Write
    uint8_t p:1;        // Prefetchable
    uint8_t r:1;        // Reset
    uint8_t b:1;        // BIST
    uint8_t c:1;        // Clear busy upon R_OK
    uint8_t rsvd0:1;
    uint8_t pmp:4;      // Port multiplier port
    uint16_t prdtl;     // Physical region descriptor table length
    uint32_t prdbc;     // Physical region descriptor byte count
    uint32_t ctba;      // Command table descriptor base address
    uint32_t ctbau;     // Command table descriptor base address upper 32 bits
    uint32_t rsvd1[4];
} __attribute__((packed)) ahci_cmd_header_t;

// Physical Region Descriptor Table Entry
typedef struct {
    uint32_t dba;       // Data base address
    uint32_t dbau;      // Data base address upper 32 bits
    uint32_t rsvd0;
    uint32_t dbc:22;    // Byte count, max 4MB
    uint32_t rsvd1:9;
    uint32_t i:1;       // Interrupt on completion
} __attribute__((packed)) ahci_prdt_entry_t;

// Command Table
typedef struct {
    uint8_t cfis[64];                   // Command FIS
    uint8_t acmd[16];                   // ATAPI command
    uint8_t rsvd[48];
    ahci_prdt_entry_t prdt_entry[1];    // Physical region descriptor table
} __attribute__((packed)) ahci_cmd_table_t;

// Register FIS - Host to Device
typedef struct {
    uint8_t fis_type;   // FIS_TYPE_REG_H2D
    uint8_t pmport:4;   // Port multiplier
    uint8_t rsvd0:3;
    uint8_t c:1;        // 1: Command, 0: Control
    uint8_t command;    // Command register
    uint8_t featurel;   // Feature register, 7:0
    uint8_t lba0;       // LBA low register, 7:0
    uint8_t lba1;       // LBA mid register, 15:8
    uint8_t lba2;       // LBA high register, 23:16
    uint8_t device;     // Device register
    uint8_t lba3;       // LBA register, 31:24
    uint8_t lba4;       // LBA register, 39:32
    uint8_t lba5;       // LBA register, 47:40
    uint8_t featureh;   // Feature register, 15:8
    uint8_t countl;     // Count register, 7:0
    uint8_t counth;     // Count register, 15:8
    uint8_t icc;        // Isochronous command completion
    uint8_t control;    // Control register
    uint8_t rsvd1[4];
} __attribute__((packed)) fis_reg_h2d_t;

typedef struct {
    uint64_t bar5_base;     // AHCI registers base address
    uint32_t bar5_size;
    struct pci_device *pci_dev;
    bool initialized;
    uint32_t ports_implemented;
    uint32_t num_ports;
    uint32_t port_sectors[32];  // Sector count for each port
    void *cmd_list[32];         // Command list for each port
    void *fis_base[32];         // FIS base for each port
    void *cmd_table[32];        // Command table for each port
    void *dma_bounce;           // Reused DMA buffer (kalloc'd at init)
    uint8_t active_port;        // Port we actually use for transfers
} ahci_controller_t;

// AHCI interrupt vector
#define AHCI_IRQ_VECTOR 48

// Interrupt status bits we care about
#define AHCI_PORT_IS_DHRS   (1 << 0)   // Device to Host Register FIS
#define AHCI_PORT_IS_PSS    (1 << 1)   // PIO Setup FIS
#define AHCI_PORT_IS_DSS    (1 << 2)   // DMA Setup FIS
#define AHCI_PORT_IS_SDBS   (1 << 3)   // Set Device Bits FIS
#define AHCI_PORT_IS_DPS    (1 << 5)   // Descriptor Processed

// Port interrupt enable mask (for normal operation)
#define AHCI_PORT_IE_NORMAL (AHCI_PORT_IS_DHRS | AHCI_PORT_IS_PSS | \
                             AHCI_PORT_IS_DSS  | AHCI_PORT_IS_SDBS | \
                             AHCI_PORT_IS_TFES)

// Function declarations
void ahci_init(void);
int  ahci_register_storage(void);
int  ahci_read_sectors(storage_device_t *dev, uint64_t lba, uint32_t sector_count, void *buffer);
int  ahci_write_sectors(storage_device_t *dev, uint64_t lba, uint32_t sector_count, const void *buffer);
int  ahci_flush(storage_device_t *dev);

// Interrupt handler – called from IDT dispatcher
void ahci_handle_interrupt(void);