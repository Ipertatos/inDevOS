#include "pci.h"
#include "acpi.h"
#include "mem.h"
#include "serial.h"
#include "registers.h"

struct pci_device *pci_devices[PCI_MAX_DEVICES];
int pci_device_count = 0;

/* ECAM windows from the ACPI MCFG table. The table may hold several
 * allocation entries (one per segment / bus range). Only segment 0 is
 * reachable through this driver. Per the PCI Firmware spec the entry's base
 * address corresponds to bus 0 (not start_bus), so a function's config space
 * is base + (bus << 20 | dev << 15 | fn << 12) for start_bus <= bus <= end_bus.
 * Buses outside every window are never touched through ECAM (reading past the
 * window hits unrelated MMIO and yields phantom devices). */
#define PCI_MCFG_MAX_ENTRIES 16
#define PCI_MCFG_HDR_SIZE    44u   /* 36-byte SDT header + 8 reserved */
#define PCI_MCFG_ENTRY_SIZE  16u
static struct { uint64_t base; uint8_t start_bus, end_bus; } pci_ecam[PCI_MCFG_MAX_ENTRIES];
static int pci_ecam_count = -1;    /* -1: MCFG not parsed yet */

static void pci_ecam_parse(void)
{
    pci_ecam_count = 0;
    if (mcfg == NULL)
        return;
    uint32_t len = mcfg->desc.length;
    const uint8_t *tbl = (const uint8_t *)mcfg;
    for (uint32_t off = PCI_MCFG_HDR_SIZE;
         off + PCI_MCFG_ENTRY_SIZE <= len && pci_ecam_count < PCI_MCFG_MAX_ENTRIES;
         off += PCI_MCFG_ENTRY_SIZE) {
        uint64_t base = *(const uint64_t *)(tbl + off);
        uint16_t seg = *(const uint16_t *)(tbl + off + 8);
        uint8_t sb = tbl[off + 10], eb = tbl[off + 11];
        serial_printf("PCI: MCFG entry base=0x%llx seg=%u bus %u-%u\n",
                      (unsigned long long)base, seg, sb, eb);
        if (seg != 0 || base == 0 || eb < sb)
            continue;
        pci_ecam[pci_ecam_count].base = base;
        pci_ecam[pci_ecam_count].start_bus = sb;
        pci_ecam[pci_ecam_count].end_bus = eb;
        pci_ecam_count++;
    }
}

/* Virtual address of the 4 KiB ECAM config page for bus:dev.fn, or 0 when
 * no ECAM window decodes that bus. */
static uint64_t pci_ecam_addr(uint8_t bus, uint8_t device, uint8_t function)
{
    if (pci_ecam_count < 0)
        pci_ecam_parse();
    for (int i = 0; i < pci_ecam_count; i++) {
        if (bus >= pci_ecam[i].start_bus && bus <= pci_ecam[i].end_bus)
            return pci_ecam[i].base + vmm_higher_half_offset
                 + ((uint64_t)bus << 20)
                 + ((uint64_t)(device & 0x1Fu) << 15)
                 + ((uint64_t)(function & 0x7u) << 12);
    }
    return 0;
}

// Get device string representation of the specified class code and subclass code
const char* get_device_str(uint8_t class_code, uint8_t sub_class_code){
    switch(class_code){
        case 0x01:
            switch(sub_class_code){
                case 0x01:
                    return "Mass Storage Controller - IDE Interface";
                case 0x06:
                    return "Mass Storage Controller - Serial ATA Controller";
                case 0x08:
                    return "Mass Storage Controller - Non-Volatile Memory Controller";
                default:
                    printf("PCI: %d:%d not found\n", class_code, sub_class_code);
                    return "Unknown Device";
            }
        case 0x02:
            switch (sub_class_code){
                case 0x00:
                    // printf("PCI: %x:%x\n", class_code, sub_class_code);
                    return "Network Controller - Ethernet Controller";
                default:
                    printf("PCI: %d:%d not found\n", class_code, sub_class_code);
                    return "Unknown Device";
            }
        case 0x03:
            switch (sub_class_code){
                case 0x00:
                    return "Display Controller - VGA Compatible Controller";
                default:
                    printf("PCI: %d:%d not found\n", class_code, sub_class_code);
                    return "Unknown Device";
            }
        case 0x04:
            switch (sub_class_code){
                case 0x01:
                    return "Multimedia Controller - Audio Device";
                case 0x03:
                    return "Multimedia Controller - HD Audio";
                default:
                    printf("PCI: %d:%d not found\n", class_code, sub_class_code);
                    return "Multimedia Controller - Unknown";
            }
        case 0x06:
            switch(sub_class_code){
                case 0x00:
                    return "Bridge - Host Bridge";
                case 0x01:
                    return "Bridge - ISA Bridge";
                default:
                    printf("PCI: %d:%d not found\n", class_code, sub_class_code);
                    return "Unknown Device";
            }
        case 0x0C:
            switch(sub_class_code){
                case 0x05:
                    return "Serial Bus Controller - USB Controller";
                default:
                    printf("PCI: %d:%d not found\n", class_code, sub_class_code);
                    return "Unknown Device";
            }
    }
    printf("PCI: %d:%d not found\n", class_code, sub_class_code);
    return "Unknown Device";
}


/* Legacy PCI configuration mechanism #1 (I/O ports 0xCF8 / 0xCFC).
 * Used when ACPI MCFG / ECAM is absent (Hyper-V and some other VMs).
 * Only the 256-byte legacy config space is reachable this way. */
#define PCI_LEGACY_CONFIG_ADDRESS 0xCF8
#define PCI_LEGACY_CONFIG_DATA    0xCFC

static uint32_t pci_legacy_config_addr(uint8_t bus, uint8_t device, uint8_t function, uint32_t offset)
{
    return 0x80000000u
         | ((uint32_t)bus << 16)
         | ((uint32_t)(device & 0x1Fu) << 11)
         | ((uint32_t)(function & 0x7u) << 8)
         | (offset & 0xFCu);
}

static uint32_t pci_legacy_read(struct pci_device *dev, uint32_t offset, uint8_t size)
{
    if (offset > 0xFF || (size != 1 && size != 2 && size != 4)) {
        DEBUG_SERIAL("PCI: legacy config read rejected off=0x%x size=%d\n", offset, size);
        return 0;
    }
    outl(PCI_LEGACY_CONFIG_ADDRESS,
         pci_legacy_config_addr(dev->bus, dev->device, dev->function, offset));
    uint32_t data = inl(PCI_LEGACY_CONFIG_DATA);
    uint32_t shift = (offset & 3u) * 8u;
    if (size == 1)
        return (data >> shift) & 0xFFu;
    if (size == 2)
        return (data >> shift) & 0xFFFFu;
    return data;
}

static void pci_legacy_write(struct pci_device *dev, uint32_t offset, uint8_t size, uint32_t val)
{
    if (offset > 0xFF || (size != 1 && size != 2 && size != 4)) {
        DEBUG_SERIAL("PCI: legacy config write rejected off=0x%x size=%d\n", offset, size);
        return;
    }
    uint32_t addr = pci_legacy_config_addr(dev->bus, dev->device, dev->function, offset);
    outl(PCI_LEGACY_CONFIG_ADDRESS, addr);
    if (size == 4) {
        outl(PCI_LEGACY_CONFIG_DATA, val);
        return;
    }
    uint32_t data = inl(PCI_LEGACY_CONFIG_DATA);
    uint32_t shift = (offset & 3u) * 8u;
    uint32_t mask = (size == 1) ? 0xFFu : 0xFFFFu;
    data = (data & ~(mask << shift)) | ((val & mask) << shift);
    outl(PCI_LEGACY_CONFIG_ADDRESS, addr);
    outl(PCI_LEGACY_CONFIG_DATA, data);
}

// read from pci configuration for a pci device
uint32_t pci_device_read(struct pci_device *dev, uint32_t offset, uint8_t size){
    if (dev == NULL) {
        printf("PCI: pci_device_read called with NULL device\n");
        DEBUG_SERIAL("PCI: pci_device_read called with NULL device\n");
        return 0;
    }
    /* No ECAM window: use mechanism #1 instead of faulting or returning 0
     * (a zero vendor id would look like a present device). */
    uint64_t base = pci_ecam_addr(dev->bus, dev->device, dev->function);
    if (base == 0) {
        /* No usable ECAM at all: mechanism #1. ECAM present but not decoding
         * this bus: nothing can live there, read as an absent function. */
        if (pci_ecam_count == 0)
            return pci_legacy_read(dev, offset, size);
        return size == 1 ? 0xFFu : size == 2 ? 0xFFFFu : 0xFFFFFFFFu;
    }
    if (offset > 0xFFC) {
        DEBUG_SERIAL("PCI: config read offset 0x%x out of range\n", offset);
        return 0;
    }
    uint64_t addr = base + offset;
    uint32_t result = 0;
    switch (size) {
    case 1:
        result = *(uint8_t*)addr;
        break;
    case 2:
        result = *(uint16_t*)addr;
        break;
    case 4:
        result = *(uint32_t*)addr;
        break;
    default:
        DEBUG_SERIAL("PCI: Invalid read size %d\n", size);
        return 0;
    }
    return result;
}

//write to pci configuration for a pci device
void pci_device_write(struct pci_device *dev, uint32_t offset, uint8_t size, uint32_t val)
{
    if (dev == NULL)
        return;
    uint64_t base = pci_ecam_addr(dev->bus, dev->device, dev->function);
    if (base == 0) {
        if (pci_ecam_count == 0)
            pci_legacy_write(dev, offset, size, val);
        return;
    }
    if (offset > 0xFFC)
        return;
    uint64_t addr = base + offset;

    switch (size)
    {
    case 1:
        *(uint8_t*)addr = (uint8_t)val;
        return;
    case 2:
        *(uint16_t*)addr = (uint16_t)val;
        return;
    case 4:
        *(uint32_t*)addr = (uint32_t)val;
        return;
    }
}

void pci_mm_write(struct pci_device *dev, uint32_t offset, uint32_t val) {
    pci_device_write(dev, offset, 4, val);
}

uint16_t pci_mm_read_vendor_id(uint8_t bus, uint8_t device, uint8_t function)
{
    struct pci_device temp_dev = {.bus = bus, .device = device, .function = function};
    uint16_t reg0 = pci_mm_read(&temp_dev, 0x0) & 0xFFFF;
    return reg0;
}

uint8_t pci_mm_read_header_type(uint8_t bus, uint8_t device, uint8_t function)
{   
    struct pci_device temp_dev = {.bus = bus, .device = device, .function = function};
    uint32_t reg3 = pci_mm_read(&temp_dev, 0xC);
    return (reg3 >> 16) & 0xFF;
}

/*
 * Reads from PCI MM config for a devices class code.
*/
uint8_t pci_mm_read_class_code(uint8_t bus, uint8_t device, uint8_t function)
{
    struct pci_device temp_dev = {.bus = bus, .device = device, .function = function};
    uint32_t reg2 = pci_mm_read(&temp_dev, 0x8);
    uint8_t classcode = (uint8_t)(reg2 >> 24);

    return classcode;
}

/*
 * Reads from PCI MM config for a devices sub-class code.
*/
uint8_t pci_mm_read_subclass_code(uint8_t bus, uint8_t device, uint8_t function)
{
    struct pci_device temp_dev = {.bus = bus, .device = device, .function = function};
    uint32_t reg2 = pci_mm_read(&temp_dev, 0x8);
    uint8_t subclass = (uint8_t)(reg2 >> 16);
    return subclass;
}

/*
 * Reads from PCI MM config for a devices prog IF.
*/
uint8_t pci_mm_read_prog_if(uint8_t bus, uint8_t device, uint8_t function)
{
    struct pci_device temp_dev = {.bus = bus, .device = device, .function = function};
    uint32_t reg2 = pci_mm_read(&temp_dev, 0x8);
    uint8_t progif = (uint8_t)(reg2 >> 8);
    return progif;
}

/*
 * Inspects a found device at the specific PCI bus, device, function location.
*/
void check_function(uint8_t bus, uint8_t device, uint8_t function, uint16_t header_type)
{
    // Read vendor/device IDs first -- skip non-existent devices before allocating
    struct pci_device temp_dev = {.bus = bus, .device = device, .function = function};
    uint16_t vendor_id = pci_mm_read(&temp_dev, 0x0) & 0xFFFF;
    if (vendor_id == 0xFFFF) return;
    uint16_t device_id = (pci_mm_read(&temp_dev, 0x0) >> 16) & 0xFFFF;

    uint8_t classcode = pci_mm_read_class_code(bus, device, function);
    uint8_t subclass = pci_mm_read_subclass_code(bus, device, function);
    uint8_t progif = pci_mm_read_prog_if(bus, device, function);

    if (pci_device_count >= PCI_MAX_DEVICES) {
        printf("PCI: device table full (%d), ignoring %02x:%02x.%x\n",
               PCI_MAX_DEVICES, bus, device, function);
        return;
    }
    /* Never register the same bus:dev.fn twice. */
    for (int i = 0; i < pci_device_count; i++) {
        struct pci_device *d = pci_devices[i];
        if (d->bus == bus && d->device == device && d->function == function) {
            serial_printf("PCI: duplicate %02x:%02x.%x ignored\n", bus, device, function);
            return;
        }
    }

    // Create the new device and store it in the device list.
    struct pci_device *dev = kalloc(sizeof(struct pci_device));
    
    // Check if allocation was successful
    if (dev == NULL) {
        printf("PCI: Failed to allocate memory for PCI device\n");
        DEBUG_SERIAL("PCI: Failed to allocate memory for PCI device\n");
        return;
    }

    dev->vendor_id = vendor_id;
    dev->device_id = device_id;
    
    pci_devices[pci_device_count++] = dev;
    dev->class_code = classcode;
    dev->subclass_code = subclass;
    dev->prog_if = progif;
    dev->description = get_device_str(classcode, subclass);
    dev->bus = bus;
    dev->function = function;
    dev->device = device;
    dev->header_type = header_type;
    /* 0 when no ECAM window covers the bus; config access then uses 0xCF8/0xCFC */
    dev->address = pci_ecam_addr(bus, device, function);

    /* Scan for MSI/MSI-X capabilities */
    pci_scan_capabilities(dev);

    printf("PCI: %02x:%02x.%x %s (Class %02x:%02x)\n", bus, device, function,
           dev->description, classcode, subclass);
    
    // Debug storage controllers
    if (classcode == 0x01) {
        printf("PCI: Storage device created - bus=%d, device=%d, func=%d\n", 
               dev->bus, dev->device, dev->function);
        DEBUG_SERIAL("PCI: Storage device created - bus=%d, device=%d, func=%d\n", 
                     dev->bus, dev->device, dev->function);
        
        // For SATA controllers, also read and log BAR5
        if (subclass == 0x06) {
            uint32_t bar5_low = pci_device_read(dev, 0x24, 4);
            uint32_t bar5_high = pci_device_read(dev, 0x28, 4);
            DEBUG_SERIAL("PCI: SATA BAR5 - low=0x%x, high=0x%x\n", bar5_low, bar5_high);
        }
    }
}

/*
 * Checks if a device is present at the specified bus and device. 
*/
void check_device(uint8_t bus, uint8_t device)
{
    uint8_t function = 0;

    struct pci_device temp_dev = {.bus = bus, .device = device, .function = function};
    uint16_t vendorID = pci_mm_read_vendor_id(bus, device, function);
    if (vendorID == 0xFFFF) {
        return;
    }

    uint16_t headerType = pci_mm_read_header_type(bus, device, function);

    check_function(bus, device, function, headerType);

    if ((headerType & 0x80) != 0) {
        // It's a multi-function device, so check remaining functions.
        for (function = 1; function < 8; function++) {
            if (pci_mm_read_vendor_id(bus, device, function) != 0xFFFF) {
                /* each function has its own header type (0 = endpoint, 1 = bridge) */
                check_function(bus, device, function,
                               pci_mm_read_header_type(bus, device, function));
            }
        }
    }
}

/*
 * Scans every device ID on every bus ID looking for present devices.
*/
void scan_all_buses()
{
    /* Brute-force scan (no bridge recursion, so no bus is visited twice).
     * Firmware has already numbered the buses behind PCI-to-PCI bridges.
     * With ECAM only the buses decoded by an MCFG window are scanned, each
     * once even if windows overlap; with no usable window every bus is
     * probed through 0xCF8/0xCFC. */
    if (pci_ecam_count < 0)
        pci_ecam_parse();
    for (uint16_t bus = 0; bus < 256; bus++) {
        if (pci_ecam_count > 0 && pci_ecam_addr((uint8_t)bus, 0, 0) == 0)
            continue;
        for (uint8_t device = 0; device < 32; device++) {
            check_device((uint8_t)bus, device);
        }
    }
}

void pci_init()
{
    if (mcfg == NULL) {
        serial_printf("PCI: MCFG/ECAM absent, scanning via legacy 0xCF8/0xCFC\n");
        printf("PCI: no MCFG, using legacy config mechanism\n");
    }
    scan_all_buses();
    printf("PCI: Found %d PCI devices.\n", pci_device_count);
    serial_printf("PCI: Found %d PCI devices (max %d)\n", pci_device_count, PCI_MAX_DEVICES);
    printf("PCI: Initialized.\n");
}

/*
 * Finds a device by its class and sub-class code.
*/
struct pci_device* pci_find_device(uint8_t class, uint8_t subclass)
{
    for (int i = 0; i < pci_device_count; i++) {
        struct pci_device *dev = pci_devices[i];
        if (dev->class_code == class && dev->subclass_code == subclass) {
            return dev;
        }
    }

    return NULL;
}

// read from the pci configuration mm space
uint32_t pci_mm_read(struct pci_device *dev, uint32_t offset) {
    return pci_device_read(dev, offset, 4);
}

/* ============================================================================
 * PCI Capability List Walking
 * ============================================================================ */

/**
 * Walk the PCI capability list and find a capability by ID.
 * Returns the config space offset of the capability, or 0 if not found.
 */
uint8_t pci_find_capability(struct pci_device *dev, uint8_t cap_id) {
    /* Check if device has capability list (Status register bit 4) */
    uint16_t status = pci_device_read(dev, 0x06, 2);
    if (!(status & PCI_STATUS_CAP_LIST)) {
        return 0;
    }

    /* Capabilities pointer is at offset 0x34 */
    uint8_t cap_ptr = pci_device_read(dev, 0x34, 1) & 0xFC;  /* Mask low 2 bits */

    /* Walk the linked list (max 48 entries to prevent infinite loops) */
    for (int i = 0; i < 48 && cap_ptr >= 0x40; i++) {
        uint8_t id = pci_device_read(dev, cap_ptr, 1);
        if (id == cap_id) {
            return cap_ptr;
        }
        cap_ptr = pci_device_read(dev, cap_ptr + 1, 1) & 0xFC;
    }

    return 0;
}

/**
 * Scan a PCI device for MSI and MSI-X capabilities and store their offsets.
 */
void pci_scan_capabilities(struct pci_device *dev) {
    dev->msi_cap_offset = pci_find_capability(dev, PCI_CAP_ID_MSI);
    dev->msix_cap_offset = pci_find_capability(dev, PCI_CAP_ID_MSIX);
    dev->msi_enabled = false;
    dev->msix_enabled = false;

    if (dev->msi_cap_offset) {
        serial_printf("PCI: %02x:%02x.%x has MSI capability at 0x%02x\n",
                     dev->bus, dev->device, dev->function, dev->msi_cap_offset);
    }
    if (dev->msix_cap_offset) {
        uint16_t msix_ctrl = pci_device_read(dev, dev->msix_cap_offset + MSIX_MSG_CTRL, 2);
        uint16_t table_size = (msix_ctrl & MSIX_CTRL_TABLE_SIZE) + 1;
        serial_printf("PCI: %02x:%02x.%x has MSI-X capability at 0x%02x (%d vectors)\n",
                     dev->bus, dev->device, dev->function, dev->msix_cap_offset, table_size);
    }
}

/* ============================================================================
 * MSI Support
 * ============================================================================ */

/**
 * Enable MSI for a PCI device with a single interrupt vector.
 *
 * @param dev       PCI device
 * @param vector    Interrupt vector number (32-255)
 * @param dest_cpu  Destination LAPIC ID
 * @return 0 on success, -1 on failure
 */
int pci_msi_enable(struct pci_device *dev, uint8_t vector, uint8_t dest_cpu) {
    if (!dev || !dev->msi_cap_offset) {
        return -1;
    }

    uint8_t cap = dev->msi_cap_offset;

    /* Read Message Control register */
    uint16_t msg_ctrl = pci_device_read(dev, cap + MSI_MSG_CTRL, 2);
    bool is_64bit = (msg_ctrl & MSI_CTRL_64BIT) != 0;

    /* Build the MSI message address (targets a specific LAPIC) */
    uint32_t addr_lo = MSI_ADDR_BASE | ((uint32_t)dest_cpu << MSI_ADDR_DEST_ID_SHIFT);
    uint32_t addr_hi = 0;

    /* Build the MSI message data */
    uint32_t data = (vector & 0xFF) | MSI_DATA_DELIVERY_FIXED | MSI_DATA_TRIGGER_EDGE;

    /* Write message address */
    pci_device_write(dev, cap + MSI_MSG_ADDR_LO, 4, addr_lo);

    if (is_64bit) {
        pci_device_write(dev, cap + MSI_MSG_ADDR_HI, 4, addr_hi);
        pci_device_write(dev, cap + MSI_MSG_DATA_64, 2, (uint16_t)data);
    } else {
        pci_device_write(dev, cap + MSI_MSG_DATA_32, 2, (uint16_t)data);
    }

    /* Request 1 vector (MME = 000b), enable MSI */
    msg_ctrl &= ~(0x70);  /* Clear Multiple Message Enable bits */
    msg_ctrl |= MSI_CTRL_ENABLE;
    pci_device_write(dev, cap + MSI_MSG_CTRL, 2, msg_ctrl);

    /* Disable legacy INTx while MSI is active */
    pci_disable_legacy_interrupts(dev);

    dev->msi_enabled = true;

    serial_printf("PCI: MSI enabled for %02x:%02x.%x - vector %d, CPU %d\n",
                 dev->bus, dev->device, dev->function, vector, dest_cpu);

    return 0;
}

void pci_msi_disable(struct pci_device *dev) {
    if (!dev || !dev->msi_cap_offset) return;

    uint8_t cap = dev->msi_cap_offset;
    uint16_t msg_ctrl = pci_device_read(dev, cap + MSI_MSG_CTRL, 2);
    msg_ctrl &= ~MSI_CTRL_ENABLE;
    pci_device_write(dev, cap + MSI_MSG_CTRL, 2, msg_ctrl);

    /* Re-enable legacy interrupts */
    pci_enable_legacy_interrupts(dev);

    dev->msi_enabled = false;
    serial_printf("PCI: MSI disabled for %02x:%02x.%x\n",
                 dev->bus, dev->device, dev->function);
}

bool pci_msi_is_enabled(struct pci_device *dev) {
    return dev && dev->msi_enabled;
}

/* ============================================================================
 * MSI-X Support
 * ============================================================================ */

uint16_t pci_msix_table_size(struct pci_device *dev) {
    if (!dev || !dev->msix_cap_offset) return 0;
    uint16_t ctrl = pci_device_read(dev, dev->msix_cap_offset + MSIX_MSG_CTRL, 2);
    return (ctrl & MSIX_CTRL_TABLE_SIZE) + 1;
}

/**
 * Enable MSI-X for a PCI device.
 * After enabling, configure individual vectors with pci_msix_configure_vector().
 */
int pci_msix_enable(struct pci_device *dev) {
    if (!dev || !dev->msix_cap_offset) return -1;

    uint8_t cap = dev->msix_cap_offset;

    /* Disable legacy INTx */
    pci_disable_legacy_interrupts(dev);

    /* Set the enable bit and function mask to configure vectors */
    uint16_t ctrl = pci_device_read(dev, cap + MSIX_MSG_CTRL, 2);
    ctrl |= MSIX_CTRL_ENABLE | MSIX_CTRL_FUNC_MASK;
    pci_device_write(dev, cap + MSIX_MSG_CTRL, 2, ctrl);

    dev->msix_enabled = true;

    /* Clear function mask after vector configuration to start delivering interrupts.
     * Callers should configure vectors with pci_msix_configure_vector() then
     * call pci_msix_unmask() when ready. For convenience, we clear it here
     * and individual entries are unmasked via vector_ctrl. */
    ctrl &= ~MSIX_CTRL_FUNC_MASK;
    pci_device_write(dev, cap + MSIX_MSG_CTRL, 2, ctrl);

    serial_printf("PCI: MSI-X enabled for %02x:%02x.%x (%d vectors)\n",
                 dev->bus, dev->device, dev->function, pci_msix_table_size(dev));

    return 0;
}

/**
 * Configure a specific MSI-X vector.
 */
int pci_msix_configure_vector(struct pci_device *dev, uint16_t index,
                               uint8_t vector, uint8_t dest_cpu) {
    if (!dev || !dev->msix_cap_offset || !dev->msix_enabled) return -1;

    uint8_t cap = dev->msix_cap_offset;
    uint16_t table_size = pci_msix_table_size(dev);
    if (index >= table_size) return -1;

    /* Read table BAR and offset */
    uint32_t table_off_raw = pci_device_read(dev, cap + MSIX_TABLE_OFFSET, 4);
    uint8_t  table_bir = table_off_raw & 0x7;
    uint32_t table_offset = table_off_raw & ~0x7;

    /* Get the BAR base address */
    uint64_t bar_base = pci_read_bar(dev, table_bir);
    if (!bar_base) return -1;

    extern uint64_t vmm_higher_half_offset;
    volatile msix_table_entry_t *table =
        (volatile msix_table_entry_t *)(bar_base + vmm_higher_half_offset + table_offset);

    /* Write the entry */
    volatile msix_table_entry_t *entry = &table[index];
    entry->msg_addr_lo = MSI_ADDR_BASE | ((uint32_t)dest_cpu << MSI_ADDR_DEST_ID_SHIFT);
    entry->msg_addr_hi = 0;
    entry->msg_data = (vector & 0xFF) | MSI_DATA_DELIVERY_FIXED | MSI_DATA_TRIGGER_EDGE;
    entry->vector_ctrl = 0;  /* Unmask */

    serial_printf("PCI: MSI-X vector %d configured: vector %d -> CPU %d\n",
                 index, vector, dest_cpu);

    return 0;
}

void pci_msix_disable(struct pci_device *dev) {
    if (!dev || !dev->msix_cap_offset) return;

    uint8_t cap = dev->msix_cap_offset;
    uint16_t ctrl = pci_device_read(dev, cap + MSIX_MSG_CTRL, 2);
    ctrl &= ~MSIX_CTRL_ENABLE;
    pci_device_write(dev, cap + MSIX_MSG_CTRL, 2, ctrl);

    pci_enable_legacy_interrupts(dev);

    dev->msix_enabled = false;
    serial_printf("PCI: MSI-X disabled for %02x:%02x.%x\n",
                 dev->bus, dev->device, dev->function);
}

bool pci_msix_is_enabled(struct pci_device *dev) {
    return dev && dev->msix_enabled;
}

/* ============================================================================
 * PCI Command Register Helpers
 * ============================================================================ */

void pci_enable_bus_mastering(struct pci_device *dev) {
    uint16_t cmd = pci_device_read(dev, 0x04, 2);
    cmd |= PCI_CMD_BUS_MASTER;
    pci_device_write(dev, 0x04, 2, cmd);
}

void pci_disable_legacy_interrupts(struct pci_device *dev) {
    uint16_t cmd = pci_device_read(dev, 0x04, 2);
    cmd |= PCI_CMD_INT_DISABLE;
    pci_device_write(dev, 0x04, 2, cmd);
}

void pci_enable_legacy_interrupts(struct pci_device *dev) {
    uint16_t cmd = pci_device_read(dev, 0x04, 2);
    cmd &= ~PCI_CMD_INT_DISABLE;
    pci_device_write(dev, 0x04, 2, cmd);
}

/* ============================================================================
 * BAR Reading
 * ============================================================================ */

uint64_t pci_read_bar(struct pci_device *dev, int bar_index) {
    if (bar_index < 0 || bar_index > 5) return 0;

    uint32_t offset = 0x10 + (bar_index * 4);
    uint32_t bar_low = pci_device_read(dev, offset, 4);

    if (bar_low & 1) {
        /* I/O space BAR */
        return bar_low & ~0x3;
    }

    /* Memory space BAR */
    uint8_t type = (bar_low >> 1) & 0x3;
    uint64_t base = bar_low & ~0xF;

    if (type == 2) {
        /* 64-bit BAR */
        uint32_t bar_high = pci_device_read(dev, offset + 4, 4);
        base |= (uint64_t)bar_high << 32;
    }

    return base;
}

/* ============================================================================
 * Find device by vendor/device ID
 * ============================================================================ */

struct pci_device* pci_find_device_by_id(uint16_t vendor, uint16_t device) {
    for (int i = 0; i < pci_device_count; i++) {
        struct pci_device *dev = pci_devices[i];
        if (dev->vendor_id == vendor && dev->device_id == device) {
            return dev;
        }
    }
    return NULL;
}