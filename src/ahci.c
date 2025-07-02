#include "ahci.h"
#include "pci.h"
#include "storage.h"
#include "mem.h"
#include "utils.h"
#include "serial.h"

static ahci_controller_t ahci_ctrl = {0};
static uint8_t ahci_storage_device_id = 0xFF;

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
    printf("AHCI: Initializing port {d}{n}", port);
    
    // Check if device is present
    uint32_t ssts = ahci_read_port_reg32(port, AHCI_PORT_SSTS);
    if ((ssts & AHCI_PORT_SSTS_DET_MASK) != AHCI_PORT_SSTS_DET_PRESENT) {
        printf("AHCI: No device on port {d}{n}", port);
        return -1;
    }
    
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
        printf("AHCI: Port {d} failed to stop{n}", port);
        return -1;
    }
    
    // Allocate memory for command list, FIS, and command table
    // Command list: 32 entries * 32 bytes = 1KB, aligned to 1KB
    ahci_ctrl.cmd_list[port] = kalloc(1024);
    if (!ahci_ctrl.cmd_list[port]) {
        printf("AHCI: Failed to allocate command list for port {d}{n}", port);
        return -1;
    }
    memset(ahci_ctrl.cmd_list[port], 0, 1024);
    
    // FIS: 256 bytes, aligned to 256 bytes
    ahci_ctrl.fis_base[port] = kalloc(256);
    if (!ahci_ctrl.fis_base[port]) {
        printf("AHCI: Failed to allocate FIS for port {d}{n}", port);
        return -1;
    }
    memset(ahci_ctrl.fis_base[port], 0, 256);
    
    // Command table: 128 bytes per entry, we'll use just one
    ahci_ctrl.cmd_table[port] = kalloc(128);
    if (!ahci_ctrl.cmd_table[port]) {
        printf("AHCI: Failed to allocate command table for port {d}{n}", port);
        return -1;
    }
    memset(ahci_ctrl.cmd_table[port], 0, 128);
    
    // Set command list and FIS base addresses
    uint64_t cmd_list_addr = (uint64_t)ahci_ctrl.cmd_list[port];
    uint64_t fis_addr = (uint64_t)ahci_ctrl.fis_base[port];
    
    ahci_write_port_reg32(port, AHCI_PORT_CLB, (uint32_t)cmd_list_addr);
    ahci_write_port_reg32(port, AHCI_PORT_CLBU, (uint32_t)(cmd_list_addr >> 32));
    ahci_write_port_reg32(port, AHCI_PORT_FB, (uint32_t)fis_addr);
    ahci_write_port_reg32(port, AHCI_PORT_FBU, (uint32_t)(fis_addr >> 32));
    
    // Setup command header
    ahci_cmd_header_t *cmd_header = (ahci_cmd_header_t*)ahci_ctrl.cmd_list[port];
    uint64_t cmd_table_addr = (uint64_t)ahci_ctrl.cmd_table[port];
    cmd_header->ctba = (uint32_t)cmd_table_addr;
    cmd_header->ctbau = (uint32_t)(cmd_table_addr >> 32);
    
    // Clear port interrupt status
    ahci_write_port_reg32(port, AHCI_PORT_IS, 0xFFFFFFFF);
    
    // Enable FIS receive
    cmd = ahci_read_port_reg32(port, AHCI_PORT_CMD);
    cmd |= AHCI_PORT_CMD_FRE;
    ahci_write_port_reg32(port, AHCI_PORT_CMD, cmd);
    
    // Power up and spin up device
    cmd |= AHCI_PORT_CMD_POD | AHCI_PORT_CMD_SUD;
    ahci_write_port_reg32(port, AHCI_PORT_CMD, cmd);
    
    printf("AHCI: Port {d} initialized successfully{n}", port);
    return 0;
}

// Issue IDENTIFY command to get drive capacity
static int ahci_identify_drive(uint8_t port) {
    if (ahci_port_wait_ready(port) != 0) {
        printf("AHCI: Port {d} not ready for IDENTIFY{n}", port);
        return -1;
    }
    
    // Allocate buffer for identify data
    uint16_t *identify_buffer = kalloc(512);
    if (!identify_buffer) {
        printf("AHCI: Failed to allocate identify buffer{n}");
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
    
    // Setup PRDT
    cmd_table->prdt_entry[0].dba = (uint32_t)(uint64_t)identify_buffer;
    cmd_table->prdt_entry[0].dbau = (uint32_t)((uint64_t)identify_buffer >> 32);
    cmd_table->prdt_entry[0].dbc = 511; // 512 bytes - 1
    cmd_table->prdt_entry[0].i = 1; // Interrupt on completion
    
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
        printf("AHCI: IDENTIFY command timeout on port {d}{n}", port);
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
        printf("AHCI: Drive on port {d} reports 0 sectors{n}", port);
        kfree(identify_buffer);
        return -1;
    }
    
    ahci_ctrl.port_sectors[port] = sectors;
    uint32_t size_mb = (sectors * 512) / (1024 * 1024);
    
    printf("AHCI: Port {d} capacity: {d} sectors ({d} MB){n}", port, sectors, size_mb);
    
    kfree(identify_buffer);
    return 0;
}

void ahci_init() {
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
    
    // For now, just register a basic device without full port initialization
    // TODO: Implement full port initialization later
    DEBUG_SERIAL("AHCI: Basic controller detection successful\n");
    
    // Check if any ports are implemented
    if (pi != 0) {
        // Find first implemented port
        for (uint8_t port = 0; port < 32; port++) {
            if (pi & (1 << port)) {
                DEBUG_SERIAL("AHCI: Port %d is implemented\n", port);
                // For now, assume 1GB drive (2097152 sectors)
                ahci_ctrl.port_sectors[port] = 2097152;
                ahci_ctrl.initialized = true;
                break;
            }
        }
        
        if (ahci_ctrl.initialized) {
            DEBUG_SERIAL("AHCI: Registering basic SATA device\n");
            ahci_register_storage();
        }
    } else {
        DEBUG_SERIAL("AHCI: No ports implemented\n");
    }
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
    
    // Find first working port
    uint8_t working_port = 0xFF;
    for (uint8_t port = 0; port < 32; port++) {
        if ((ahci_ctrl.ports_implemented & (1 << port)) && ahci_ctrl.port_sectors[port] > 0) {
            working_port = port;
            break;
        }
    }
    
    if (working_port == 0xFF) {
        DEBUG_SERIAL("AHCI: No working port found\n");
        return -1;
    }
    
    ahci_storage_device_id = storage_device_count;
    storage_device_t *ahci_device = &storage_devices[ahci_storage_device_id];
    
    ahci_device->device_id = ahci_storage_device_id;
    ahci_device->sector_count = ahci_ctrl.port_sectors[working_port];
    ahci_device->sector_size = 512;
    ahci_device->is_initialized = true;
    ahci_device->pci_dev = ahci_ctrl.pci_dev;
    ahci_device->base_address = ahci_ctrl.bar5_base;
    
    // Simple string copy
    const char *name = "SATA Drive";
    for (int i = 0; i < 31 && name[i] != '\0'; i++) {
        ahci_device->name[i] = name[i];
    }
    ahci_device->name[31] = '\0';
    
    storage_device_count++;
    
    DEBUG_SERIAL("AHCI: Registered as storage device %d\n", ahci_storage_device_id);
    return 0;
}

// Simplified read/write functions (PIO mode for now)
int ahci_read_sectors(storage_device_t *dev, uint64_t lba, uint32_t sector_count, void *buffer) {
    printf("AHCI: Read not yet implemented{n}");
    return -1; // TODO: Implement DMA read
}

int ahci_write_sectors(storage_device_t *dev, uint64_t lba, uint32_t sector_count, const void *buffer) {
    printf("AHCI: Write not yet implemented{n}");
    return -1; // TODO: Implement DMA write
}

int ahci_flush(storage_device_t *dev) {
    return 0; // TODO: Implement cache flush
} 