#pragma once
#include "types.h"
#include "storage.h"

/* NVMe admin opcodes */
#define NVME_ADMIN_DELETE_SQ    0x00
#define NVME_ADMIN_CREATE_SQ    0x01
#define NVME_ADMIN_DELETE_CQ    0x04
#define NVME_ADMIN_CREATE_CQ    0x05
#define NVME_ADMIN_IDENTIFY     0x06
#define NVME_ADMIN_SET_FEATURES 0x09

/* NVM command set opcodes */
#define NVME_CMD_FLUSH          0x00
#define NVME_CMD_WRITE          0x01
#define NVME_CMD_READ           0x02

/* Identify CNS values */
#define NVME_ID_CNS_NS          0x00
#define NVME_ID_CNS_CTRL        0x01
#define NVME_ID_CNS_NS_ACTIVE   0x02

/* Controller registers (offsets from BAR0) */
#define NVME_REG_CAP            0x00    /* Controller Capabilities (64-bit) */
#define NVME_REG_VS             0x08    /* Version */
#define NVME_REG_INTMS          0x0c    /* Interrupt Mask Set */
#define NVME_REG_INTMC          0x10    /* Interrupt Mask Clear */
#define NVME_REG_CC             0x14    /* Controller Configuration */
#define NVME_REG_CSTS           0x1c    /* Controller Status */
#define NVME_REG_AQA            0x24    /* Admin Queue Attributes */
#define NVME_REG_ASQ            0x28    /* Admin SQ Base Address (64-bit) */
#define NVME_REG_ACQ            0x30    /* Admin CQ Base Address (64-bit) */
#define NVME_REG_DBS            0x1000  /* Doorbells start */

/* CAP fields */
#define NVME_CAP_MQES(c)        ((uint32_t)((c) & 0xFFFF))          /* 0-based */
#define NVME_CAP_TO(c)          ((uint32_t)(((c) >> 24) & 0xFF))    /* 500 ms units */
#define NVME_CAP_DSTRD(c)       ((uint32_t)(((c) >> 32) & 0xF))
#define NVME_CAP_CSS_NVM(c)     (((c) >> 37) & 1)
#define NVME_CAP_MPSMIN(c)      ((uint32_t)(((c) >> 48) & 0xF))
#define NVME_CAP_MPSMAX(c)      ((uint32_t)(((c) >> 52) & 0xF))

/* CC bits */
#define NVME_CC_ENABLE          (1u << 0)
#define NVME_CC_CSS_NVM         (0u << 4)
#define NVME_CC_MPS_4K          (0u << 7)
#define NVME_CC_AMS_RR          (0u << 11)
#define NVME_CC_SHN_MASK        (3u << 14)
#define NVME_CC_SHN_NORMAL      (1u << 14)
#define NVME_CC_IOSQES_64       (6u << 16)
#define NVME_CC_IOCQES_16       (4u << 20)

/* CSTS bits */
#define NVME_CSTS_RDY           (1u << 0)
#define NVME_CSTS_CFS           (1u << 1)
#define NVME_CSTS_SHST_MASK     (3u << 2)
#define NVME_CSTS_SHST_DONE     (2u << 2)

typedef struct {
    uint32_t cdw0;      /* opcode [7:0], fuse, psdt, cid [31:16] */
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
    uint16_t status;    /* bit 0 = phase tag, [8:1] SC, [11:9] SCT, [14] M, [15] DNR */
} __attribute__((packed)) nvme_completion_t;

void nvme_init(void);
int nvme_register_storage(void);
/* Block-layer entry points (512-byte logical sectors, also on 4K-LBA namespaces) */
int nvme_read_sectors(storage_device_t *dev, uint64_t lba, uint32_t sector_count, void *buffer);
int nvme_write_sectors(storage_device_t *dev, uint64_t lba, uint32_t sector_count, const void *buffer);
int nvme_flush(storage_device_t *dev);
void nvme_print_info(void);
int nvme_usable_count(void);
