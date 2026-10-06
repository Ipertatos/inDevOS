#include "ahci.h"
#include "pci.h"
#include "storage.h"
#include "mem.h"
#include "utils.h"
#include "serial.h"
#include "debug_config.h"
#include "atomic.h"
#include "apic.h"
#include "ktime.h"   /* ktime_deadline_ms/ktime_expired for AHCI completion waits */

static ahci_controller_t ahci_ctrl = {0};
static uint8_t ahci_storage_device_id = 0xFF;
/* storage device_id -> AHCI port. 0xFF means "not an AHCI disk". */
static uint8_t ahci_dev_port[MAX_STORAGE_DEVICES];

static uint8_t ahci_port_of(storage_device_t *dev) {
    if (dev && dev->device_id < MAX_STORAGE_DEVICES &&
        ahci_dev_port[dev->device_id] != 0xFF)
        return ahci_dev_port[dev->device_id];
    return ahci_ctrl.active_port;
}

// Global AHCI buffers - guaranteed to be in mapped kernel memory
static uint8_t ahci_global_cmd_list[1024] __attribute__((aligned(1024)));
static uint8_t ahci_global_fis[256] __attribute__((aligned(256)));
static uint8_t ahci_global_cmd_table[128] __attribute__((aligned(128)));
#define AHCI_DMA_BOUNCE_BYTES (128 * 512)

static spinlock_t ahci_lock = {0};

// ---- Interrupt-driven completion tracking -----------------------------------
// Per-port completion flag: set by ISR, cleared by command issuer.
static volatile uint32_t ahci_port_complete[32];
// Per-port error flag: set by ISR if TFES.
static volatile uint32_t ahci_port_error[32];

// Helper functions for register access
static uint32_t ahci_read_reg32(uint32_t offset) {
    if (!ahci_ctrl.bar5_base) {
        DEBUG_SERIAL("AHCI: BAR5 not initialized\n");
        return 0;
    }
    return *(volatile uint32_t*)(ahci_ctrl.bar5_base + offset);
}

static void ahci_write_reg32(uint32_t offset, uint32_t value) {
    if (!ahci_ctrl.bar5_base) {
        DEBUG_SERIAL("AHCI: BAR5 not initialized\n");
        return;
    }
    *(volatile uint32_t*)(ahci_ctrl.bar5_base + offset) = value;
}

static uint32_t ahci_read_port_reg32(uint8_t port, uint32_t offset) {
    if (!ahci_ctrl.bar5_base || port >= 32) {
        DEBUG_SERIAL("AHCI: Invalid port or BAR5 not initialized\n");
        return 0;
    }
    uint32_t port_base = AHCI_REG_PORTS + (port * 0x80);
    return *(volatile uint32_t*)(ahci_ctrl.bar5_base + port_base + offset);
}

static void ahci_write_port_reg32(uint8_t port, uint32_t offset, uint32_t value) {
    if (!ahci_ctrl.bar5_base || port >= 32) {
        DEBUG_SERIAL("AHCI: Invalid port or BAR5 not initialized\n");
        return;
    }
    uint32_t port_base = AHCI_REG_PORTS + (port * 0x80);
    *(volatile uint32_t*)(ahci_ctrl.bar5_base + port_base + offset) = value;
}

// Wait for port to be ready
static int ahci_port_wait_ready(uint8_t port) {
    int timeout = 10000;
    
    while (timeout--) {
        uint32_t tfd = ahci_read_port_reg32(port, AHCI_PORT_TFD);
        if (!(tfd & 0x88)) { // Not BSY and not DRQ
            return 0;
        }
        // Small delay
        for (volatile int i = 0; i < 1000; i++);
    }
    
    return -1; // Timeout
}

// Initialize a single AHCI port
static int ahci_init_port(uint8_t port) {
    printf("AHCI: Initializing port %d\n", port);
    
    // Check if device is present (be lenient for QEMU)
    uint32_t ssts = ahci_read_port_reg32(port, AHCI_PORT_SSTS);
    uint32_t det = ssts & AHCI_PORT_SSTS_DET_MASK;
    printf("AHCI: Port %d SSTS=0x%x, DET=0x%x\n", port, ssts, det);
    
    // Be more lenient - accept any detection state except "no device"
    if (det == 0) {
        printf("AHCI: No device detected on port %d\n", port);
        return -1;
    }
    printf("AHCI: Device detected on port %d (DET=0x%x)\n", port, det);
    
    // Stop port operations
    uint32_t cmd = ahci_read_port_reg32(port, AHCI_PORT_CMD);
    cmd &= ~(AHCI_PORT_CMD_ST | AHCI_PORT_CMD_FRE);
    ahci_write_port_reg32(port, AHCI_PORT_CMD, cmd);
    
    // Wait for port to stop
    int timeout = 1000;
    while (timeout--) {
        cmd = ahci_read_port_reg32(port, AHCI_PORT_CMD);
        if (!(cmd & (AHCI_PORT_CMD_CR | AHCI_PORT_CMD_FR))) {
            break;
        }
    }
    
    if (timeout <= 0) {
        printf("AHCI: Port %d failed to stop\n", port);
        return -1;
    }
    
    // Allocate memory for command list, FIS, and command table
    // Command list: 32 entries * 32 bytes = 1KB, aligned to 1KB
    ahci_ctrl.cmd_list[port] = kalloc(1024);
    if (!ahci_ctrl.cmd_list[port]) {
        printf("AHCI: Failed to allocate command list for port %d\n", port);
        return -1;
    }
    memset(ahci_ctrl.cmd_list[port], 0, 1024);
    
    // FIS: 256 bytes, aligned to 256 bytes
    ahci_ctrl.fis_base[port] = kalloc(256);
    if (!ahci_ctrl.fis_base[port]) {
        printf("AHCI: Failed to allocate FIS for port %d\n", port);
        return -1;
    }
    memset(ahci_ctrl.fis_base[port], 0, 256);
    
    // Command table: allocate 256 bytes to satisfy 128-byte alignment requirement
    ahci_ctrl.cmd_table[port] = kalloc(256);
    if (!ahci_ctrl.cmd_table[port]) {
        printf("AHCI: Failed to allocate command table for port %d\n", port);
        return -1;
    }
    memset(ahci_ctrl.cmd_table[port], 0, 256);
    
    // Set command list and FIS base addresses - convert virtual to physical
    uint64_t cmd_list_virt = (uint64_t)ahci_ctrl.cmd_list[port];
    uint64_t fis_virt = (uint64_t)ahci_ctrl.fis_base[port];
    uint64_t cmd_list_phys = cmd_list_virt - vmm_higher_half_offset;
    uint64_t fis_phys = fis_virt - vmm_higher_half_offset;
    
    ahci_write_port_reg32(port, AHCI_PORT_CLB, (uint32_t)cmd_list_phys);
    ahci_write_port_reg32(port, AHCI_PORT_CLBU, (uint32_t)(cmd_list_phys >> 32));
    ahci_write_port_reg32(port, AHCI_PORT_FB, (uint32_t)fis_phys);
    ahci_write_port_reg32(port, AHCI_PORT_FBU, (uint32_t)(fis_phys >> 32));
    
    // Debug address conversion
    serial_printf("AHCI INIT: CmdList V=0x%x P=0x%x, FIS V=0x%x P=0x%x\n",
                 (uint32_t)cmd_list_virt, (uint32_t)cmd_list_phys,
                 (uint32_t)fis_virt, (uint32_t)fis_phys);
    
    // Setup command header - convert virtual to physical
    ahci_cmd_header_t *cmd_header = (ahci_cmd_header_t*)ahci_ctrl.cmd_list[port];
    uint64_t cmd_table_virt = (uint64_t)ahci_ctrl.cmd_table[port];
    uint64_t cmd_table_phys = cmd_table_virt - vmm_higher_half_offset;
    cmd_header->ctba = (uint32_t)cmd_table_phys;
    cmd_header->ctbau = (uint32_t)(cmd_table_phys >> 32);
    
    serial_printf("AHCI INIT: CmdTable V=0x%x P=0x%x\n",
                 (uint32_t)cmd_table_virt, (uint32_t)cmd_table_phys);
    
    // Clear port interrupt status
    ahci_write_port_reg32(port, AHCI_PORT_IS, 0xFFFFFFFF);
    
    // Enable FIS receive (FRE) and power up / spin up (POD | SUD)
    cmd = ahci_read_port_reg32(port, AHCI_PORT_CMD);
    cmd |= AHCI_PORT_CMD_FRE | AHCI_PORT_CMD_POD | AHCI_PORT_CMD_SUD;
    ahci_write_port_reg32(port, AHCI_PORT_CMD, cmd);

    // Finally start the command engine (ST)
    cmd |= AHCI_PORT_CMD_ST;
    ahci_write_port_reg32(port, AHCI_PORT_CMD, cmd);
    
    printf("AHCI: Port %d initialized successfully\n", port);
    return 0;
}

// Issue IDENTIFY command to get drive capacity
static int ahci_identify_drive(uint8_t port) {
    if (ahci_port_wait_ready(port) != 0) {
        printf("AHCI: Port %d not ready for IDENTIFY\n", port);
        return -1;
    }
    
    // Allocate buffer for identify data
    uint16_t *identify_buffer = kalloc(512);
    if (!identify_buffer) {
        printf("AHCI: Failed to allocate identify buffer\n");
        return -1;
    }
    memset(identify_buffer, 0, 512);
    
    // Setup command table
    ahci_cmd_table_t *cmd_table = (ahci_cmd_table_t*)ahci_ctrl.cmd_table[port];
    memset(cmd_table, 0, sizeof(ahci_cmd_table_t));
    
    // Setup FIS
    fis_reg_h2d_t *fis = (fis_reg_h2d_t*)cmd_table->cfis;
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->c = 1; // Command
    fis->command = ATA_CMD_IDENTIFY;
    
    // Setup PRDT - convert virtual to physical address
    uint64_t buffer_virt = (uint64_t)identify_buffer;
    uint64_t buffer_phys = buffer_virt - vmm_higher_half_offset;
    
    cmd_table->prdt_entry[0].dba = (uint32_t)buffer_phys;
    cmd_table->prdt_entry[0].dbau = (uint32_t)(buffer_phys >> 32);
    cmd_table->prdt_entry[0].dbc = 511; // 512 bytes - 1
    cmd_table->prdt_entry[0].i = 1; // Interrupt on completion
    
    // Debug the address conversion
    serial_printf("AHCI IDENTIFY: buffer V=0x%x P=0x%x\n", 
                 (uint32_t)buffer_virt, (uint32_t)buffer_phys);
    
    // Setup command header
    ahci_cmd_header_t *cmd_header = (ahci_cmd_header_t*)ahci_ctrl.cmd_list[port];
    cmd_header->cfl = sizeof(fis_reg_h2d_t) / 4; // FIS length in DWORDS
    cmd_header->w = 0; // Read
    cmd_header->prdtl = 1; // One PRDT entry
    cmd_header->prdbc = 0; // Will be filled by hardware
    
    // Issue command
    ahci_write_port_reg32(port, AHCI_PORT_CI, 1); // Issue command slot 0
    
    // Wait for command completion
    int timeout = 10000;
    while (timeout--) {
        uint32_t ci = ahci_read_port_reg32(port, AHCI_PORT_CI);
        if (!(ci & 1)) { // Command completed
            break;
        }
    }
    
    if (timeout <= 0) {
        printf("AHCI: IDENTIFY command timeout on port %d\n", port);
        kfree(identify_buffer);
        return -1;
    }
    
    // Get sector count from identify data (words 60-61 for LBA28)
    uint32_t sectors = identify_buffer[60] | (identify_buffer[61] << 16);
    
    // For LBA48, check words 100-103 if LBA28 reports max value
    if (sectors == 0x0FFFFFFF) {
        sectors = identify_buffer[100] | (identify_buffer[101] << 16);
    }
    
    if (sectors == 0) {
        printf("AHCI: Drive on port %d reports 0 sectors\n", port);
        kfree(identify_buffer);
        return -1;
    }
    
    ahci_ctrl.port_sectors[port] = sectors;
    uint32_t size_mb = (sectors * 512) / (1024 * 1024);
    
    printf("AHCI: Port %d capacity: %d sectors (%d MB)\n", port, sectors, size_mb);
    
    kfree(identify_buffer);
    return 0;
}

// ---- AHCI Interrupt Handler -------------------------------------------------
void ahci_handle_interrupt(void) {
    if (!ahci_ctrl.initialized) return;

    // Read global interrupt status (which ports have pending interrupts)
    uint32_t global_is = ahci_read_reg32(AHCI_REG_IS);
    if (global_is == 0) return;

    for (uint8_t port = 0; port < 32; port++) {
        if (!(global_is & (1 << port))) continue;

        // Read and clear port interrupt status
        uint32_t port_is = ahci_read_port_reg32(port, AHCI_PORT_IS);
        ahci_write_port_reg32(port, AHCI_PORT_IS, port_is);  // W1C

        if (port_is & AHCI_PORT_IS_TFES) {
            ahci_port_error[port] = 1;
            serial_printf("AHCI IRQ: Port %d TFES error, IS=0x%x\n", port, port_is);
        }

        // Signal completion (D2H register FIS or any transfer-complete indication)
        if (port_is & (AHCI_PORT_IS_DHRS | AHCI_PORT_IS_PSS | AHCI_PORT_IS_DSS |
                       AHCI_PORT_IS_SDBS | AHCI_PORT_IS_TFES)) {
            ahci_port_complete[port] = 1;
        }
    }

    // Clear global interrupt status
    ahci_write_reg32(AHCI_REG_IS, global_is);

    // Send End-of-Interrupt to LAPIC so further interrupts (including HPET timer
    // and subsequent AHCI interrupts) are not blocked.
    apic_eoi();
}

/**
 * Wait for command completion using interrupt-driven notification.
 * Falls back to polling if the interrupt doesn't fire within timeout.
 * Returns 0 on success, -1 on timeout/error.
 */
static int ahci_wait_completion(uint8_t port, int timeout_ms) {
    /* Fast path: the completion interrupt already fired. */
    if (ahci_port_complete[port]) {
        ahci_port_complete[port] = 0;
        return ahci_port_error[port] ? -1 : 0;
    }

    /* Poll PxCI rather than halting until the next interrupt.
     *
     * The old loop did "sti; hlt; cli" and only re-checked completion after
     * each halt, so it could not observe completion more than once per timer
     * tick.  Profiling showed 13 x 128-sector reads costing 946 ms (~60 ms per
     * command = one tick each even though the transfer itself is far quicker).
     * Polling returns as soon as the controller clears CI; the interrupt fast
     * path above is still used when the IRQ does fire. */
    uint64_t deadline = ktime_deadline_ms((uint64_t)timeout_ms);

    for (;;) {
        /* Reading PxCI back as 0 means the command has left the queue.  The
         * HBA latches the bit written by the caller before it starts work, so
         * a clear bit here means completion, not "not started yet". */
        uint32_t ci = ahci_read_port_reg32(port, AHCI_PORT_CI);
        if ((ci & 1) == 0) {
            uint32_t tfd = ahci_read_port_reg32(port, AHCI_PORT_TFD);
            return (tfd & 0x1) ? -1 : 0;
        }

        if (ahci_port_complete[port]) {
            ahci_port_complete[port] = 0;
            if (ahci_port_error[port]) {
                ahci_port_error[port] = 0;
                return -1;
            }
            return 0;
        }

        if (ktime_expired(deadline)) {
            break;
        }

        __asm__ volatile("pause");
    }

    printf("AHCI: Command timeout on port %d\n", port);
    return -1;
}

void ahci_init(void) {
    if (ahci_ctrl.initialized) {
        DEBUG_SERIAL("AHCI: already initialized\n");
        return;
    }
    DEBUG_SERIAL("AHCI: Starting AHCI initialization\n");
    
    struct pci_device* dev = pci_find_device(0x01, 0x06);
    if (dev == NULL) {
        DEBUG_SERIAL("AHCI: No SATA controller found (class 01:06)\n");
        return;
    }
    
    DEBUG_SERIAL("AHCI: Found SATA controller at bus %d, device %d, func %d\n", 
           dev->bus, dev->device, dev->function);
    
    ahci_ctrl.pci_dev = dev;
    
    // Read BAR5 (AHCI registers)
    uint32_t bar5_low = pci_device_read(dev, 0x24, 4);
    uint32_t bar5_high = pci_device_read(dev, 0x28, 4);
    
    DEBUG_SERIAL("AHCI: BAR5 registers - low: 0x%x, high: 0x%x\n", bar5_low, bar5_high);
    
    // Calculate base address and validate
    ahci_ctrl.bar5_base = bar5_low & 0xFFFFFFF0;
    if ((bar5_low & 0x6) == 0x4) { // 64-bit BAR
        ahci_ctrl.bar5_base |= ((uint64_t)bar5_high) << 32;
    }
    
    if (ahci_ctrl.bar5_base == 0) {
        DEBUG_SERIAL("AHCI: Invalid BAR5 address\n");
        return;
    }
    
    // Add higher half offset for virtual memory mapping
    extern uint64_t vmm_higher_half_offset;
    uint64_t original_base = ahci_ctrl.bar5_base;
    ahci_ctrl.bar5_base += vmm_higher_half_offset;
    
    DEBUG_SERIAL("AHCI: BAR5 physical: 0x%x, virtual: 0x%x\n", original_base, ahci_ctrl.bar5_base);
    
    // Enable bus mastering and memory space
    uint16_t cmd = pci_device_read(dev, 0x04, 2);
    cmd |= 0x06; // Enable memory space and bus mastering
    pci_device_write(dev, 0x04, 2, cmd);
    
    // Enable AHCI mode
    uint32_t ghc = ahci_read_reg32(AHCI_REG_GHC);
    ghc |= AHCI_GHC_AHCI_EN;
    ahci_write_reg32(AHCI_REG_GHC, ghc);
    
    // Read capabilities and ports implemented
    uint32_t cap = ahci_read_reg32(AHCI_REG_CAP);
    uint32_t pi = ahci_read_reg32(AHCI_REG_PI);
    
    ahci_ctrl.ports_implemented = pi;
    ahci_ctrl.num_ports = (cap & 0x1F) + 1;
    
    DEBUG_SERIAL("AHCI: %d ports, implemented mask: 0x%x\n", 
                 ahci_ctrl.num_ports, ahci_ctrl.ports_implemented);
    
    DEBUG_SERIAL("AHCI: Beginning per-port initialization\n");

    bool port_found = false;
    for (uint8_t port = 0; port < 32; port++) {
        if (!(pi & (1 << port))) {
            continue; /* Port not in port-implemented mask */
        }

        DEBUG_SERIAL("AHCI: Attempting to init port %d\n", port);
        if (ahci_init_port(port) != 0) {
            continue; // failed
        }

        if (ahci_identify_drive(port) != 0) {
            printf("AHCI: IDENTIFY failed on port %d – skipping\n", port);
            continue;
        }

        /* Keep every detected disk. The first one stays the boot device. */
        if (!port_found)
            ahci_ctrl.active_port = port;
        port_found = true;
    }

    if (!port_found) {
        printf("AHCI: No usable ports found – aborting init\n");
        return;
    }

    /* kalloc once — static BSS was not DMA-reachable; per-read kalloc was slow. */
    if (!ahci_ctrl.dma_bounce) {
        ahci_ctrl.dma_bounce = kalloc(AHCI_DMA_BOUNCE_BYTES);
        if (!ahci_ctrl.dma_bounce) {
            printf("AHCI: Failed to allocate DMA bounce buffer\n");
            return;
        }
    }

    ahci_ctrl.initialized = true;

    // ---- Enable interrupt-driven I/O ----------------------------------------

    // Clear completion/error flags
    memset((void*)ahci_port_complete, 0, sizeof(ahci_port_complete));
    memset((void*)ahci_port_error,    0, sizeof(ahci_port_error));

    /* Enable interrupts on every port that identified a disk. */
    for (uint8_t port = 0; port < 32; port++) {
        if (ahci_ctrl.port_sectors[port] == 0)
            continue;
        ahci_write_port_reg32(port, AHCI_PORT_IE, AHCI_PORT_IE_NORMAL);
        ahci_write_port_reg32(port, AHCI_PORT_IS, 0xFFFFFFFF);
    }

    // Enable global AHCI interrupts
    ghc = ahci_read_reg32(AHCI_REG_GHC);
    ghc |= AHCI_GHC_IE;
    ahci_write_reg32(AHCI_REG_GHC, ghc);
    // Clear global IS
    ahci_write_reg32(AHCI_REG_IS, 0xFFFFFFFF);

    // Route the PCI interrupt to our IDT vector via IOAPIC
    uint8_t pci_irq = pci_device_read(dev, 0x3C, 1) & 0xFF;
    if (pci_irq != 0 && pci_irq != 0xFF) {
        ioapic_redirect_irq(0, AHCI_IRQ_VECTOR, pci_irq, true);
        printf("AHCI: Interrupt-driven I/O enabled (IRQ %d -> vector %d)\n",
               pci_irq, AHCI_IRQ_VECTOR);
    } else {
        printf("AHCI: No PCI IRQ assigned – using polling fallback\n");
    }

    // Register SATA storage device with proper size
    ahci_register_storage();
}

int ahci_register_storage() {
    if (!ahci_ctrl.initialized) {
        return -1;
    }
    
    // Find a free storage device slot
    extern storage_device_t storage_devices[MAX_STORAGE_DEVICES];
    extern uint8_t storage_device_count;
    
    if (storage_device_count >= MAX_STORAGE_DEVICES) {
        DEBUG_SERIAL("AHCI: No free storage device slots\n");
        return -1;
    }
    
    memset(ahci_dev_port, 0xFF, sizeof(ahci_dev_port));
    ahci_storage_device_id = 0xFF;
    int registered = 0;

    for (uint8_t port = 0; port < 32; port++) {
        if (!(ahci_ctrl.ports_implemented & (1u << port)))
            continue;
        if (ahci_ctrl.port_sectors[port] == 0)
            continue;
        if (storage_device_count >= MAX_STORAGE_DEVICES) {
            printf("AHCI: no free storage slot for port %u\n", port);
            break;
        }

        uint8_t id = storage_device_count;
        storage_device_t *ahci_device = &storage_devices[id];
        memset(ahci_device, 0, sizeof(*ahci_device));
        ahci_device->device_id = id;
        ahci_device->sector_count = ahci_ctrl.port_sectors[port];
        ahci_device->sector_size = 512;
        ahci_device->is_initialized = true;
        ahci_device->pci_dev = ahci_ctrl.pci_dev;
        ahci_device->base_address = ahci_ctrl.bar5_base;
        ahci_device->name[0] = 'S';
        ahci_device->name[1] = 'A';
        ahci_device->name[2] = 'T';
        ahci_device->name[3] = 'A';
        if (port >= 10) {
            ahci_device->name[4] = (char)('0' + port / 10);
            ahci_device->name[5] = (char)('0' + port % 10);
        } else {
            ahci_device->name[4] = (char)('0' + port);
        }
        ahci_dev_port[id] = port;
        if (ahci_storage_device_id == 0xFF)
            ahci_storage_device_id = id;
        storage_device_count++;
        registered++;
        printf("AHCI: Registered port %u as storage device %u (%u sectors)\n",
               port, id, ahci_ctrl.port_sectors[port]);
    }

    if (!registered) {
        DEBUG_SERIAL("AHCI: No working port found\n");
        return -1;
    }
    return 0;
}

// AHCI DMA read implementation
int ahci_read_sectors(storage_device_t *dev, uint64_t lba, uint32_t sector_count, void *buffer) {
    spinlock_lock(&ahci_lock);
    if (!ahci_ctrl.initialized || !dev || !buffer) {
        printf("AHCI: Invalid parameters for read\n");
        spinlock_unlock(&ahci_lock);
        return -1;
    }
    //printf("AHCI: Reading %d sectors starting at LBA %llu\n", sector_count, lba);

    uint8_t port = ahci_port_of(dev);

    uint32_t sectors_remaining = sector_count;
    uint8_t *buf_ptr = (uint8_t*)buffer;

    const uint16_t MAX_SECTORS_PER_OP = 128;
    void *dma_bounce = ahci_ctrl.dma_bounce;
    if (!dma_bounce) {
        spinlock_unlock(&ahci_lock);
        return -1;
    }

    while (sectors_remaining > 0) {
        uint16_t sectors_this = (sectors_remaining > MAX_SECTORS_PER_OP) ? MAX_SECTORS_PER_OP : (uint16_t)sectors_remaining;

        if (ahci_port_wait_ready(port) != 0) {
            printf("AHCI: Port %d not ready for read\n", port);
            spinlock_unlock(&ahci_lock);
            return -1;
        }

        // Ensure slot 0 is free
        int slot_timeout = 100000;
        while (slot_timeout-- && (ahci_read_port_reg32(port, AHCI_PORT_CI) & 1)) {
            // Busy wait until previous command clears
        }
        if (slot_timeout <= 0) {
            printf("AHCI: Timeout waiting for free command slot (read)\n");
            spinlock_unlock(&ahci_lock);
            return -1;
        }

        // Setup command table
        ahci_cmd_table_t *cmd_table = (ahci_cmd_table_t*)ahci_ctrl.cmd_table[port];
        memset(cmd_table, 0, sizeof(ahci_cmd_table_t));

        // Setup PRDT - convert virtual to physical for DMA buffer (bounce buffer)
        uint64_t buf_virt = (uint64_t)dma_bounce;
        extern uint64_t vmm_higher_half_offset;
        uint64_t buf_phys = buf_virt - vmm_higher_half_offset;

        cmd_table->prdt_entry[0].dba = (uint32_t)buf_phys;
        cmd_table->prdt_entry[0].dbau = (uint32_t)(buf_phys >> 32);
        cmd_table->prdt_entry[0].dbc = (sectors_this * 512) - 1; // byte count = N*512 -1
        cmd_table->prdt_entry[0].i = 1;

        // Setup the command FIS
        fis_reg_h2d_t *fis = (fis_reg_h2d_t*)cmd_table->cfis;
        memset(fis, 0, sizeof(fis_reg_h2d_t));
        fis->fis_type = FIS_TYPE_REG_H2D;
        fis->c = 1; // command
        fis->command = ATA_CMD_READ_DMA_EXT;
        fis->device = 1 << 6; // LBA mode

        uint64_t lba_tmp = lba;
        fis->lba0 = (uint8_t)(lba_tmp & 0xFF);
        fis->lba1 = (uint8_t)((lba_tmp >> 8) & 0xFF);
        fis->lba2 = (uint8_t)((lba_tmp >> 16) & 0xFF);
        fis->lba3 = (uint8_t)((lba_tmp >> 24) & 0xFF);
        fis->lba4 = (uint8_t)((lba_tmp >> 32) & 0xFF);
        fis->lba5 = (uint8_t)((lba_tmp >> 40) & 0xFF);

        fis->countl = (uint8_t)(sectors_this & 0xFF);
        fis->counth = (uint8_t)((sectors_this >> 8) & 0xFF);

        // Setup command header
        ahci_cmd_header_t *cmd_header = (ahci_cmd_header_t*)ahci_ctrl.cmd_list[port];
        uint64_t cmd_table_phys = ((uint64_t)ahci_ctrl.cmd_table[port]) - vmm_higher_half_offset;
        memset(cmd_header, 0, sizeof(ahci_cmd_header_t));
        cmd_header->cfl = sizeof(fis_reg_h2d_t) / 4; // 20 bytes /4 =5
        cmd_header->w = 0; // read
        cmd_header->p = 0;
        cmd_header->prdtl = 1;
        cmd_header->ctba = (uint32_t)cmd_table_phys;
        cmd_header->ctbau = (uint32_t)(cmd_table_phys >> 32);

        // Clear completion flag and port interrupt status before issuing
        ahci_port_complete[port] = 0;
        ahci_port_error[port]    = 0;
        ahci_write_port_reg32(port, AHCI_PORT_IS, 0xFFFFFFFF);

        // Issue command in slot 0
        ahci_write_port_reg32(port, AHCI_PORT_CI, 1);

        // Wait for completion (interrupt-driven with polling fallback)
        if (ahci_wait_completion(port, 1000) != 0) {
            printf("AHCI: Read error/timeout – port %d\n", port);
            spinlock_unlock(&ahci_lock);
            return -1;
        }

        // Copy data from bounce buffer to caller's buffer
        memmove(buf_ptr, dma_bounce, sectors_this * 512);

        // Advance pointers/counters
        lba += sectors_this;
        buf_ptr += sectors_this * 512;
        sectors_remaining -= sectors_this;
    }

    spinlock_unlock(&ahci_lock);
    return 0;
}

int ahci_write_sectors(storage_device_t *dev, uint64_t lba, uint32_t sector_count, const void *buffer) {
    spinlock_lock(&ahci_lock);
    if (!ahci_ctrl.initialized || !dev || !buffer) {
        printf("AHCI: Invalid parameters for write\n");
        spinlock_unlock(&ahci_lock);
        return -1;
    }

    SERIAL_DEBUG_PRINT(AHCI, "AHCI: Writing %d sectors starting at LBA %llu\n", sector_count, lba);

    uint8_t port = ahci_port_of(dev);

    uint32_t sectors_remaining = sector_count;
    const uint8_t *buf_ptr = (const uint8_t*)buffer;

    const uint16_t MAX_SECTORS_PER_OP = 128;
    void *dma_bounce = ahci_ctrl.dma_bounce;
    if (!dma_bounce) {
        spinlock_unlock(&ahci_lock);
        return -1;
    }

    while (sectors_remaining > 0) {
        uint16_t sectors_this = (sectors_remaining > MAX_SECTORS_PER_OP) ? MAX_SECTORS_PER_OP : (uint16_t)sectors_remaining;

        // Copy from caller buffer into DMA bounce
        memmove(dma_bounce, buf_ptr, sectors_this * 512);

        // Ensure slot 0 is free
        int slot_timeout = 100000;
        while (slot_timeout-- && (ahci_read_port_reg32(port, AHCI_PORT_CI) & 1)) {}
        if (slot_timeout <= 0) {
            printf("AHCI: Timeout waiting for free command slot (write)\n");
            spinlock_unlock(&ahci_lock);
            return -1;
        }

        if (ahci_port_wait_ready(port) != 0) {
            printf("AHCI: Port %d not ready for write\n", port);
            spinlock_unlock(&ahci_lock);
            return -1;
        }

        // Setup command table
        ahci_cmd_table_t *cmd_table = (ahci_cmd_table_t*)ahci_ctrl.cmd_table[port];
        memset(cmd_table, 0, sizeof(ahci_cmd_table_t));

        // PRDT entry pointing to bounce buffer
        uint64_t buf_virt = (uint64_t)dma_bounce;
        extern uint64_t vmm_higher_half_offset;
        uint64_t buf_phys = buf_virt - vmm_higher_half_offset;

        cmd_table->prdt_entry[0].dba = (uint32_t)buf_phys;
        cmd_table->prdt_entry[0].dbau = (uint32_t)(buf_phys >> 32);
        cmd_table->prdt_entry[0].dbc = (sectors_this * 512) - 1;
        cmd_table->prdt_entry[0].i = 1;

        // Setup FIS for write
        fis_reg_h2d_t *fis = (fis_reg_h2d_t*)cmd_table->cfis;
        memset(fis, 0, sizeof(fis_reg_h2d_t));
        fis->fis_type = FIS_TYPE_REG_H2D;
        fis->c = 1;
        fis->command = ATA_CMD_WRITE_DMA_EXT;
        fis->device = 1 << 6; // LBA mode

        uint64_t lba_tmp = lba;
        fis->lba0 = (uint8_t)(lba_tmp & 0xFF);
        fis->lba1 = (uint8_t)((lba_tmp >> 8) & 0xFF);
        fis->lba2 = (uint8_t)((lba_tmp >> 16) & 0xFF);
        fis->lba3 = (uint8_t)((lba_tmp >> 24) & 0xFF);
        fis->lba4 = (uint8_t)((lba_tmp >> 32) & 0xFF);
        fis->lba5 = (uint8_t)((lba_tmp >> 40) & 0xFF);

        fis->countl = (uint8_t)(sectors_this & 0xFF);
        fis->counth = (uint8_t)((sectors_this >> 8) & 0xFF);

        // Command header
        ahci_cmd_header_t *cmd_header = (ahci_cmd_header_t*)ahci_ctrl.cmd_list[port];
        uint64_t cmd_table_phys = ((uint64_t)ahci_ctrl.cmd_table[port]) - vmm_higher_half_offset;
        memset(cmd_header, 0, sizeof(ahci_cmd_header_t));
        cmd_header->cfl = sizeof(fis_reg_h2d_t) / 4;
        cmd_header->w = 1; // write
        cmd_header->p = 0;
        cmd_header->prdtl = 1;
        cmd_header->ctba = (uint32_t)cmd_table_phys;
        cmd_header->ctbau = (uint32_t)(cmd_table_phys >> 32);

        // Clear completion flag and port interrupt status, issue command
        ahci_port_complete[port] = 0;
        ahci_port_error[port]    = 0;
        ahci_write_port_reg32(port, AHCI_PORT_IS, 0xFFFFFFFF);
        ahci_write_port_reg32(port, AHCI_PORT_CI, 1);

        // Wait for completion (interrupt-driven with polling fallback)
        if (ahci_wait_completion(port, 1000) != 0) {
            printf("AHCI: Write error/timeout – port %d\n", port);
            spinlock_unlock(&ahci_lock);
            return -1;
        }

        // Advance
        lba += sectors_this;
        buf_ptr += sectors_this * 512;
        sectors_remaining -= sectors_this;
    }

    spinlock_unlock(&ahci_lock);
    return 0;
}

int ahci_flush(storage_device_t *dev) {
    (void)dev;
    spinlock_lock(&ahci_lock);
    if (!ahci_ctrl.initialized) {
        spinlock_unlock(&ahci_lock);
        return -1;
    }

    uint8_t port = ahci_port_of(dev);

    int slot_timeout = 100000;
    while (slot_timeout-- && (ahci_read_port_reg32(port, AHCI_PORT_CI) & 1)) {}
    if (slot_timeout <= 0) {
        printf("AHCI: flush — timeout waiting for free command slot\n");
        spinlock_unlock(&ahci_lock);
        return -1;
    }

    if (ahci_port_wait_ready(port) != 0) {
        spinlock_unlock(&ahci_lock);
        return -1;
    }

    ahci_cmd_table_t *cmd_table = (ahci_cmd_table_t *)ahci_ctrl.cmd_table[port];
    memset(cmd_table, 0, sizeof(ahci_cmd_table_t));

    fis_reg_h2d_t *fis = (fis_reg_h2d_t *)cmd_table->cfis;
    memset(fis, 0, sizeof(fis_reg_h2d_t));
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->c = 1;
    fis->command = ATA_CMD_FLUSH_CACHE_EXT;
    fis->device = 1 << 6;

    ahci_cmd_header_t *cmd_header = (ahci_cmd_header_t *)ahci_ctrl.cmd_list[port];
    extern uint64_t vmm_higher_half_offset;
    uint64_t cmd_table_phys = ((uint64_t)ahci_ctrl.cmd_table[port]) - vmm_higher_half_offset;
    memset(cmd_header, 0, sizeof(ahci_cmd_header_t));
    cmd_header->cfl = sizeof(fis_reg_h2d_t) / 4;
    cmd_header->w = 0;
    cmd_header->prdtl = 0;
    cmd_header->ctba = (uint32_t)cmd_table_phys;
    cmd_header->ctbau = (uint32_t)(cmd_table_phys >> 32);

    ahci_port_complete[port] = 0;
    ahci_port_error[port] = 0;
    ahci_write_port_reg32(port, AHCI_PORT_IS, 0xFFFFFFFF);
    ahci_write_port_reg32(port, AHCI_PORT_CI, 1);

    int wret = ahci_wait_completion(port, 5000);
    spinlock_unlock(&ahci_lock);
    if (wret != 0) {
        printf("AHCI: flush command timeout / error on port %d\n", port);
        return -1;
    }
    return 0;
} 