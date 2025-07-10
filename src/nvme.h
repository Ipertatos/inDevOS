#pragma once
#include "types.h"
#include "storage.h"

// NVMe Admin Commands
#define NVME_ADMIN_CREATE_SQ    0x01
#define NVME_ADMIN_CREATE_CQ    0x05
#define NVME_ADMIN_IDENTIFY     0x06

// NVMe I/O Commands  
#define NVME_CMD_READ           0x02
#define NVME_CMD_WRITE          0x01

// NVMe Controller Registers (offsets from BAR0)
#define NVME_REG_CAP            0x00    // Controller Capabilities
#define NVME_REG_VS             0x08    // Version
#define NVME_REG_INTMS          0x0c    // Interrupt Mask Set
#define NVME_REG_INTMC          0x10    // Interrupt Mask Clear
#define NVME_REG_CC             0x14    // Controller Configuration
#define NVME_REG_CSTS           0x1c    // Controller Status
#define NVME_REG_AQA            0x24    // Admin Queue Attributes
#define NVME_REG_ASQ            0x28    // Admin Submission Queue Base Address
#define NVME_REG_ACQ            0x30    // Admin Completion Queue Base Address

// Controller Configuration Register bits
#define NVME_CC_ENABLE          (1 << 0)
#define NVME_CC_CSS_NVM         (0 << 4)
#define NVME_CC_MPS_4K          (0 << 7)
#define NVME_CC_AMS_RR          (0 << 11)
#define NVME_CC_SHN_NONE        (0 << 14)
#define NVME_CC_IOSQES_64       (6 << 16)
#define NVME_CC_IOCQES_16       (4 << 20)

// Controller Status Register bits
#define NVME_CSTS_RDY           (1 << 0)
#define NVME_CSTS_CFS           (1 << 1)
#define NVME_CSTS_SHST_MASK     (3 << 2)

typedef struct {
    uint64_t bar0_base;
    uint32_t bar0_size;
    struct pci_device *pci_dev;
    bool initialized;
    uint32_t max_queue_entries;
    uint32_t namespace_count;
    uint64_t namespace_size;  // Size in sectors
} nvme_controller_t;

typedef struct {
    uint32_t cdw0;
    uint32_t nsid;
    uint64_t rsvd1;
    uint64_t metadata;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t cdw10;
    uint32_t cdw11;
    uint32_t cdw12;
    uint32_t cdw13;
    uint32_t cdw14;
    uint32_t cdw15;
} __attribute__((packed)) nvme_command_t;

typedef struct {
    uint32_t result;
    uint32_t rsvd;
    uint16_t sq_head;
    uint16_t sq_id;
    uint16_t cid;
    uint16_t status;
} __attribute__((packed)) nvme_completion_t;

// Function declarations
void nvme_init();
int nvme_register_storage();
int nvme_read_sectors(storage_device_t *dev, uint64_t lba, uint32_t sector_count, void *buffer);
int nvme_write_sectors(uint8_t device_id, uint64_t lba, uint32_t sector_count, const void *buffer);
int nvme_flush(uint8_t device_id);