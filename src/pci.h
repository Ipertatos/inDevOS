#pragma once

#include "types.h"

#define pci_reg0_offset 0x00 // device id and vendor id
#define pci_reg1_offset 0x04 // status and command
#define pci_reg2_offset 0x08 // class code , subclass , prog if , rev id
#define pci_reg3_offset 0x0C // BIST , header type , latency timer , cache line size
#define pci_reg4_offset 0x10 // BAR0
#define pci_reg5_offset 0x14 // BAR1

/* PCI command register bits */
#define PCI_CMD_IO_SPACE        (1 << 0)
#define PCI_CMD_MEMORY_SPACE    (1 << 1)
#define PCI_CMD_BUS_MASTER      (1 << 2)
#define PCI_CMD_INT_DISABLE     (1 << 10)

/* PCI status register bits */
#define PCI_STATUS_CAP_LIST     (1 << 4)

/* PCI capability IDs */
#define PCI_CAP_ID_MSI          0x05
#define PCI_CAP_ID_MSIX         0x11

/* MSI message address format (for x86 LAPIC) */
#define MSI_ADDR_BASE           0xFEE00000
#define MSI_ADDR_DEST_ID_SHIFT  12
#define MSI_ADDR_REDIRECT_HINT  (1 << 3)
#define MSI_ADDR_DEST_MODE_LOG  (1 << 2)

/* MSI message data format */
#define MSI_DATA_VECTOR_SHIFT   0
#define MSI_DATA_DELIVERY_FIXED 0
#define MSI_DATA_LEVEL_ASSERT   (1 << 14)
#define MSI_DATA_TRIGGER_EDGE   0
#define MSI_DATA_TRIGGER_LEVEL  (1 << 15)

/* MSI capability structure offsets (relative to capability pointer) */
#define MSI_CAP_ID              0x00
#define MSI_CAP_NEXT            0x01
#define MSI_MSG_CTRL            0x02
#define MSI_MSG_ADDR_LO         0x04
#define MSI_MSG_ADDR_HI         0x08  /* Only for 64-bit capable */
#define MSI_MSG_DATA_32         0x08  /* For 32-bit capable */
#define MSI_MSG_DATA_64         0x0C  /* For 64-bit capable */

/* MSI Message Control register bits */
#define MSI_CTRL_ENABLE         (1 << 0)
#define MSI_CTRL_64BIT          (1 << 7)
#define MSI_CTRL_PERVEC_MASK    (1 << 8)

/* MSI-X capability structure offsets */
#define MSIX_CAP_ID             0x00
#define MSIX_CAP_NEXT           0x01
#define MSIX_MSG_CTRL           0x02
#define MSIX_TABLE_OFFSET       0x04
#define MSIX_PBA_OFFSET         0x08

/* MSI-X Message Control bits */
#define MSIX_CTRL_ENABLE        (1 << 15)
#define MSIX_CTRL_FUNC_MASK     (1 << 14)
#define MSIX_CTRL_TABLE_SIZE    0x7FF

/* MSI-X table entry structure */
typedef struct {
    uint32_t msg_addr_lo;
    uint32_t msg_addr_hi;
    uint32_t msg_data;
    uint32_t vector_ctrl;
} __attribute__((packed)) msix_table_entry_t;

#define MSIX_ENTRY_CTRL_MASKED  (1 << 0)

struct pci_device {
    uint8_t class_code;
    uint8_t subclass_code;
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint8_t prog_if;
    const char* description;
    uint64_t address;
    uint32_t header_type;
    uint16_t vendor_id;
    uint16_t device_id;
    /* MSI/MSI-X capability offsets (0 = not present) */
    uint8_t msi_cap_offset;
    uint8_t msix_cap_offset;
    bool msi_enabled;
    bool msix_enabled;
};

/* Upper bound on enumerated PCI functions. Real laptops easily exceed 32
 * (PCH functions + root ports + GPU/NVMe/Wi-Fi/USB behind bridges). */
#define PCI_MAX_DEVICES 256

extern struct pci_device *pci_devices[PCI_MAX_DEVICES];
extern int pci_device_count;

void pci_init(void);
struct pci_device* pci_find_device(uint8_t class_code, uint8_t subclass_code);
struct pci_device* pci_find_device_by_id(uint16_t vendor_id, uint16_t device_id);

uint32_t pci_device_read(struct pci_device *dev, uint32_t offset, uint8_t size);
void pci_device_write(struct pci_device *dev, uint32_t offset, uint8_t size, uint32_t val);

uint32_t pci_mm_read(struct pci_device *dev, uint32_t offset);
void pci_mm_write(struct pci_device *dev, uint32_t offset, uint32_t val);

/* PCI capability list walking */
uint8_t pci_find_capability(struct pci_device *dev, uint8_t cap_id);
void pci_scan_capabilities(struct pci_device *dev);

/* MSI support */
int pci_msi_enable(struct pci_device *dev, uint8_t vector, uint8_t dest_cpu);
void pci_msi_disable(struct pci_device *dev);
bool pci_msi_is_enabled(struct pci_device *dev);

/* MSI-X support */
int pci_msix_enable(struct pci_device *dev);
int pci_msix_configure_vector(struct pci_device *dev, uint16_t index,
                               uint8_t vector, uint8_t dest_cpu);
void pci_msix_disable(struct pci_device *dev);
bool pci_msix_is_enabled(struct pci_device *dev);
uint16_t pci_msix_table_size(struct pci_device *dev);

/* PCI command register helpers */
void pci_enable_bus_mastering(struct pci_device *dev);
void pci_disable_legacy_interrupts(struct pci_device *dev);
void pci_enable_legacy_interrupts(struct pci_device *dev);

/* BAR reading */
uint64_t pci_read_bar(struct pci_device *dev, int bar_index);