#include "ahci.h"
#include "pci.h"
#include "storage.h"
#include "mem.h"
#include "utils.h"
#include "serial.h"

static ahci_controller_t ahci_ctrl = {0};
static uint8_t ahci_storage_device_id = 0xFF;

// Global AHCI buffers - guaranteed to be in mapped kernel memory
static uint8_t ahci_global_cmd_list[1024] __attribute__((aligned(1024)));
static uint8_t ahci_global_fis[256] __attribute__((aligned(256)));
static uint8_t ahci_global_cmd_table[128] __attribute__((aligned(128)));

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
    
    // Check if device is present (be lenient for QEMU)
    uint32_t ssts = ahci_read_port_reg32(port, AHCI_PORT_SSTS);
    uint32_t det = ssts & AHCI_PORT_SSTS_DET_MASK;
    printf("AHCI: Port {d} SSTS=0x{x}, DET=0x{x}{n}", port, ssts, det);
    
    // Be more lenient - accept any detection state except "no device"
    if (det == 0) {
        printf("AHCI: No device detected on port {d}{n}", port);
        return -1;
    }
    printf("AHCI: Device detected on port {d} (DET=0x{x}){n}", port, det);
    
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
    
    // Simplified port setup - just register a basic device for now
    if (pi != 0) {
        // Find first implemented port and assume it works
        for (uint8_t port = 0; port < 32; port++) {
            if (pi & (1 << port)) {
                DEBUG_SERIAL("AHCI: Port %d is implemented\n", port);
                
                // For now, assume a reasonable drive size and mark as initialized
                // We'll do lazy initialization when actually reading
                ahci_ctrl.port_sectors[port] = 2097152; // 1GB default
                ahci_ctrl.initialized = true;
                
                DEBUG_SERIAL("AHCI: Using port %d with default size\n", port);
                break;
            }
        }
        
        if (ahci_ctrl.initialized) {
            DEBUG_SERIAL("AHCI: Registering SATA device with lazy initialization\n");
            ahci_register_storage();
        } else {
            DEBUG_SERIAL("AHCI: No ports implemented\n");
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
    
    // Properly initialize the name field
    memset(ahci_device->name, 0, sizeof(ahci_device->name));
    const char *name = "SATA Drive";
    for (int i = 0; i < 31 && name[i] != '\0'; i++) {
        ahci_device->name[i] = name[i];
    }
    
    storage_device_count++;
    
    DEBUG_SERIAL("AHCI: Registered as storage device %d\n", ahci_storage_device_id);
    return 0;
}

// AHCI DMA read implementation
int ahci_read_sectors(storage_device_t *dev, uint64_t lba, uint32_t sector_count, void *buffer) {
    if (!ahci_ctrl.initialized || !dev || !buffer) {
        printf("AHCI: Invalid parameters for read{n}");
        return -1;
    }
    
    printf("AHCI: Reading {d} sectors from LBA {d}{n}", sector_count, lba);
    
    // Find the port for this device (for now, use port 0)
    uint8_t port = 0;
    
    // Find first implemented port
    for (uint8_t p = 0; p < 32; p++) {
        if (ahci_ctrl.ports_implemented & (1 << p)) {
            port = p;
            break;
        }
    }
    
    // Lazy initialization - set up port structures if not already done
    if (!ahci_ctrl.cmd_list[port]) {
        printf("AHCI: Lazy initializing port {d}{n}", port);
        if (ahci_init_port(port) != 0) {
            printf("AHCI: Port {d} initialization failed, trying minimal setup{n}", port);
            
            // Minimal setup - use kalloc'd memory which has proper HHDM mapping
            ahci_ctrl.cmd_list[port] = kalloc(1024);
            ahci_ctrl.fis_base[port] = kalloc(256);
            ahci_ctrl.cmd_table[port] = kalloc(128);
            
            if (!ahci_ctrl.cmd_list[port] || !ahci_ctrl.fis_base[port] || !ahci_ctrl.cmd_table[port]) {
                printf("AHCI: Failed to allocate minimal setup buffers{n}");
                return -1;
            }
            
            // Touch the memory to ensure pages are mapped and writable
            printf("AHCI: Testing buffer writability...{n}");
            memset(ahci_ctrl.cmd_list[port], 0, 1024);
            memset(ahci_ctrl.fis_base[port], 0, 256);
            memset(ahci_ctrl.cmd_table[port], 0, 128);
            printf("AHCI: Buffer initialization complete{n}");
            
            // Get virtual addresses
            uint64_t cmd_list_virt = (uint64_t)ahci_ctrl.cmd_list[port];
            uint64_t fis_virt = (uint64_t)ahci_ctrl.fis_base[port];
            uint64_t cmd_table_virt = (uint64_t)ahci_ctrl.cmd_table[port];
            
            // Convert to physical addresses using HHDM offset
            uint64_t cmd_list_phys = cmd_list_virt - vmm_higher_half_offset;
            uint64_t fis_phys = fis_virt - vmm_higher_half_offset;
            uint64_t cmd_table_phys = cmd_table_virt - vmm_higher_half_offset;
            
            printf("AHCI: Kalloc buffer setup (HHDM offset: 0x{x}):{n}", vmm_higher_half_offset);
            printf("  CmdList: V=0x{x}, P=0x{x} (aligned: {s}){n}", 
                   cmd_list_virt, cmd_list_phys, (cmd_list_phys & 1023) ? "NO" : "YES");
            printf("  FIS: V=0x{x}, P=0x{x} (aligned: {s}){n}", 
                   fis_virt, fis_phys, (fis_phys & 255) ? "NO" : "YES");
            
            // Additional serial debug with proper format
            serial_printf("AHCI: HHDM high=0x%x low=0x%x, CmdList P=0x%x, FIS P=0x%x\n", 
                         (uint32_t)(vmm_higher_half_offset >> 32), (uint32_t)vmm_higher_half_offset,
                         (uint32_t)cmd_list_phys, (uint32_t)fis_phys);
            
            // Setup port registers with PHYSICAL addresses
            ahci_write_port_reg32(port, AHCI_PORT_CLB, (uint32_t)cmd_list_phys);
            ahci_write_port_reg32(port, AHCI_PORT_CLBU, (uint32_t)(cmd_list_phys >> 32));
            ahci_write_port_reg32(port, AHCI_PORT_FB, (uint32_t)fis_phys);
            ahci_write_port_reg32(port, AHCI_PORT_FBU, (uint32_t)(fis_phys >> 32));
            
            // Setup basic command header
            ahci_cmd_header_t *cmd_header = (ahci_cmd_header_t*)ahci_ctrl.cmd_list[port];
            cmd_header->ctba = (uint32_t)cmd_table_phys;
            cmd_header->ctbau = (uint32_t)(cmd_table_phys >> 32);
            
            printf("  CmdTable: V=0x{x}, P=0x{x} (aligned: {s}){n}", 
                   cmd_table_virt, cmd_table_phys, (cmd_table_phys & 127) ? "NO" : "YES");
            
            // Enable FIS receive only (don't start command engine yet)
            uint32_t cmd_reg = ahci_read_port_reg32(port, AHCI_PORT_CMD);
            printf("  Initial CMD reg: 0x%x{n}", cmd_reg);
            
            cmd_reg |= AHCI_PORT_CMD_FRE;  // Only enable FIS receive
            ahci_write_port_reg32(port, AHCI_PORT_CMD, cmd_reg);
            
            // Wait for FIS receive to be running
            int timeout = 1000;
            while (timeout-- && !(ahci_read_port_reg32(port, AHCI_PORT_CMD) & AHCI_PORT_CMD_FR)) {
                // Wait for FR bit
            }
            
            uint32_t cmd_after = ahci_read_port_reg32(port, AHCI_PORT_CMD);
            printf("  After FRE CMD reg: 0x%x (FR running: {s}){n}", 
                   cmd_after, (cmd_after & AHCI_PORT_CMD_FR) ? "YES" : "NO");
            
            printf("AHCI: Minimal setup complete for port {d}{n}", port);
        } else {
            // Try to identify the drive for real capacity
            if (ahci_identify_drive(port) == 0) {
                printf("AHCI: Port {d} drive identified successfully{n}", port);
            } else {
                printf("AHCI: Using default drive size for port {d}{n}", port);
            }
        }
    }
    
    // Verify port is ready for commands
    uint32_t cmd = ahci_read_port_reg32(port, AHCI_PORT_CMD);
    printf("AHCI: Port {d} CMD register: 0x{x}{n}", port, cmd);
    if (!(cmd & AHCI_PORT_CMD_ST)) {
        printf("AHCI: Starting command engine for port {d}{n}", port);
        cmd |= AHCI_PORT_CMD_ST;
        ahci_write_port_reg32(port, AHCI_PORT_CMD, cmd);
    }
    
    if (ahci_port_wait_ready(port) != 0) {
        printf("AHCI: Port {d} not ready for read{n}", port);
        return -1;
    }
    
    // Check bounds
    if (lba + sector_count > ahci_ctrl.port_sectors[port]) {
        printf("AHCI: Read beyond drive capacity{n}");
        return -1;
    }
    
    // Setup command table
    ahci_cmd_table_t *cmd_table = (ahci_cmd_table_t*)ahci_ctrl.cmd_table[port];
    memset(cmd_table, 0, sizeof(ahci_cmd_table_t));
    
    // Setup READ DMA EXT FIS
    fis_reg_h2d_t *fis = (fis_reg_h2d_t*)cmd_table->cfis;
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->c = 1; // Command
    fis->command = ATA_CMD_READ_DMA_EXT;
    
    // Set LBA (48-bit)
    fis->lba0 = (uint8_t)(lba & 0xFF);
    fis->lba1 = (uint8_t)((lba >> 8) & 0xFF);
    fis->lba2 = (uint8_t)((lba >> 16) & 0xFF);
    fis->lba3 = (uint8_t)((lba >> 24) & 0xFF);
    fis->lba4 = (uint8_t)((lba >> 32) & 0xFF);
    fis->lba5 = (uint8_t)((lba >> 40) & 0xFF);
    
    // Set device register (LBA mode)
    fis->device = 0x40; // LBA mode
    
    // Set sector count (16-bit)
    fis->countl = (uint8_t)(sector_count & 0xFF);
    fis->counth = (uint8_t)((sector_count >> 8) & 0xFF);
    
    printf("AHCI: FIS setup - LBA:{d}, count:{d}, cmd:0x{x}{n}", lba, sector_count, ATA_CMD_READ_DMA_EXT);
    
    // Setup PRDT (Physical Region Descriptor Table)
    uint32_t bytes_to_read = sector_count * 512;
    uint64_t buffer_virt = (uint64_t)buffer;
    uint64_t buffer_phys = buffer_virt - vmm_higher_half_offset;
    
    printf("AHCI: Setting up PRDT - buffer V:0x{x}, P:0x{x}, bytes:{d}{n}", 
           buffer_virt, buffer_phys, bytes_to_read);
    
    // Serial debug with proper format
    serial_printf("AHCI: PRDT buffer P=0x%x, bytes=%d\n", (uint32_t)buffer_phys, bytes_to_read);
    
    cmd_table->prdt_entry[0].dba = (uint32_t)buffer_phys;
    cmd_table->prdt_entry[0].dbau = (uint32_t)(buffer_phys >> 32);
    cmd_table->prdt_entry[0].dbc = bytes_to_read - 1; // Byte count minus 1
    cmd_table->prdt_entry[0].i = 1; // Interrupt on completion
    
    // Setup command header
    ahci_cmd_header_t *cmd_header = (ahci_cmd_header_t*)ahci_ctrl.cmd_list[port];
    cmd_header->cfl = sizeof(fis_reg_h2d_t) / 4; // FIS length in DWORDS
    cmd_header->w = 0; // Read (not write)
    cmd_header->prdtl = 1; // One PRDT entry
    cmd_header->prdbc = 0; // Will be filled by hardware
    
    // Clear any pending interrupts
    ahci_write_port_reg32(port, AHCI_PORT_IS, 0xFFFFFFFF);
    
    // Show final register state before issuing command
    uint32_t tfd_before = ahci_read_port_reg32(port, AHCI_PORT_TFD);
    uint32_t cmd_before = ahci_read_port_reg32(port, AHCI_PORT_CMD);
    uint32_t is_before = ahci_read_port_reg32(port, AHCI_PORT_IS);
    printf("AHCI: Before command - TFD:0x%x, CMD:0x%x, IS:0x%x{n}", 
           tfd_before, cmd_before, is_before);
    
    // Issue command
    printf("AHCI: Issuing command to slot 0{n}");
    ahci_write_port_reg32(port, AHCI_PORT_CI, 1); // Issue command slot 0
    
    // Wait for command completion
    int timeout = 10000;
    printf("AHCI: Waiting for command completion (CI=0x{x}){n}", ahci_read_port_reg32(port, AHCI_PORT_CI));
    
    while (timeout--) {
        uint32_t ci = ahci_read_port_reg32(port, AHCI_PORT_CI);
        if (!(ci & 1)) { // Command completed
            printf("AHCI: Command completed successfully{n}");
            break;
        }
        
        // Check for errors
        uint32_t is = ahci_read_port_reg32(port, AHCI_PORT_IS);
        if (is & 0x7D800000) { // Error bits
            printf("AHCI: Read command error - IS: 0x{x}, TFD: 0x{x}{n}", 
                   is, ahci_read_port_reg32(port, AHCI_PORT_TFD));
            return -1;
        }
        
        // Debug output every 2000 iterations to reduce spam
        if (timeout % 2000 == 0) {
            printf("AHCI: Still waiting... CI=0x{x}, IS=0x{x}, timeout={d}{n}", 
                   ci, is, timeout);
        }
    }
    
    if (timeout <= 0) {
        uint32_t ci = ahci_read_port_reg32(port, AHCI_PORT_CI);
        uint32_t is = ahci_read_port_reg32(port, AHCI_PORT_IS);
        uint32_t tfd = ahci_read_port_reg32(port, AHCI_PORT_TFD);
        uint32_t cmd_reg = ahci_read_port_reg32(port, AHCI_PORT_CMD);
        printf("AHCI: Read command timeout - CI=0x{x}, IS=0x{x}, TFD=0x{x}, CMD=0x{x}{n}", 
               ci, is, tfd, cmd_reg);
        
        // Serial debug with proper format
        serial_printf("AHCI: TIMEOUT - CI=0x%x, IS=0x%x, TFD=0x%x, CMD=0x%x\n", 
                     ci, is, tfd, cmd_reg);
        return -1;
    }
    
    // Check if transfer completed successfully
    uint32_t is = ahci_read_port_reg32(port, AHCI_PORT_IS);
    if (is & 0x7D800000) { // Error bits
        printf("AHCI: Read completed with errors - IS: 0x{x}{n}", is);
        return -1;
    }
    
    return 0;
}

int ahci_write_sectors(storage_device_t *dev, uint64_t lba, uint32_t sector_count, const void *buffer) {
    printf("AHCI: Write not yet implemented{n}");
    return -1; // TODO: Implement DMA write
}

int ahci_flush(storage_device_t *dev) {
    return 0; // TODO: Implement cache flush
} 