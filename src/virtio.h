/**
 * @file virtio.h
 * @brief Virtio PCI transport layer
 *
 * Implements the virtio 1.0+ PCI transport for modern virtio devices.
 * Used by virtio_gpu, virtio_net, etc.
 */

#ifndef VIRTIO_H
#define VIRTIO_H

#include "types.h"
#include "pci.h"

/* ============================================================================
 * Virtio PCI vendor/device IDs
 * ============================================================================ */

#define VIRTIO_PCI_VENDOR_ID    0x1AF4

/* Transitional device IDs (subsystem device ID indicates actual device type) */
#define VIRTIO_PCI_DEVICE_NET   0x1000  /* Network card (legacy) */
#define VIRTIO_PCI_DEVICE_BLK   0x1001  /* Block device (legacy) */
#define VIRTIO_PCI_DEVICE_CON   0x1003  /* Console (legacy) */
#define VIRTIO_PCI_DEVICE_GPU   0x1050  /* GPU device (modern only) */

/* Non-transitional (modern) device IDs: 0x1040 + device_type */
#define VIRTIO_PCI_MODERN_NET   0x1041
#define VIRTIO_PCI_MODERN_BLK   0x1042
#define VIRTIO_PCI_MODERN_CON   0x1043
#define VIRTIO_PCI_MODERN_GPU   0x1050

/* ============================================================================
 * Virtio PCI Capabilities (modern transport)
 * ============================================================================ */

#define VIRTIO_PCI_CAP_VENDOR_CFG   0x09  /* Vendor-specific PCI capability */

/* Virtio PCI capability configuration types */
#define VIRTIO_PCI_CAP_COMMON_CFG   1
#define VIRTIO_PCI_CAP_NOTIFY_CFG   2
#define VIRTIO_PCI_CAP_ISR_CFG      3
#define VIRTIO_PCI_CAP_DEVICE_CFG   4
#define VIRTIO_PCI_CAP_PCI_CFG      5

/* Virtio PCI capability structure */
typedef struct {
    uint8_t cap_vndr;       /* 0x09 for vendor-specific */
    uint8_t cap_next;
    uint8_t cap_len;
    uint8_t cfg_type;       /* VIRTIO_PCI_CAP_*_CFG */
    uint8_t bar;            /* BAR index */
    uint8_t padding[3];
    uint32_t offset;        /* Offset within BAR */
    uint32_t length;        /* Length of the structure */
} __attribute__((packed)) virtio_pci_cap_t;

/* ============================================================================
 * Virtio Common Configuration Structure (in BAR)
 * ============================================================================ */

typedef struct {
    /* About the whole device */
    uint32_t device_feature_select;     /* R/W */
    uint32_t device_feature;            /* R */
    uint32_t driver_feature_select;     /* R/W */
    uint32_t driver_feature;            /* R/W */
    uint16_t msix_config;              /* R/W */
    uint16_t num_queues;               /* R */
    uint8_t  device_status;            /* R/W */
    uint8_t  config_generation;        /* R */
    /* About a specific virtqueue */
    uint16_t queue_select;             /* R/W */
    uint16_t queue_size;               /* R/W */
    uint16_t queue_msix_vector;        /* R/W */
    uint16_t queue_enable;             /* R/W */
    uint16_t queue_notify_off;         /* R */
    uint64_t queue_desc;               /* R/W */
    uint64_t queue_driver;             /* R/W - available ring */
    uint64_t queue_device;             /* R/W - used ring */
} __attribute__((packed)) virtio_pci_common_cfg_t;

/* ============================================================================
 * Device Status Bits
 * ============================================================================ */

#define VIRTIO_STATUS_ACKNOWLEDGE   1
#define VIRTIO_STATUS_DRIVER        2
#define VIRTIO_STATUS_DRIVER_OK     4
#define VIRTIO_STATUS_FEATURES_OK   8
#define VIRTIO_STATUS_DEVICE_NEEDS_RESET 64
#define VIRTIO_STATUS_FAILED        128

/* ============================================================================
 * Virtqueue Structures (Split Virtqueue)
 * ============================================================================ */

/* Descriptor flags */
#define VIRTQ_DESC_F_NEXT       1   /* Buffer continues in next descriptor */
#define VIRTQ_DESC_F_WRITE      2   /* Buffer is device-writable (vs read) */
#define VIRTQ_DESC_F_INDIRECT   4   /* Contains a list of descriptors */

/* Descriptor table entry */
typedef struct {
    uint64_t addr;      /* Physical address of buffer */
    uint32_t len;       /* Length of buffer */
    uint16_t flags;     /* VIRTQ_DESC_F_* */
    uint16_t next;      /* Next descriptor index (if F_NEXT) */
} __attribute__((packed)) virtq_desc_t;

/* Available ring */
typedef struct {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];    /* Variable length: ring[queue_size] */
} __attribute__((packed)) virtq_avail_t;

/* Used ring element */
typedef struct {
    uint32_t id;        /* Index of start of used descriptor chain */
    uint32_t len;       /* Total bytes written to the descriptor chain */
} __attribute__((packed)) virtq_used_elem_t;

/* Used ring */
typedef struct {
    uint16_t flags;
    uint16_t idx;
    virtq_used_elem_t ring[];   /* Variable length */
} __attribute__((packed)) virtq_used_t;

/* ============================================================================
 * Virtqueue Management
 * ============================================================================ */

#define VIRTIO_MAX_QUEUES   4
#define VIRTQ_DEFAULT_SIZE  128

typedef struct {
    uint16_t size;          /* Number of descriptors */
    uint16_t free_head;     /* Index of first free descriptor */
    uint16_t last_used_idx; /* Last processed used ring index */
    uint16_t num_free;      /* Number of free descriptors */

    /* Pointers into allocated memory */
    virtq_desc_t  *desc;
    virtq_avail_t *avail;
    virtq_used_t  *used;

    /* Physical addresses for device programming */
    uint64_t desc_phys;
    uint64_t avail_phys;
    uint64_t used_phys;

    /* Notification */
    volatile uint16_t *notify_addr;
} virtqueue_t;

/* ============================================================================
 * Virtio Device Structure
 * ============================================================================ */

typedef struct {
    struct pci_device *pci_dev;

    /* BAR mappings */
    volatile void *bar_addrs[6];

    /* Mapped configuration regions */
    volatile virtio_pci_common_cfg_t *common_cfg;
    volatile uint8_t *isr_cfg;
    volatile void *device_cfg;
    volatile uint16_t *notify_base;
    uint32_t notify_off_multiplier;

    /* Virtqueues */
    virtqueue_t queues[VIRTIO_MAX_QUEUES];
    uint16_t num_queues;

    /* Device type */
    uint16_t device_type;
} virtio_device_t;

/* ============================================================================
 * API
 * ============================================================================ */

/**
 * @brief Initialize a virtio PCI device
 *
 * Finds the virtio capability structures, maps BARs, and prepares
 * the device for driver use.
 *
 * @param vdev   Output virtio device structure
 * @param pci    PCI device to initialize
 * @return 0 on success, -1 on failure
 */
int virtio_init_device(virtio_device_t *vdev, struct pci_device *pci);

/**
 * @brief Allocate and set up a virtqueue
 *
 * @param vdev   Virtio device
 * @param index  Queue index
 * @param size   Queue size (0 = use device default)
 * @return 0 on success, -1 on failure
 */
int virtio_setup_queue(virtio_device_t *vdev, uint16_t index, uint16_t size);

/**
 * @brief Add a buffer to a virtqueue (available ring)
 *
 * @param vq       Virtqueue
 * @param bufs     Array of buffer addresses
 * @param lens     Array of buffer lengths
 * @param out_num  Number of device-readable (outgoing) buffers
 * @param in_num   Number of device-writable (incoming) buffers
 * @return Descriptor head index, or -1 on failure
 */
int virtio_add_buf(virtqueue_t *vq, uint64_t *bufs, uint32_t *lens,
                   uint16_t out_num, uint16_t in_num);

/**
 * @brief Notify the device that buffers are available
 *
 * @param vq  Virtqueue to notify
 */
void virtio_kick(virtqueue_t *vq);

/**
 * @brief Check if the device has used any buffers
 *
 * @param vq  Virtqueue to check
 * @return true if there are used buffers to process
 */
bool virtio_has_used(virtqueue_t *vq);

/**
 * @brief Get the next used buffer from the device
 *
 * @param vq       Virtqueue
 * @param len_out  Output: bytes written by device (can be NULL)
 * @return Descriptor head index, or -1 if no used buffers
 */
int virtio_get_used(virtqueue_t *vq, uint32_t *len_out);

/**
 * @brief Free a descriptor chain back to the free list
 *
 * @param vq    Virtqueue
 * @param head  Head descriptor index
 */
void virtio_free_desc(virtqueue_t *vq, uint16_t head);

/**
 * @brief Set device status bits
 */
void virtio_set_status(virtio_device_t *vdev, uint8_t status);

/**
 * @brief Read device status
 */
uint8_t virtio_get_status(virtio_device_t *vdev);

/**
 * @brief Negotiate features with the device
 *
 * @param vdev      Virtio device
 * @param features  Bitmask of features the driver supports
 * @return Negotiated feature bitmask
 */
uint64_t virtio_negotiate_features(virtio_device_t *vdev, uint64_t features);

/**
 * @brief Complete device initialization (set DRIVER_OK)
 */
void virtio_device_ready(virtio_device_t *vdev);

/**
 * @brief Reset the virtio device
 */
void virtio_reset(virtio_device_t *vdev);

#endif /* VIRTIO_H */
