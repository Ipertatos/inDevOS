/**
 * @file virtio.c
 * @brief Virtio PCI transport layer implementation
 *
 * Implements the virtio 1.0+ modern PCI transport. Handles capability
 * discovery, BAR mapping, virtqueue allocation, and buffer management.
 */

#include "virtio.h"
#include "mem.h"
#include "serial.h"
#include "utils.h"
#include "pci.h"
#include "string.h"

extern uint64_t vmm_higher_half_offset;

/* ============================================================================
 * PCI Capability Discovery
 * ============================================================================ */

/**
 * Find virtio PCI capabilities by walking the PCI capability list.
 * Virtio uses vendor-specific capabilities (cap_vndr = 0x09) with
 * different cfg_type values to indicate common, notify, ISR, and
 * device-specific configuration regions.
 */
static int virtio_find_capabilities(virtio_device_t *vdev) {
    struct pci_device *pci = vdev->pci_dev;
    uint16_t status = pci_device_read(pci, 0x06, 2);

    if (!(status & PCI_STATUS_CAP_LIST)) {
        serial_puts("virtio: device has no PCI capability list\n");
        return -1;
    }

    uint8_t cap_ptr = pci_device_read(pci, 0x34, 1) & 0xFC;
    bool found_common = false;
    bool found_notify = false;
    bool found_isr    = false;
    bool found_device = false;

    for (int i = 0; i < 48 && cap_ptr >= 0x40; i++) {
        uint8_t cap_id = pci_device_read(pci, cap_ptr, 1);

        if (cap_id == 0x09) {  /* Vendor-specific */
            uint8_t cfg_type = pci_device_read(pci, cap_ptr + 3, 1);
            uint8_t bar      = pci_device_read(pci, cap_ptr + 4, 1);
            uint32_t offset  = pci_device_read(pci, cap_ptr + 8, 4);
            uint32_t length  = pci_device_read(pci, cap_ptr + 12, 4);

            /* Map the BAR if not already mapped */
            if (bar < 6 && !vdev->bar_addrs[bar]) {
                uint64_t bar_base = pci_read_bar(pci, bar);
                if (bar_base) {
                    vdev->bar_addrs[bar] = (volatile void *)(bar_base + vmm_higher_half_offset);
                    serial_printf("virtio: mapped BAR%d at phys 0x%llx\n",
                                  bar, (unsigned long long)bar_base);
                }
            }

            volatile void *base = vdev->bar_addrs[bar];
            if (!base) {
                serial_printf("virtio: BAR%d not mapped for cfg_type %d\n", bar, cfg_type);
                cap_ptr = pci_device_read(pci, cap_ptr + 1, 1) & 0xFC;
                continue;
            }

            switch (cfg_type) {
            case VIRTIO_PCI_CAP_COMMON_CFG:
                vdev->common_cfg = (volatile virtio_pci_common_cfg_t *)
                    ((uint8_t *)base + offset);
                found_common = true;
                serial_printf("virtio: common_cfg at BAR%d+0x%x\n", bar, offset);
                break;

            case VIRTIO_PCI_CAP_NOTIFY_CFG:
                vdev->notify_base = (volatile uint16_t *)
                    ((uint8_t *)base + offset);
                /* The notify_off_multiplier is at cap_ptr + 16 */
                vdev->notify_off_multiplier = pci_device_read(pci, cap_ptr + 16, 4);
                found_notify = true;
                serial_printf("virtio: notify at BAR%d+0x%x (mult=%d)\n",
                              bar, offset, vdev->notify_off_multiplier);
                break;

            case VIRTIO_PCI_CAP_ISR_CFG:
                vdev->isr_cfg = (volatile uint8_t *)
                    ((uint8_t *)base + offset);
                found_isr = true;
                serial_printf("virtio: ISR at BAR%d+0x%x\n", bar, offset);
                break;

            case VIRTIO_PCI_CAP_DEVICE_CFG:
                vdev->device_cfg = (volatile void *)
                    ((uint8_t *)base + offset);
                found_device = true;
                serial_printf("virtio: device_cfg at BAR%d+0x%x\n", bar, offset);
                break;

            case VIRTIO_PCI_CAP_PCI_CFG:
                /* PCI configuration access capability - not needed for MMIO */
                break;
            }
        }

        cap_ptr = pci_device_read(pci, cap_ptr + 1, 1) & 0xFC;
    }

    if (!found_common || !found_notify || !found_isr) {
        serial_printf("virtio: missing required capabilities "
                      "(common=%d notify=%d isr=%d device=%d)\n",
                      found_common, found_notify, found_isr, found_device);
        return -1;
    }

    return 0;
}

/* ============================================================================
 * Device Initialization
 * ============================================================================ */

int virtio_init_device(virtio_device_t *vdev, struct pci_device *pci) {
    memset(vdev, 0, sizeof(virtio_device_t));
    vdev->pci_dev = pci;

    serial_printf("virtio: initializing device %04x:%04x at %02x:%02x.%x\n",
                  pci->vendor_id, pci->device_id,
                  pci->bus, pci->device, pci->function);

    /* Enable bus mastering and memory space */
    pci_enable_bus_mastering(pci);
    uint16_t cmd = pci_device_read(pci, 0x04, 2);
    cmd |= PCI_CMD_MEMORY_SPACE;
    pci_device_write(pci, 0x04, 2, cmd);

    /* Discover virtio PCI capabilities */
    if (virtio_find_capabilities(vdev) < 0) {
        serial_puts("virtio: capability discovery failed\n");
        return -1;
    }

    /* Reset the device */
    virtio_reset(vdev);

    /* Acknowledge the device */
    virtio_set_status(vdev, VIRTIO_STATUS_ACKNOWLEDGE);
    virtio_set_status(vdev, virtio_get_status(vdev) | VIRTIO_STATUS_DRIVER);

    vdev->num_queues = vdev->common_cfg->num_queues;
    serial_printf("virtio: device has %d queues\n", vdev->num_queues);

    return 0;
}

/* ============================================================================
 * Virtqueue Setup
 * ============================================================================ */

int virtio_setup_queue(virtio_device_t *vdev, uint16_t index, uint16_t size) {
    if (index >= VIRTIO_MAX_QUEUES || !vdev->common_cfg) return -1;

    volatile virtio_pci_common_cfg_t *cfg = vdev->common_cfg;

    /* Select the queue */
    cfg->queue_select = index;

    /* Read maximum queue size */
    uint16_t max_size = cfg->queue_size;
    if (max_size == 0) {
        serial_printf("virtio: queue %d not available\n", index);
        return -1;
    }

    if (size == 0 || size > max_size) {
        size = max_size;
    }
    if (size > VIRTQ_DEFAULT_SIZE) {
        size = VIRTQ_DEFAULT_SIZE;
    }

    /* Set the queue size */
    cfg->queue_size = size;

    virtqueue_t *vq = &vdev->queues[index];
    vq->size = size;

    /* Allocate descriptor table: 16 bytes per entry, 16-byte aligned */
    uint64_t desc_size = sizeof(virtq_desc_t) * size;
    vq->desc = (virtq_desc_t *)kalloc(desc_size);
    if (!vq->desc) return -1;
    memset(vq->desc, 0, desc_size);

    /* Allocate available ring: 6 + 2*size bytes, 2-byte aligned */
    uint64_t avail_size = sizeof(virtq_avail_t) + sizeof(uint16_t) * size;
    vq->avail = (virtq_avail_t *)kalloc(avail_size);
    if (!vq->avail) return -1;
    memset(vq->avail, 0, avail_size);

    /* Allocate used ring: 6 + 8*size bytes, 4-byte aligned */
    uint64_t used_size = sizeof(virtq_used_t) + sizeof(virtq_used_elem_t) * size;
    vq->used = (virtq_used_t *)kalloc(used_size);
    if (!vq->used) return -1;
    memset(vq->used, 0, used_size);

    /* Get physical addresses (subtract HHDM offset) */
    vq->desc_phys  = (uint64_t)vq->desc  - vmm_higher_half_offset;
    vq->avail_phys = (uint64_t)vq->avail - vmm_higher_half_offset;
    vq->used_phys  = (uint64_t)vq->used  - vmm_higher_half_offset;

    /* Initialize free list */
    vq->free_head = 0;
    vq->num_free = size;
    vq->last_used_idx = 0;
    for (uint16_t i = 0; i < size - 1; i++) {
        vq->desc[i].next = i + 1;
    }
    vq->desc[size - 1].next = 0xFFFF;  /* End of free list */

    /* Program the queue addresses into the device */
    cfg->queue_desc   = vq->desc_phys;
    cfg->queue_driver = vq->avail_phys;
    cfg->queue_device = vq->used_phys;

    /* Set up notification address */
    uint16_t notify_off = cfg->queue_notify_off;
    vq->notify_addr = (volatile uint16_t *)
        ((uint8_t *)vdev->notify_base + notify_off * vdev->notify_off_multiplier);

    /* Enable the queue */
    cfg->queue_enable = 1;

    serial_printf("virtio: queue %d set up with %d descriptors\n", index, size);

    return 0;
}

/* ============================================================================
 * Buffer Management
 * ============================================================================ */

int virtio_add_buf(virtqueue_t *vq, uint64_t *bufs, uint32_t *lens,
                   uint16_t out_num, uint16_t in_num) {
    uint16_t total = out_num + in_num;
    if (total == 0 || vq->num_free < total) return -1;

    uint16_t head = vq->free_head;
    uint16_t i = head;

    for (uint16_t n = 0; n < total; n++) {
        vq->desc[i].addr = bufs[n];
        vq->desc[i].len  = lens[n];
        vq->desc[i].flags = 0;

        if (n >= out_num) {
            vq->desc[i].flags |= VIRTQ_DESC_F_WRITE;
        }
        if (n < total - 1) {
            vq->desc[i].flags |= VIRTQ_DESC_F_NEXT;
            i = vq->desc[i].next;
        } else {
            /* Last descriptor: update free list head */
            uint16_t next_free = vq->desc[i].next;
            vq->desc[i].flags &= ~VIRTQ_DESC_F_NEXT;
            vq->free_head = next_free;
        }
    }

    vq->num_free -= total;

    /* Add to available ring */
    uint16_t avail_idx = vq->avail->idx & (vq->size - 1);
    vq->avail->ring[avail_idx] = head;

    /* Memory barrier before updating idx */
    __asm__ volatile("mfence" ::: "memory");
    vq->avail->idx++;

    return head;
}

void virtio_kick(virtqueue_t *vq) {
    /* Memory barrier to ensure all writes are visible */
    __asm__ volatile("mfence" ::: "memory");

    if (vq->notify_addr) {
        *vq->notify_addr = 0;  /* Value is queue index, but we write 0 */
    }
}

bool virtio_has_used(virtqueue_t *vq) {
    /* Memory barrier for used ring visibility */
    __asm__ volatile("lfence" ::: "memory");
    return vq->last_used_idx != vq->used->idx;
}

int virtio_get_used(virtqueue_t *vq, uint32_t *len_out) {
    if (!virtio_has_used(vq)) return -1;

    uint16_t used_idx = vq->last_used_idx & (vq->size - 1);
    virtq_used_elem_t *elem = &vq->used->ring[used_idx];

    uint32_t id = elem->id;
    if (len_out) *len_out = elem->len;

    vq->last_used_idx++;

    return id;
}

void virtio_free_desc(virtqueue_t *vq, uint16_t head) {
    uint16_t i = head;

    /* Walk the chain and return descriptors to free list */
    while (1) {
        vq->num_free++;
        bool has_next = (vq->desc[i].flags & VIRTQ_DESC_F_NEXT) != 0;
        uint16_t next = vq->desc[i].next;

        vq->desc[i].next = vq->free_head;
        vq->free_head = i;

        if (!has_next) break;
        i = next;
    }
}

/* ============================================================================
 * Device Status & Features
 * ============================================================================ */

void virtio_set_status(virtio_device_t *vdev, uint8_t status) {
    vdev->common_cfg->device_status = status;
}

uint8_t virtio_get_status(virtio_device_t *vdev) {
    return vdev->common_cfg->device_status;
}

void virtio_reset(virtio_device_t *vdev) {
    vdev->common_cfg->device_status = 0;
    /* Wait for reset to complete */
    while (vdev->common_cfg->device_status != 0) {
        __asm__ volatile("pause");
    }
}

uint64_t virtio_negotiate_features(virtio_device_t *vdev, uint64_t driver_features) {
    volatile virtio_pci_common_cfg_t *cfg = vdev->common_cfg;
    uint64_t device_features = 0;

    /* Read device features (two 32-bit halves) */
    cfg->device_feature_select = 0;
    device_features = cfg->device_feature;
    cfg->device_feature_select = 1;
    device_features |= (uint64_t)cfg->device_feature << 32;

    /* Negotiate: only accept features both sides support */
    uint64_t negotiated = device_features & driver_features;

    /* Write driver features */
    cfg->driver_feature_select = 0;
    cfg->driver_feature = (uint32_t)(negotiated & 0xFFFFFFFF);
    cfg->driver_feature_select = 1;
    cfg->driver_feature = (uint32_t)(negotiated >> 32);

    /* Set FEATURES_OK */
    virtio_set_status(vdev, virtio_get_status(vdev) | VIRTIO_STATUS_FEATURES_OK);

    /* Verify the device accepted our features */
    if (!(virtio_get_status(vdev) & VIRTIO_STATUS_FEATURES_OK)) {
        serial_puts("virtio: device rejected features\n");
        virtio_set_status(vdev, VIRTIO_STATUS_FAILED);
        return 0;
    }

    serial_printf("virtio: features negotiated: 0x%llx\n",
                  (unsigned long long)negotiated);
    return negotiated;
}

void virtio_device_ready(virtio_device_t *vdev) {
    virtio_set_status(vdev, virtio_get_status(vdev) | VIRTIO_STATUS_DRIVER_OK);
    serial_puts("virtio: device is ready\n");
}
