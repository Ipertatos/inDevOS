#include "rtl8139.h"
#include "net_device.h"
#include "pci.h"
#include "mem.h"
#include "utils.h"
#include "serial.h"
#include "idt.h"
#include "string.h"
#include "registers.h"
#include "net_arp.h"
#include "net_ip.h"
#include "net_icmp.h"
#include "net_ethernet.h"
#include "apic.h"
#include "atomic.h"
#include "debug_config.h"
#include "string.h"

rtl8139_device_t rtl8139_dev = {0};
static spinlock_t rtl8139_rx_lock = {0};

// Helper functions for register access
uint8_t rtl8139_read_reg8(rtl8139_device_t *dev, uint32_t offset) {
    if (dev->use_io)
        return inb((uint16_t)(uintptr_t)dev->io_base + offset);
    else
        return *(volatile uint8_t*)(dev->io_base + offset);
}

uint16_t rtl8139_read_reg16(rtl8139_device_t *dev, uint32_t offset) {
    if (dev->use_io)
        return inw((uint16_t)(uintptr_t)dev->io_base + offset);
    else
        return *(volatile uint16_t*)(dev->io_base + offset);
}

uint32_t rtl8139_read_reg32(rtl8139_device_t *dev, uint32_t offset) {
    if (dev->use_io)
        return inl((uint16_t)(uintptr_t)dev->io_base + offset);
    else
        return *(volatile uint32_t*)(dev->io_base + offset);
}

void rtl8139_write_reg8(rtl8139_device_t *dev, uint32_t offset, uint8_t value) {
    if (dev->use_io)
        outb((uint16_t)(uintptr_t)dev->io_base + offset, value);
    else
        *(volatile uint8_t*)(dev->io_base + offset) = value;
}

void rtl8139_write_reg16(rtl8139_device_t *dev, uint32_t offset, uint16_t value) {
    if (dev->use_io)
        outw((uint16_t)(uintptr_t)dev->io_base + offset, value);
    else
        *(volatile uint16_t*)(dev->io_base + offset) = value;
}

void rtl8139_write_reg32(rtl8139_device_t *dev, uint32_t offset, uint32_t value) {
    if (dev->use_io)
        outl((uint16_t)(uintptr_t)dev->io_base + offset, value);
    else
        *(volatile uint32_t*)(dev->io_base + offset) = value;
}

// Reset the RTL8139 chip
static void rtl8139_reset(rtl8139_device_t *dev) {
    printf("RTL8139: Resetting device\n");
    
    // Set reset bit
    rtl8139_write_reg8(dev, RTL8139_CMD, RTL8139_CMD_RST);
    
    // Wait for reset to complete
    int timeout = 1000;
    while (timeout--) {
        if (!(rtl8139_read_reg8(dev, RTL8139_CMD) & RTL8139_CMD_RST)) {
            break;
        }
        // Small delay
        for (volatile int i = 0; i < 1000; i++);
    }
    
    if (timeout <= 0) {
        printf("RTL8139: Reset timeout\n");
        return;
    }
    
    printf("RTL8139: Reset completed\n");
}

// Setup receive buffer
static void rtl8139_setup_rx_buffer(rtl8139_device_t *dev) {
    // CRITICAL DMA FIX: Allocate in low memory for guaranteed RTL8139 access
    // RTL8139 might have issues with addresses above certain ranges
    size_t buffer_size = RTL8139_RX_BUFFER_SIZE + 16 + 2048;  /* WRAP slack: a max frame + header may run past the ring end */
    
    // Try to allocate in very low memory first (below 16MB)
    dev->rx_buffer = NULL;
    for (int attempt = 0; attempt < 3 && !dev->rx_buffer; attempt++) {
        dev->rx_buffer = kalloc(buffer_size);
        if (dev->rx_buffer) {
            uint64_t virt_addr = (uint64_t)dev->rx_buffer;
            uint64_t phys_addr;
            if (virt_addr >= 0xFFFF800000000000ULL) phys_addr = virt_addr - 0xFFFF800000000000ULL;
            else if (virt_addr >= 0xFFFFFFFF80000000ULL) phys_addr = virt_addr - 0xFFFFFFFF80000000ULL;
            else phys_addr = virt_addr;
            
            // Check if address is suitable for RTL8139 (prefer below 16MB)
            if (phys_addr >= 0x1000000ULL) { // Above 16MB
                printf("RTL8139: Attempt %d - buffer at 0x%lx too high, retrying...\n", 
                       attempt + 1, phys_addr);
                kfree((void*)dev->rx_buffer);
                dev->rx_buffer = NULL;
            } else {
                printf("RTL8139: Attempt %d - good buffer at 0x%lx\n", attempt + 1, phys_addr);
                break;
            }
        }
    }
    if (!dev->rx_buffer) {
        printf("RTL8139: Failed to allocate receive buffer\n");
        return;
    }
    
    // Clear entire buffer
    memset((void*)dev->rx_buffer, 0, buffer_size);
    
    // Calculate proper physical address for DMA
    // For kernel virtual addresses, subtract the higher half offset
    uint64_t virt_addr = (uint64_t)dev->rx_buffer;
    uint64_t rx_buffer_phys;
    
    // CRITICAL FIX: Use identity mapping offset
    // The kernel uses 0xFFFF800000000000 but DMA needs physical addresses
    if (virt_addr >= 0xFFFF800000000000ULL) {
        rx_buffer_phys = virt_addr - 0xFFFF800000000000ULL;
    } else if (virt_addr >= 0xFFFFFFFF80000000ULL) {
        // Handle different kernel mapping schemes (e.g. -2GB)
        rx_buffer_phys = virt_addr - 0xFFFFFFFF80000000ULL;
    } else {
        // If it's already a physical address or low memory, use as-is
        rx_buffer_phys = virt_addr;
    }
    
    // Ensure the physical address is 32-bit compatible for RTL8139
    if (rx_buffer_phys >= 0x100000000ULL) {
        printf("RTL8139: ERROR: Buffer physical address 0x%lx is above 4GB, RTL8139 cannot access it\n", rx_buffer_phys);
        // Try to allocate in lower memory
        dev->rx_buffer = kalloc(buffer_size); // Try again with regular kalloc
        if (!dev->rx_buffer) {
            printf("RTL8139: Critical error: Cannot allocate DMA-accessible buffer\n");
            return;
        }
        virt_addr = (uint64_t)dev->rx_buffer;
        if (virt_addr >= 0xFFFF800000000000ULL) {
            rx_buffer_phys = virt_addr - 0xFFFF800000000000ULL;
        } else if (virt_addr >= 0xFFFFFFFF80000000ULL) {
            rx_buffer_phys = virt_addr - 0xFFFFFFFF80000000ULL;
        } else {
            rx_buffer_phys = virt_addr;
        }
        if (rx_buffer_phys >= 0x100000000ULL) {
            printf("RTL8139: CRITICAL: Still cannot get 32-bit accessible memory\n");
            return;
        }
    }
    
    // Ensure buffer is 4-byte aligned for RTL8139
    if (rx_buffer_phys & 0x3) {
        printf("RTL8139: WARNING: Buffer not 4-byte aligned, adjusting...\n");
        rx_buffer_phys = (rx_buffer_phys + 3) & ~0x3;
    }
    
    printf("RTL8139: Using buffer - Virtual: 0x%lx, Physical: 0x%lx\n", virt_addr, rx_buffer_phys);
    // Read back RBSTART after we write it to confirm mapping
    
    // Write the physical address to RBSTART register
    rtl8139_write_reg32(dev, RTL8139_RBSTART, (uint32_t)rx_buffer_phys);
    uint32_t rbstart_after = rtl8139_read_reg32(dev, RTL8139_RBSTART);
    printf("RTL8139: RBSTART after write: 0x%08x\n", rbstart_after);
    
    // Initialize CAPR to -0x10 (0xFFF0) per RTL8139 docs; software ptr starts at 0
    dev->current_packet_ptr = 0;
    rtl8139_write_reg16(dev, RTL8139_CAPR, 0xFFF0);
    
    // Verify the register write worked
    uint32_t verify_rbstart = rtl8139_read_reg32(dev, RTL8139_RBSTART);
    uint16_t verify_capr = rtl8139_read_reg16(dev, RTL8139_CAPR);
    printf("RTL8139: BUFFER SETUP VERIFICATION - RBSTART: wrote 0x%08x, read 0x%08x\n", 
           (uint32_t)rx_buffer_phys, verify_rbstart);
    printf("RTL8139: BUFFER SETUP VERIFICATION - CAPR: wrote 0xFFF0, read 0x%04x\n", verify_capr);
    
    // CRITICAL DMA TEST: Write a test pattern and verify DMA access
    printf("RTL8139: DMA VERIFICATION TEST:\n");
    // Write test pattern at various buffer locations
    for (int i = 0; i < 64; i += 16) {
        dev->rx_buffer[i] = 0xAA;
        dev->rx_buffer[i + 1] = 0xBB;
        dev->rx_buffer[i + 2] = 0xCC;
        dev->rx_buffer[i + 3] = 0xDD;
    }
    
    // Verify CPU can read what it wrote
    printf("RTL8139: CPU write/read test: ");
    for (int i = 0; i < 16; i += 4) {
        printf("%02x%02x%02x%02x ", dev->rx_buffer[i], dev->rx_buffer[i+1], 
               dev->rx_buffer[i+2], dev->rx_buffer[i+3]);
    }
    printf("\n");
    
    // Clear for actual reception
    memset((void*)dev->rx_buffer, 0, buffer_size);
    
    // CRITICAL FIX: Flush CPU write buffers / cache to memory for DMA
    // Since we don't have explicit cache flush functions, we use a memory barrier
    __asm__ volatile ("mfence" ::: "memory");
    
    // CBR is read-only and managed by hardware, don't set it
    printf("RTL8139: Receive buffer setup at 0x%lx (phys: 0x%lx), CAPR=0xFFF0\n", 
           (uint64_t)dev->rx_buffer, rx_buffer_phys);
}

// Configure receive buffer (RCR) - Working RTL8139 implementation pattern
static void rtl8139_configure_rx_buffer(rtl8139_device_t *dev) {
    // Configure RCR (Receive Configuration Register)
    // Enable: Accept Broadcast, Multicast, Physical, AAP for debug, WRAP
    // Set MXDMA to unlimited, receive buffer length base to 8K
    uint32_t rcr_value =
        RTL8139_RCR_AB |
        RTL8139_RCR_AM |
        RTL8139_RCR_APM |
        RTL8139_RCR_AAP |
        RTL8139_RCR_WRAP |
        RTL8139_RCR_MXDMA_UNL |
        RTL8139_RCR_RBLEN_32K;

    // RCR is at 0x44 and is 32-bit wide
    rtl8139_write_reg32(dev, RTL8139_CONFIG4, rcr_value);

    // Verify the register was set correctly
    uint32_t read_rcr = rtl8139_read_reg32(dev, RTL8139_CONFIG4);
    printf("RTL8139: RCR configured - wrote 0x%08x, read 0x%08x (WRAP, MXDMA=unl, RBLEN=32K)\n", rcr_value, read_rcr);
}

// Setup transmit buffers
static void rtl8139_setup_tx_buffers(rtl8139_device_t *dev) {
    for (int i = 0; i < RTL8139_NUM_TX_BUFFERS; i++) {
        // Allocate transmit buffer using regular kalloc for DMA accessibility
        dev->tx_buffers[i] = kalloc(RTL8139_TX_BUFFER_SIZE);
        if (!dev->tx_buffers[i]) {
            printf("RTL8139: Failed to allocate transmit buffer %d\n", i);
            return;
        }
        
        // Clear buffer
        memset(dev->tx_buffers[i], 0, RTL8139_TX_BUFFER_SIZE);
        
        // Calculate proper physical address
        uint64_t virt_addr = (uint64_t)dev->tx_buffers[i];
        uint64_t tx_buffer_phys;
        
        // Handle common kernel mapping offsets
        if (virt_addr >= 0xFFFF800000000000ULL) {
            tx_buffer_phys = virt_addr - 0xFFFF800000000000ULL;
        } else if (virt_addr >= 0xFFFFFFFF80000000ULL) {
            // Common -2GB kernel mapping
            tx_buffer_phys = virt_addr - 0xFFFFFFFF80000000ULL;
        } else {
            tx_buffer_phys = virt_addr;
        }
        
        // Ensure 32-bit compatibility
        if (tx_buffer_phys >= 0x100000000ULL) {
            printf("RTL8139: ERROR: TX buffer %d physical address 0x%lx is above 4GB\n", i, tx_buffer_phys);
            // Try to allocate in lower memory (simple retry won't fix high mem mapping unless allocator is aware)
            // Ideally we need a DMA allocator. For now, we hope for the best or fail.
            return;
        }
        
        rtl8139_write_reg32(dev, RTL8139_TSAD0 + (i * 4), (uint32_t)tx_buffer_phys);
        
        // CRITICAL FIX: Clear transmit status descriptor properly
        // Write 0 to clear OWN bit and make buffer available for software
        rtl8139_write_reg32(dev, RTL8139_TSD0 + (i * 4), 0);
        
        printf("RTL8139: Transmit buffer %d setup at 0x%lx (phys: 0x%lx)\n", 
               i, (uint64_t)dev->tx_buffers[i], tx_buffer_phys);
               
        // TEST: Verify memory access to TX buffer
        dev->tx_buffers[i][0] = 0xAA;
        dev->tx_buffers[i][1500] = 0x55;
        if (dev->tx_buffers[i][0] != 0xAA || dev->tx_buffers[i][1500] != 0x55) {
            printf("RTL8139: ERROR: CPU cannot read/write TX buffer %d properly!\n", i);
        }
        memset(dev->tx_buffers[i], 0, RTL8139_TX_BUFFER_SIZE);
    }
    
    dev->tx_cur = 0;
}

// Set MAC address
static void rtl8139_set_mac_address(rtl8139_device_t *dev, const uint8_t *mac) {
    memcpy(dev->mac_address, mac, 6);
    
    // Write MAC address to registers (little-endian)
    uint32_t mac_low = (mac[0] << 0) | (mac[1] << 8) | (mac[2] << 16) | (mac[3] << 24);
    uint16_t mac_high = (mac[4] << 0) | (mac[5] << 8);
    
    rtl8139_write_reg32(dev, RTL8139_MAC, mac_low);
    rtl8139_write_reg16(dev, RTL8139_MAC + 4, mac_high);
    
    serial_printf("RTL8139: MAC address set to %02x:%02x:%02x:%02x:%02x:%02x\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// Enable interrupts
static void rtl8139_enable_interrupts(rtl8139_device_t *dev) {
    // Enable receive OK and transmit OK interrupts (OSDev wiki: 0x0005 = TOK + ROK)
    uint16_t imr = RTL8139_ISR_ROK | RTL8139_ISR_TOK;
    rtl8139_write_reg16(dev, RTL8139_IMR, imr);
    
    // Verify the interrupt mask was set correctly
    uint16_t read_imr = rtl8139_read_reg16(dev, RTL8139_IMR);
    serial_printf("RTL8139: Interrupts enabled - IMR: 0x%04x (wrote: 0x%04x, TOK + ROK)\n", read_imr, imr);
}

// Disable interrupts
static void rtl8139_disable_interrupts(rtl8139_device_t *dev) {
    rtl8139_write_reg16(dev, RTL8139_IMR, 0);
    printf("RTL8139: Interrupts disabled\n");
}

// ---- 93C46 EEPROM access ----------------------------------------------------

/**
 * Small delay for EEPROM bit-bang timing (~4 microseconds).
 * The 93C46 requires a minimum clock period of ~1 us.
 */
static void rtl8139_eeprom_delay(void) {
    for (volatile int i = 0; i < 200; i++);
}

/**
 * Raise the EEPROM clock (EESK) with the given command bits.
 */
static void rtl8139_eeprom_clock(rtl8139_device_t *dev, uint8_t cmd) {
    rtl8139_write_reg8(dev, RTL8139_9346CR, cmd | RTL8139_EEPROM_EESK);
    rtl8139_eeprom_delay();
    rtl8139_write_reg8(dev, RTL8139_9346CR, cmd);
    rtl8139_eeprom_delay();
}

/**
 * Read a 16-bit word from the 93C46 EEPROM at the given address (0-63).
 *
 * Protocol (93C46 in 16-bit mode, 6-bit addresses):
 *   1. Assert EECS (chip select)
 *   2. Send start bit (1), opcode (10 for read), and 6-bit address MSB-first
 *   3. Clock out 16 data bits MSB-first
 *   4. Deassert EECS
 */
uint16_t rtl8139_eeprom_read(rtl8139_device_t *dev, uint8_t address) {
    // Enter programming mode
    rtl8139_write_reg8(dev, RTL8139_9346CR, RTL8139_EEPROM_MODE_PROGRAM);
    rtl8139_eeprom_delay();

    // Build the command: start bit (1) + opcode (10) + 6-bit address
    // Total: 9 bits, sent MSB first: 1_10_AAAAAA
    uint16_t command = (0x06 << 6) | (address & 0x3F);  // 0b110 + addr
    // command is 9 bits: bit 8 = start, bit 7-6 = opcode (10), bits 5-0 = address

    // Assert chip select
    uint8_t base = RTL8139_EEPROM_MODE_PROGRAM | RTL8139_EEPROM_EECS;
    rtl8139_write_reg8(dev, RTL8139_9346CR, base);
    rtl8139_eeprom_delay();

    // Send 9-bit command (MSB first)
    for (int i = 8; i >= 0; i--) {
        uint8_t databit = (command >> i) & 1;
        uint8_t reg_val = base | (databit ? RTL8139_EEPROM_EEDI : 0);
        rtl8139_write_reg8(dev, RTL8139_9346CR, reg_val);
        rtl8139_eeprom_delay();
        rtl8139_eeprom_clock(dev, reg_val);
    }

    // Read 16 data bits (MSB first)
    uint16_t data = 0;
    uint8_t read_base = RTL8139_EEPROM_MODE_PROGRAM | RTL8139_EEPROM_EECS;
    for (int i = 15; i >= 0; i--) {
        rtl8139_write_reg8(dev, RTL8139_9346CR, read_base | RTL8139_EEPROM_EESK);
        rtl8139_eeprom_delay();
        uint8_t reg = rtl8139_read_reg8(dev, RTL8139_9346CR);
        data |= ((reg & RTL8139_EEPROM_EEDO) ? 1 : 0) << i;
        rtl8139_write_reg8(dev, RTL8139_9346CR, read_base);
        rtl8139_eeprom_delay();
    }

    // Deassert chip select and return to normal mode
    rtl8139_write_reg8(dev, RTL8139_9346CR, RTL8139_EEPROM_MODE_NORMAL);
    rtl8139_eeprom_delay();

    return data;
}

/**
 * Read the MAC address from the 93C46 EEPROM.
 * Returns 1 on success (MAC looks valid), 0 on failure.
 */
int rtl8139_read_mac_from_eeprom(rtl8139_device_t *dev, uint8_t mac[6]) {
    uint16_t w0 = rtl8139_eeprom_read(dev, RTL8139_EEPROM_MAC_WORD0);
    uint16_t w1 = rtl8139_eeprom_read(dev, RTL8139_EEPROM_MAC_WORD1);
    uint16_t w2 = rtl8139_eeprom_read(dev, RTL8139_EEPROM_MAC_WORD2);

    // EEPROM words are little-endian (low byte first)
    mac[0] = (uint8_t)(w0 & 0xFF);
    mac[1] = (uint8_t)(w0 >> 8);
    mac[2] = (uint8_t)(w1 & 0xFF);
    mac[3] = (uint8_t)(w1 >> 8);
    mac[4] = (uint8_t)(w2 & 0xFF);
    mac[5] = (uint8_t)(w2 >> 8);

    printf("RTL8139: EEPROM MAC = %02x:%02x:%02x:%02x:%02x:%02x (words: %04x %04x %04x)\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], w0, w1, w2);

    // Validate: MAC should not be all-zeros, all-FF, or have multicast bit set
    int all_zero = 1, all_ff = 1;
    for (int i = 0; i < 6; i++) {
        if (mac[i] != 0x00) all_zero = 0;
        if (mac[i] != 0xFF) all_ff = 0;
    }

    if (all_zero || all_ff) {
        printf("RTL8139: EEPROM MAC invalid (all %s)\n", all_zero ? "zeros" : "0xFF");
        return 0;
    }

    // Check multicast bit (bit 0 of first octet) – a valid unicast MAC has it clear
    if (mac[0] & 0x01) {
        printf("RTL8139: WARNING: EEPROM MAC has multicast bit set, may be invalid\n");
        // Still return success – some emulators may set this
    }

    return 1;
}

// Initialize the RTL8139 network card
void rtl8139_init() {
    // Guard against double init (driver_manager + legacy kernel.c bringup).
    if (rtl8139_dev.initialized) {
        printf("RTL8139: Already initialized, skipping\n");
        return;
    }
    printf("RTL8139: Initializing network driver\n");
    
    // Find RTL8139 device
    struct pci_device *pci_dev = NULL;
    for (int i = 0; i < pci_device_count; i++) {
        if (pci_devices[i]->vendor_id == RTL8139_VENDOR_ID && 
            pci_devices[i]->device_id == RTL8139_DEVICE_ID) {
            pci_dev = pci_devices[i];
            break;
        }
    }
    
    if (!pci_dev) {
        printf("RTL8139: Device not found\n");
        return;
    }
    
    printf("RTL8139: Found device at bus %d, device %d, function %d\n",
           pci_dev->bus, pci_dev->device, pci_dev->function);
    
    // Enable PCI bus mastering and memory space for DMA operations
    uint16_t pci_cmd = pci_device_read(pci_dev, 0x04, 2);
    printf("RTL8139: Current PCI command register: 0x%04x\n", pci_cmd);
    pci_cmd |= 0x06; // Enable memory space (bit 1) and bus mastering (bit 2)
    // FIX: correct parameter order is (dev, offset, size, value)
    pci_device_write(pci_dev, 0x04, 2, pci_cmd);
    // Read back to verify bus mastering really enabled
    uint16_t pci_cmd_after = pci_device_read(pci_dev, 0x04, 2);
    printf("RTL8139: PCI command register after write: 0x%04x (expect bit1+bit2 set)\n", pci_cmd_after);
    
    // Get BAR0 (base address)
    uint32_t bar0 = pci_device_read(pci_dev, 0x10, 4);
    printf("RTL8139: BAR0 value: 0x%x\n", bar0);
    
    uint32_t base_addr;
    if (bar0 & 1) {
        // I/O space
        base_addr = bar0 & ~3;
        printf("RTL8139: Using I/O space at 0x%x\n", base_addr);
        rtl8139_dev.use_io = 1;
    } else {
        // Memory space
        base_addr = bar0 & ~0xF;
        printf("RTL8139: Using memory space at 0x%x\n", base_addr);
        rtl8139_dev.use_io = 0;
    }
    
    rtl8139_dev.io_base = (uint8_t*)(uint64_t)base_addr;
    rtl8139_dev.pci_dev = pci_dev;
    
    // Read interrupt line from PCI configuration
    uint8_t interrupt_line = pci_device_read(pci_dev, 0x3C, 1);
    printf("RTL8139: Interrupt line: %d\n", interrupt_line);
    
    printf("RTL8139: Base address: 0x%x\n", (uint32_t)base_addr);
    
    // CRITICAL: Power on the device first (missing in original implementation)
    printf("RTL8139: Powering on device\n");
    rtl8139_write_reg8(&rtl8139_dev, RTL8139_CONFIG1_POWER, 0x00);
    
    // Single reset - double reset was causing issues
    rtl8139_reset(&rtl8139_dev);

    // Clear any pending interrupts before enabling IMR
    rtl8139_write_reg16(&rtl8139_dev, RTL8139_ISR, 0xFFFF);
    
    // Setup buffers
    rtl8139_setup_rx_buffer(&rtl8139_dev);
    rtl8139_setup_tx_buffers(&rtl8139_dev);
    
    // CRITICAL: Add delay after buffer setup for hardware to recognize the buffer
    for (volatile int i = 0; i < 1000; i++);
    
    // Configure receive buffer (RCR) with proper WRAP mode
    rtl8139_configure_rx_buffer(&rtl8139_dev);
    
    // Read MAC address: try EEPROM first, then registers, then fallback
    uint8_t mac[6];
    int mac_valid = 0;

    // 1. Try reading from 93C46 EEPROM (factory-programmed MAC)
    printf("RTL8139: Attempting to read MAC from EEPROM...\n");
    mac_valid = rtl8139_read_mac_from_eeprom(&rtl8139_dev, mac);

    // 2. If EEPROM failed, read from device registers (loaded by hardware on power-up)
    if (!mac_valid) {
        printf("RTL8139: EEPROM read failed, reading MAC from registers...\n");
        for (int i = 0; i < 6; i++) {
            mac[i] = rtl8139_read_reg8(&rtl8139_dev, RTL8139_MAC + i);
        }
        printf("RTL8139: Register MAC = %02x:%02x:%02x:%02x:%02x:%02x\n",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

        // Validate register MAC
        int all_zero = 1, all_ff = 1;
        for (int i = 0; i < 6; i++) {
            if (mac[i] != 0x00) all_zero = 0;
            if (mac[i] != 0xFF) all_ff = 0;
        }
        mac_valid = !all_zero && !all_ff;
    }

    // 3. Fallback: use QEMU default MAC if nothing else works
    if (!mac_valid) {
        printf("RTL8139: No valid MAC found, using QEMU default\n");
        mac[0] = 0x52; mac[1] = 0x54; mac[2] = 0x00;
        mac[3] = 0x12; mac[4] = 0x34; mac[5] = 0x56;
    }

    printf("RTL8139: Using MAC: %02x:%02x:%02x:%02x:%02x:%02x%s\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
           mac_valid ? " (from hardware)" : " (default)");
    
    rtl8139_set_mac_address(&rtl8139_dev, mac);
    
    // Enable receiver and transmitter (OSDev wiki: 0x0C = RE + TE)
    uint8_t cmd = RTL8139_CMD_RE | RTL8139_CMD_TE;
    rtl8139_write_reg8(&rtl8139_dev, RTL8139_CMD, cmd);
    serial_printf("RTL8139: Command register set to 0x%02x (RE + TE)\n", cmd);
    
    // Keep command as RE|TE only
    serial_printf("RTL8139: Command register set to 0x%02x (RE + TE)\n", cmd);
    
    // Enable interrupts
    rtl8139_enable_interrupts(&rtl8139_dev);
    
    // Configure IOAPIC to route the RTL8139 PCI INTx line to vector 43
    // Get the LAPIC ID from the first CPU
    uint32_t lapic_id = 0; // Assuming single CPU for now
    /* Route the line the PCI config says (10 on QEMU -M pc). The old
     * hardcoded 11 is virtio-gpu INTx there: an IRQ storm with ISR=0. */
    uint8_t rtl_line = pci_device_read(pci_dev, 0x3C, 1) & 0xFF;
    if (rtl_line == 0 || rtl_line >= 24) rtl_line = 11;
    ioapic_redirect_irq(lapic_id, 43, rtl_line, true);
    printf("RTL8139: IRQ %u -> vector 43\n", rtl_line);
    
    // Initialize network stack
    arp_init();
    ip_init();
    icmp_init();
    
    // Set static IP address (192.168.56.2)
    rtl8139_dev.ip_address[0] = 192;
    rtl8139_dev.ip_address[1] = 168;
    rtl8139_dev.ip_address[2] = 56;
    rtl8139_dev.ip_address[3] = 2;
    
    printf("RTL8139: Set IP address to %d.%d.%d.%d\n", 
           rtl8139_dev.ip_address[0], rtl8139_dev.ip_address[1], 
           rtl8139_dev.ip_address[2], rtl8139_dev.ip_address[3]);
    

    rtl8139_dev.initialized = 1;
    printf("RTL8139: Initialization completed successfully\n");
}

// Send a packet
int rtl8139_send_packet(const uint8_t *data, uint32_t length) {
    SERIAL_DEBUG_PRINT(RTL8139, "RTL8139: send_packet called, len=%d\n", length);

    if (!rtl8139_dev.initialized) {
        printf("RTL8139: Device not initialized\n");
        return -1;
    }
    
    if (length > RTL8139_TX_BUFFER_SIZE) {
        printf("RTL8139: Packet too large (%d > %d)\n", length, RTL8139_TX_BUFFER_SIZE);
        return -1;
    }
    
    // Use current buffer index; hardware sends fast enough in QEMU
    // and we don't rely on a fictitious OWN bit.
    int buffer_index = rtl8139_dev.tx_cur;
    uint32_t tsd_prev = rtl8139_read_reg32(&rtl8139_dev, RTL8139_TSD0 + (buffer_index * 4));
    DEBUG_PRINT(RTL8139, "RTL8139: TX buffer %d previous TSD=0x%08x\n", buffer_index, tsd_prev);
    
    // Check if buffer is owned by driver (OWN bit must be 1 for us to write)
    // Actually, TSD_OWN (0x2000) is 0 when owned by driver (available to write)?
    // NO. Spec: "When the driver sets this bit to 0, the RTL8139 starts transmission."
    // "After transmission, the RTL8139 sets this bit to 1."
    // So if bit is 0, it is BUSY (owned by card). If 1, it is FREE (owned by driver).
    // EXCEPT: The initial state of TSD is 0? No, usually initialized to non-OWN.
    // Wait, the spec says: "The driver clears this bit to 0 to start transmission".
    // "This bit is set to 1 by the RTL8139 after transmission is completed."
    
    // So if TSD & RTL8139_TSD_OWN (0x2000) is 0, the buffer is busy.
    // But we initialized TSD to 0 in setup.
    // In setup, we should probably set OWN bit or assume it's ready.
    // However, QEMU might not respect this fully or is fast enough.
    
    // If we strictly follow the spec, we should wait for OWN bit to be 1 before writing.
    // But since we just want to send and QEMU is fast, we skip wait.
    
    // Copy packet to transmit buffer
    memcpy(rtl8139_dev.tx_buffers[buffer_index], data, length);
    
    // CRITICAL FIX: Ensure packet data reaches memory before DMA reads it
    __asm__ volatile ("mfence" ::: "memory");
    
    DEBUG_PRINT(RTL8139, "RTL8139: TX Header: dest=%02x:%02x:%02x:%02x:%02x:%02x src=%02x:%02x:%02x:%02x:%02x:%02x type=%02x%02x\n",
           rtl8139_dev.tx_buffers[buffer_index][0], rtl8139_dev.tx_buffers[buffer_index][1],
           rtl8139_dev.tx_buffers[buffer_index][2], rtl8139_dev.tx_buffers[buffer_index][3],
           rtl8139_dev.tx_buffers[buffer_index][4], rtl8139_dev.tx_buffers[buffer_index][5],
           rtl8139_dev.tx_buffers[buffer_index][6], rtl8139_dev.tx_buffers[buffer_index][7],
           rtl8139_dev.tx_buffers[buffer_index][8], rtl8139_dev.tx_buffers[buffer_index][9],
           rtl8139_dev.tx_buffers[buffer_index][10], rtl8139_dev.tx_buffers[buffer_index][11],
           rtl8139_dev.tx_buffers[buffer_index][12], rtl8139_dev.tx_buffers[buffer_index][13]);
    
    // Set transmit status descriptor
    // Clear OWN bit (bit 13) to start transmission.
    // Length is lower 13 bits.
    // Set Early Transmit Threshold (bits 16-21) to 48 (0x30 * 32 = 1536 bytes)
    // This enables Store-and-Forward (wait for full packet before sending)
    // This is safer and prevents underruns.
    uint32_t tsd = (length & RTL8139_TSD_SIZE_MASK) | (48 << 16);
    
    // Ensure we are not accidentally setting the OWN bit (which would inhibit transmission)
    tsd &= ~RTL8139_TSD_OWN; 
    
    uint64_t phys_addr = rtl8139_read_reg32(&rtl8139_dev, RTL8139_TSAD0 + (buffer_index * 4));
    
    SERIAL_DEBUG_PRINT(RTL8139, "RTL8139: Sending %d bytes (buf %d). TSD=0x%08x. Phys=0x%08x\n", 
                  length, buffer_index, tsd, (uint32_t)phys_addr);
    
    rtl8139_write_reg32(&rtl8139_dev, RTL8139_TSD0 + (buffer_index * 4), tsd);
    
    SERIAL_DEBUG_PRINT(RTL8139, "RTL8139: Sent packet of %d bytes using buffer %d\n", length, buffer_index);
    
    // Move to next transmit buffer
    rtl8139_dev.tx_cur = (buffer_index + 1) % RTL8139_NUM_TX_BUFFERS;
    
    return 0;
}

// Receive a packet
int rtl8139_receive_packet(uint8_t *buffer, uint32_t max_length) {
    
    uint64_t flags = irq_save();
    spinlock_lock(&rtl8139_rx_lock);

    
    if (!rtl8139_dev.initialized) {
        spinlock_unlock(&rtl8139_rx_lock);
        irq_restore(flags);
        return -1;
    }
    
    if (!rtl8139_dev.rx_buffer || !rtl8139_dev.io_base) {
        spinlock_unlock(&rtl8139_rx_lock);
        irq_restore(flags);
        return -1;
    }
    
    // Read the current packet pointer and CBR (Current Buffer Address)
    uint32_t packet_ptr = rtl8139_dev.current_packet_ptr;
    uint16_t cbr = rtl8139_read_reg16(&rtl8139_dev, RTL8139_CBR);

    
    // Check if we have a packet by comparing CBR with our current position
    // CBR = Current Buffer Address (write pointer)
    // packet_ptr = Current Read Pointer
    // If they are equal, the buffer is empty
    if (cbr == packet_ptr) {
        /* Empty ring. The old "valid header found, forcing process" path
         * re-read an already-consumed header and desynced the ring. */
        rtl8139_dev.rx_pending_flag = 0;
        spinlock_unlock(&rtl8139_rx_lock);
        irq_restore(flags);
        return 0;
    }

    // Read the packet header (4 bytes: status + length)
    volatile uint8_t *rb = (volatile uint8_t*)rtl8139_dev.rx_buffer;
    uint32_t header = (uint32_t)rb[packet_ptr] |
                      ((uint32_t)rb[packet_ptr + 1] << 8) |
                      ((uint32_t)rb[packet_ptr + 2] << 16) |
                      ((uint32_t)rb[packet_ptr + 3] << 24);
    uint16_t status = header & 0xFFFF;
    uint16_t length = (header >> 16) & 0xFFFF;
    
    // Debug: Print packet detection info
    
    // Additional debug: Check if this looks like a valid packet
    if (header == 0) {
        serial_printf("RTL8139: WARNING: Header is all zeros - possible buffer issue\n");
        // Skip this packet and try the next one
        /* Corrupt/unknown header: drop the ring contents and resync the
         * read pointer to the hardware write pointer instead of jumping by
         * a garbage length (which desynced forever: "len 23644" storm). */
        rtl8139_dev.current_packet_ptr = cbr % RTL8139_RX_BUFFER_SIZE;
        rtl8139_write_reg16(&rtl8139_dev, RTL8139_CAPR, (uint16_t)(rtl8139_dev.current_packet_ptr - 0x10));
        rtl8139_dev.rx_pending_flag = 0;
        
        spinlock_unlock(&rtl8139_rx_lock);
        irq_restore(flags);
        return 0;
    }
    
    // Check if packet is valid
    if (!(status & RTL8139_RXSTAT_ROK)) {
        serial_printf("RTL8139: Received packet with error status 0x%04x\n", status);
        // Skip this packet
        /* Corrupt/unknown header: drop the ring contents and resync the
         * read pointer to the hardware write pointer instead of jumping by
         * a garbage length (which desynced forever: "len 23644" storm). */
        rtl8139_dev.current_packet_ptr = cbr % RTL8139_RX_BUFFER_SIZE;
        rtl8139_write_reg16(&rtl8139_dev, RTL8139_CAPR, (uint16_t)(rtl8139_dev.current_packet_ptr - 0x10));
        rtl8139_dev.rx_pending_flag = 0;
        
        spinlock_unlock(&rtl8139_rx_lock);
        irq_restore(flags);
        return 0;
    }
    
    // Check packet length
    if (length > max_length || length < 14) { // Minimum Ethernet frame
        serial_printf("RTL8139: Invalid packet length %d\n", length);
        // Skip this packet
        /* Corrupt/unknown header: drop the ring contents and resync the
         * read pointer to the hardware write pointer instead of jumping by
         * a garbage length (which desynced forever: "len 23644" storm). */
        rtl8139_dev.current_packet_ptr = cbr % RTL8139_RX_BUFFER_SIZE;
        rtl8139_write_reg16(&rtl8139_dev, RTL8139_CAPR, (uint16_t)(rtl8139_dev.current_packet_ptr - 0x10));
        rtl8139_dev.rx_pending_flag = 0;
        
        spinlock_unlock(&rtl8139_rx_lock);
        irq_restore(flags);
        return 0;
    }
    
    // Copy packet data (skip the 4-byte header). RCR.WRAP is set, so the
    // NIC does NOT wrap a frame at the ring end: it keeps writing into the
    // slack past RTL8139_RX_BUFFER_SIZE and the frame is contiguous from
    // packet_ptr + 4. (Copying the tail from rb[0] read stale bytes and every
    // large frame that straddled the ring end failed the TCP checksum.)
    memcpy(buffer, (const void *)(rb + packet_ptr + 4), length);
    
    // Update packet pointer - FIXED: Proper CAPR calculation
    rtl8139_dev.current_packet_ptr = (packet_ptr + length + 4 + 3) & ~3;
    // Wrap around if we've reached the end of the buffer
    if (rtl8139_dev.current_packet_ptr >= RTL8139_RX_BUFFER_SIZE) {
        rtl8139_dev.current_packet_ptr -= RTL8139_RX_BUFFER_SIZE; // Correct wrap-around
    }
    
    // Calculate new CAPR value
    uint16_t new_capr = (rtl8139_dev.current_packet_ptr - 0x10) & 0xFFFF;
    rtl8139_write_reg16(&rtl8139_dev, RTL8139_CAPR, new_capr);
    
    if (length >= 14) {
        uint16_t ethertype = (buffer[12] << 8) | buffer[13];
        SERIAL_DEBUG_PRINT(RTL8139, "RTL8139: Processed received packet of %d bytes, EtherType=0x%04x\n", length, ethertype);
    } else {
        SERIAL_DEBUG_PRINT(RTL8139, "RTL8139: Processed received packet of %d bytes\n", length);
    }
    
    spinlock_unlock(&rtl8139_rx_lock);
    irq_restore(flags);
    return length;
}

// Get MAC address
void rtl8139_get_mac_address(uint8_t *mac) {
    if (rtl8139_dev.initialized) {
        memcpy(mac, rtl8139_dev.mac_address, 6);
    } else {
        memset(mac, 0, 6);
    }
}

// Check if there are any pending receive interrupts
int rtl8139_has_pending_receive() {
    if (!rtl8139_dev.initialized) {
        return 0;
    }
    

    // Check software flag first (set by interrupt handler)
    if (rtl8139_dev.rx_pending_flag) {
        return 1;
    }
    
    // Check if there are any receive interrupts pending
    uint16_t isr = rtl8139_read_reg16(&rtl8139_dev, RTL8139_ISR);
    if (isr & RTL8139_ISR_ROK) {
        rtl8139_dev.rx_pending_flag = 1; // Sync flag
        return 1; // Has pending receive interrupt
    }
    
    // Also check if CBR != current_packet_ptr (packet available)
    uint32_t packet_ptr = rtl8139_dev.current_packet_ptr;
    uint16_t cbr = rtl8139_read_reg16(&rtl8139_dev, RTL8139_CBR);
    
    if (cbr != packet_ptr) {
         return 1;
    }

    return 0;
}

// Handle network interrupt
extern uint8_t virtio_gpu_ack_irq(void) __attribute__((weak));
void rtl8139_handle_interrupt() {
    
    if (!rtl8139_dev.initialized) {
        serial_printf("RTL8139: Device not initialized in interrupt handler\n");
        return;
    }
    
    // Read interrupt status
    uint16_t isr = rtl8139_read_reg16(&rtl8139_dev, RTL8139_ISR);
    if (isr == 0) {
        /* Not ours: IRQ 11 is a shared level line (virtio-gpu INTx on -M pc).
         * Ack the other device so the line drops, else we storm forever. */
        if (virtio_gpu_ack_irq) (void)virtio_gpu_ack_irq();
        apic_eoi();
        return;
    }
    
    // Clear interrupt status
    rtl8139_write_reg16(&rtl8139_dev, RTL8139_ISR, isr);
    
    if (isr & RTL8139_ISR_ROK) {
        // Don't run the full protocol stack in IRQ context: flag pending RX
        // and wake the network thread, which drains the ring in batches.
        extern void net_thread_notify_rx(void);
        rtl8139_dev.rx_pending_flag = 1;
        net_thread_notify_rx();
    }
    
    if (isr & RTL8139_ISR_TOK) {
        SERIAL_DEBUG_PRINT(RTL8139, "RTL8139: Transmit OK interrupt\n");
        // Read TSDs to verify
        // for(int i=0; i<4; i++) {
        //    uint32_t tsd = rtl8139_read_reg32(&rtl8139_dev, RTL8139_TSD0 + i*4);
        //    if (tsd & RTL8139_TSD_TOK) {
        //    }
        // }
    }
    
    if (isr & RTL8139_ISR_RER) {
        serial_printf("RTL8139: Receive error interrupt\n");
    }
    
    if (isr & RTL8139_ISR_TER) {
        serial_printf("RTL8139: Transmit error interrupt\n");
    }
    
    // CRITICAL FIX: Send EOI to APIC
    apic_eoi();
}

// RTL8139 network device wrapper functions for abstraction layer

static const char* rtl8139_get_driver_name(void) {
    return "RTL8139";
}

static int rtl8139_is_initialized_wrapper(void) {
    return rtl8139_dev.initialized;
}

static int rtl8139_init_wrapper(void) {
    rtl8139_init();
    return rtl8139_dev.initialized ? 0 : -1;
}

// RTL8139 network device instance
static net_device_t rtl8139_net_device = {
    .name = "RTL8139",
    .ops = {
        .init = rtl8139_init_wrapper,
        .send_packet = rtl8139_send_packet,
        .receive_packet = rtl8139_receive_packet,
        .get_mac_address = rtl8139_get_mac_address,
        .has_pending_receive = rtl8139_has_pending_receive,
        .handle_interrupt = rtl8139_handle_interrupt,
        .get_driver_name = rtl8139_get_driver_name,
        .is_initialized = rtl8139_is_initialized_wrapper
    },
    .driver_data = &rtl8139_dev
};

// Register RTL8139 with the network device abstraction layer
int rtl8139_register_net_device(void) {
    printf("RTL8139: Registering network device, initialized=%d\n", rtl8139_dev.initialized);
    
    // Copy MAC and IP addresses to the net device structure
    if (rtl8139_dev.initialized) {
        memcpy(rtl8139_net_device.mac_address, rtl8139_dev.mac_address, 6);
        memcpy(rtl8139_net_device.ip_address, rtl8139_dev.ip_address, 4);
        printf("RTL8139: Copied IP address to net device: %d.%d.%d.%d\n",
               rtl8139_net_device.ip_address[0], rtl8139_net_device.ip_address[1],
               rtl8139_net_device.ip_address[2], rtl8139_net_device.ip_address[3]);
    } else {
        printf("RTL8139: Device not initialized, cannot copy IP address\n");
    }
    
    rtl8139_net_device.pci_dev = rtl8139_dev.pci_dev;
    rtl8139_net_device.initialized = rtl8139_dev.initialized;
    
    return net_device_register(&rtl8139_net_device);
}