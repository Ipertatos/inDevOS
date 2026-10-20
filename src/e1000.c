#include "net_ethernet.h"
#include "e1000.h"
#include "net_device.h"
#include "pci.h"
#include "mem.h"
#include "utils.h"
#include "serial.h"
#include "idt.h"
#include "string.h"
#include "net_ip.h"
#include "net_icmp.h"
#include "registers.h"
#include "net_arp.h"
#include "net_ip.h"
#include "net_icmp.h"
#include "apic.h"
#include "string.h"

e1000_device_t e1000_dev = {0};

// Helper functions for register access
uint8_t e1000_read_reg8(e1000_device_t *dev, uint32_t offset) {
    if (dev->use_io)
        return inb((uint16_t)(uintptr_t)dev->io_base + offset);
    else
        return *(volatile uint8_t*)(dev->io_base + offset);
}

uint16_t e1000_read_reg16(e1000_device_t *dev, uint32_t offset) {
    if (dev->use_io)
        return inw((uint16_t)(uintptr_t)dev->io_base + offset);
    else
        return *(volatile uint16_t*)(dev->io_base + offset);
}

uint32_t e1000_read_reg32(e1000_device_t *dev, uint32_t offset) {
    if (dev->use_io)
        return inl((uint16_t)(uintptr_t)dev->io_base + offset);
    else
        return *(volatile uint32_t*)(dev->io_base + offset);
}

void e1000_write_reg8(e1000_device_t *dev, uint32_t offset, uint8_t value) {
    if (dev->use_io)
        outb((uint16_t)(uintptr_t)dev->io_base + offset, value);
    else
        *(volatile uint8_t*)(dev->io_base + offset) = value;
}

void e1000_write_reg16(e1000_device_t *dev, uint32_t offset, uint16_t value) {
    if (dev->use_io)
        outw((uint16_t)(uintptr_t)dev->io_base + offset, value);
    else
        *(volatile uint16_t*)(dev->io_base + offset) = value;
}

void e1000_write_reg32(e1000_device_t *dev, uint32_t offset, uint32_t value) {
    if (dev->use_io)
        outl((uint16_t)(uintptr_t)dev->io_base + offset, value);
    else
        *(volatile uint32_t*)(dev->io_base + offset) = value;
}

// Reset the E1000 chip
static void e1000_reset(e1000_device_t *dev) {
    printf("E1000: Resetting device\n");
    
    // Set reset bit
    e1000_write_reg32(dev, E1000_CTRL, E1000_CTRL_RST);
    
    // Wait for reset to complete
    int timeout = 1000;
    while (timeout--) {
        if (!(e1000_read_reg32(dev, E1000_CTRL) & E1000_CTRL_RST)) {
            break;
        }
        // Small delay
        for (volatile int i = 0; i < 1000; i++);
    }
    
    if (timeout < 0) {
        printf("E1000: Reset timeout, continuing\n");
    } else {
        printf("E1000: Reset completed\n");
    }

    // 82574/e1000e keeps the link down until software sets SLU. Do not wait for LU.
    uint32_t ctrl = e1000_read_reg32(dev, E1000_CTRL);
    ctrl |= E1000_CTRL_SLU;
    ctrl &= ~(uint32_t)(E1000_CTRL_LRST | E1000_CTRL_PHY_RST | E1000_CTRL_RST);
    e1000_write_reg32(dev, E1000_CTRL, ctrl);
}

// 82540EM-class EERD uses DONE=bit4 and address<<8.
// 82573/82574 (the QEMU q35 default, e1000e) uses DONE=bit1 and address<<2.
// Polling the wrong DONE bit never completes, which hung boot right after reset.
static int e1000_eerd_new_layout(e1000_device_t *dev) {
    uint16_t id = (dev->pci_dev) ? dev->pci_dev->device_id : 0;
    return id == E1000_DEVICE_ID_82573L || id == E1000_DEVICE_ID_82574L;
}

// Read one EEPROM word. Returns 0 on success. Always bounded.
static int e1000_read_eeprom(e1000_device_t *dev, uint8_t addr, uint16_t *out) {
    int neu = e1000_eerd_new_layout(dev);
    uint32_t done_bit = neu ? E1000_EERD_DONE_NEW : E1000_EERD_DONE;
    uint32_t addr_shift = neu ? E1000_EERD_ADDR_SHIFT_NEW : E1000_EERD_ADDR_SHIFT;
    uint32_t cmd = ((uint32_t)addr << addr_shift) | E1000_EERD_START;

    e1000_write_reg32(dev, E1000_EERD, cmd);

    for (int i = 0; i < 10000; i++) {
        uint32_t v = e1000_read_reg32(dev, E1000_EERD);
        if (v & done_bit) {
            *out = (uint16_t)((v >> E1000_EERD_DATA_SHIFT) & 0xFFFF);
            return 0;
        }
    }

    printf("E1000: EEPROM read timeout at word %d\n", addr);
    return -1;
}

// MAC from EEPROM, else the address the NIC already loaded into RAL/RAH.
// Never spins. A missing MAC does not fail init.
static void e1000_read_mac(e1000_device_t *dev, uint8_t mac[6]) {
    for (int z = 0; z < 6; z++) mac[z] = 0;
    int ok = 1;
    for (int i = 0; i < 3; i++) {
        uint16_t word = 0;
        if (e1000_read_eeprom(dev, (uint8_t)i, &word) != 0) {
            ok = 0;
            break;
        }
        mac[i * 2] = (uint8_t)(word & 0xFF);
        mac[i * 2 + 1] = (uint8_t)((word >> 8) & 0xFF);
    }

    int nonzero = 0;
    int allff = 1;
    for (int i = 0; i < 6; i++) {
        if (mac[i] != 0) nonzero = 1;
        if (mac[i] != 0xFF) allff = 0;
    }
    if (ok && nonzero && !allff)
        return;

    printf("E1000: EEPROM MAC unavailable, reading RAL/RAH\n");
    uint32_t ral = e1000_read_reg32(dev, E1000_RA);
    uint32_t rah = e1000_read_reg32(dev, E1000_RA + 4);
    if (rah & E1000_RAH_AV) {
        mac[0] = (uint8_t)(ral & 0xFF);
        mac[1] = (uint8_t)((ral >> 8) & 0xFF);
        mac[2] = (uint8_t)((ral >> 16) & 0xFF);
        mac[3] = (uint8_t)((ral >> 24) & 0xFF);
        mac[4] = (uint8_t)(rah & 0xFF);
        mac[5] = (uint8_t)((rah >> 8) & 0xFF);
        return;
    }

    printf("E1000: No hardware MAC, using fallback\n");
    mac[0] = 0x52; mac[1] = 0x54; mac[2] = 0x00;
    mac[3] = 0x12; mac[4] = 0x34; mac[5] = 0x56;
}

// Setup receive descriptors and buffers
static void e1000_setup_rx(e1000_device_t *dev) {
    // Allocate receive descriptor array
    dev->rx_descs = kalloc_high(sizeof(e1000_rx_desc_t) * E1000_NUM_RX_DESC);
    if (!dev->rx_descs) {
        printf("E1000: Failed to allocate receive descriptors\n");
        return;
    }
    
    // Allocate receive buffers
    for (int i = 0; i < E1000_NUM_RX_DESC; i++) {
        dev->rx_buffers[i] = kalloc_high(E1000_RX_BUFFER_SIZE);
        if (!dev->rx_buffers[i]) {
            printf("E1000: Failed to allocate receive buffer %d\n", i);
            return;
        }
        
        // Clear buffer
        memset(dev->rx_buffers[i], 0, E1000_RX_BUFFER_SIZE);
        
        // Setup descriptor
        dev->rx_descs[i].addr = (uint64_t)dev->rx_buffers[i] - vmm_higher_half_offset;
        dev->rx_descs[i].length = E1000_RX_BUFFER_SIZE;
        dev->rx_descs[i].csum = 0;
        dev->rx_descs[i].status = 0;
        dev->rx_descs[i].errors = 0;
        dev->rx_descs[i].special = 0;
    }
    
    // Setup receive descriptor registers
    uint64_t rx_desc_phys = (uint64_t)dev->rx_descs - vmm_higher_half_offset;
    e1000_write_reg32(dev, E1000_RDBAL, (uint32_t)rx_desc_phys);
    e1000_write_reg32(dev, E1000_RDBAH, (uint32_t)(rx_desc_phys >> 32));
    e1000_write_reg32(dev, E1000_RDLEN, sizeof(e1000_rx_desc_t) * E1000_NUM_RX_DESC);
    e1000_write_reg32(dev, E1000_RDH, 0);
    e1000_write_reg32(dev, E1000_RDT, E1000_NUM_RX_DESC - 1);
    
    // Enable receiver
    uint32_t rctl = e1000_read_reg32(dev, E1000_RCTL);
    rctl |= E1000_RCTL_EN | E1000_RCTL_SBP | E1000_RCTL_UPE | E1000_RCTL_MPE;
    rctl |= E1000_RCTL_BSIZE_2048 | E1000_RCTL_SECRC;
    e1000_write_reg32(dev, E1000_RCTL, rctl);
    
    dev->rx_cur = 0;
    
    printf("E1000: Receive setup completed\n");
}

// Setup transmit descriptors and buffers
static void e1000_setup_tx(e1000_device_t *dev) {
    // Allocate transmit descriptor array
    dev->tx_descs = kalloc_high(sizeof(e1000_tx_desc_t) * E1000_NUM_TX_DESC);
    if (!dev->tx_descs) {
        printf("E1000: Failed to allocate transmit descriptors\n");
        return;
    }
    
    // Allocate transmit buffers
    for (int i = 0; i < E1000_NUM_TX_DESC; i++) {
        dev->tx_buffers[i] = kalloc_high(E1000_TX_BUFFER_SIZE);
        if (!dev->tx_buffers[i]) {
            printf("E1000: Failed to allocate transmit buffer %d\n", i);
            return;
        }
        
        // Clear buffer
        memset(dev->tx_buffers[i], 0, E1000_TX_BUFFER_SIZE);
        
        // Setup descriptor
        dev->tx_descs[i].addr = (uint64_t)dev->tx_buffers[i] - vmm_higher_half_offset;
        dev->tx_descs[i].length = 0;
        dev->tx_descs[i].cso = 0;
        dev->tx_descs[i].cmd = 0;
        dev->tx_descs[i].status = 0;
        dev->tx_descs[i].css = 0;
        dev->tx_descs[i].special = 0;
    }
    
    // Setup transmit descriptor registers
    uint64_t tx_desc_phys = (uint64_t)dev->tx_descs - vmm_higher_half_offset;
    e1000_write_reg32(dev, E1000_TDBAL, (uint32_t)tx_desc_phys);
    e1000_write_reg32(dev, E1000_TDBAH, (uint32_t)(tx_desc_phys >> 32));
    e1000_write_reg32(dev, E1000_TDLEN, sizeof(e1000_tx_desc_t) * E1000_NUM_TX_DESC);
    e1000_write_reg32(dev, E1000_TDH, 0);
    e1000_write_reg32(dev, E1000_TDT, 0);
    
    // Enable transmitter
    uint32_t tctl = e1000_read_reg32(dev, E1000_TCTL);
    printf("E1000: Initial TCTL: 0x%08x\n", tctl);
    tctl |= E1000_TCTL_EN | E1000_TCTL_PSP;
    tctl |= (15 << 4) | (64 << 12); // Collision threshold and distance
    printf("E1000: Setting TCTL to: 0x%08x\n", tctl);
    e1000_write_reg32(dev, E1000_TCTL, tctl);
    
    // Verify the write
    uint32_t tctl_verify = e1000_read_reg32(dev, E1000_TCTL);
    printf("E1000: TCTL after write: 0x%08x\n", tctl_verify);
    
    dev->tx_cur = 0;
    dev->tx_used = 0;
    
    printf("E1000: Transmit setup completed\n");
}

// Set MAC address
static void e1000_set_mac_address(e1000_device_t *dev, const uint8_t *mac) {
    memcpy(dev->mac_address, mac, 6);
    
    // Write MAC address to registers
    uint32_t low = (mac[0] << 0) | (mac[1] << 8) | (mac[2] << 16) | (mac[3] << 24);
    uint32_t high = (mac[4] << 0) | (mac[5] << 8);
    
    e1000_write_reg32(dev, E1000_RA, low);
    e1000_write_reg32(dev, E1000_RA + 4, high | 0x80000000); // Set valid bit
    
    serial_printf("E1000: MAC address set to %02x:%02x:%02x:%02x:%02x:%02x\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// Configure flow control
static void e1000_configure_flow_control(e1000_device_t *dev) {
    // Disable flow control for simplicity
    e1000_write_reg32(dev, E1000_FCT, 0);
    e1000_write_reg32(dev, E1000_FCRTL, 0);
    e1000_write_reg32(dev, E1000_FCRTH, 0);
}

// Enable interrupts
static void e1000_enable_interrupts(e1000_device_t *dev) {
    // Clear any pending interrupts
    e1000_read_reg32(dev, E1000_ICR);
    
    // Enable interrupts we care about
    uint32_t ims = E1000_ICR_LSC | E1000_ICR_RXT0 | E1000_ICR_RXDMT0;
    e1000_write_reg32(dev, E1000_IMS, ims);
    
    printf("E1000: Interrupts enabled\n");
}

// Disable interrupts
static void e1000_disable_interrupts(e1000_device_t *dev) {
    e1000_write_reg32(dev, E1000_IMC, 0xFFFFFFFF);
    printf("E1000: Interrupts disabled\n");
}

// Initialize E1000 device
void e1000_init() {
    // Guard against double init (driver_manager + legacy kernel.c bringup):
    // re-initializing would reset the NIC and leak DMA rings.
    if (e1000_dev.initialized) {
        printf("E1000: Already initialized, skipping\n");
        return;
    }
    printf("E1000: Initializing E1000 network adapter\n");
    
    struct pci_device *pci_dev = NULL;
    
    // Find E1000 device
    for (int i = 0; i < pci_device_count; i++) {
        if (pci_devices[i]->vendor_id == E1000_VENDOR_ID &&
            (pci_devices[i]->device_id == E1000_DEVICE_ID_82540EM ||
             pci_devices[i]->device_id == E1000_DEVICE_ID_82573L ||
             pci_devices[i]->device_id == E1000_DEVICE_ID_82574L)) {
            pci_dev = pci_devices[i];
            printf("E1000: Found device %04x:%04x\n", pci_dev->vendor_id, pci_dev->device_id);
            break;
        }
    }
    
    if (!pci_dev) {
        printf("E1000: No E1000 device found\n");
        return;
    }
    
    // Get BAR0 (I/O base address)
    uint32_t bar0 = pci_device_read(pci_dev, pci_reg4_offset, 4);
    uint32_t bar1 = pci_device_read(pci_dev, pci_reg5_offset, 4);
    
    if (bar0 & 1) {
        // I/O space
        e1000_dev.io_base = (uint8_t*)(uintptr_t)(bar0 & ~1);
        e1000_dev.use_io = 1;
        printf("E1000: Using I/O space at 0x%x\n", (uint32_t)(uintptr_t)e1000_dev.io_base);
    } else {
        // Memory space
        e1000_dev.io_base = (uint8_t*)(uintptr_t)(bar0 & ~0xF);
        e1000_dev.use_io = 0;
        printf("E1000: Using memory space at 0x%lx\n", (uint64_t)e1000_dev.io_base);
    }
    
    e1000_dev.pci_dev = pci_dev;
    
    // Reset device
    e1000_reset(&e1000_dev);
    
    // Read MAC (EEPROM layout depends on the device; never blocks forever)
    uint8_t mac[6];
    e1000_read_mac(&e1000_dev, mac);

    // Set MAC address
    e1000_set_mac_address(&e1000_dev, mac);
    
    // Configure flow control
    e1000_configure_flow_control(&e1000_dev);
    
    // Setup receive and transmit
    e1000_setup_rx(&e1000_dev);
    e1000_setup_tx(&e1000_dev);
    
    // Enable interrupts
    e1000_enable_interrupts(&e1000_dev);
    
    // Configure IOAPIC to route E1000 interrupt to vector 44
    // Get the LAPIC ID from the first CPU
    uint32_t lapic_id = 0; // Assuming single CPU for now
    
    // Read PCI interrupt line
    uint8_t interrupt_line = pci_device_read(pci_dev, 0x3C, 1) & 0xFF;
    printf("E1000: PCI interrupt line: %d\n", interrupt_line);
    
    // Route the interrupt to vector 44 (using a different vector than RTL8139)
    ioapic_redirect_irq(lapic_id, 44, interrupt_line, true); // Enable interrupt
    printf("E1000: Configured interrupt routing: IRQ %d -> vector 44\n", interrupt_line);
    
    // Set static IP address (192.168.56.2)
    e1000_dev.ip_address[0] = 192;
    e1000_dev.ip_address[1] = 168;
    e1000_dev.ip_address[2] = 56;
    e1000_dev.ip_address[3] = 2;
    
    // Initialize network stack
    arp_init();
    ip_init();
    icmp_init();

    
    e1000_dev.initialized = 1;
    
    printf("E1000: Initialization completed successfully\n");
}

// Send packet
int e1000_send_packet(const uint8_t *data, uint32_t length) {
    if (!e1000_dev.initialized) {
        printf("E1000: Device not initialized\n");
        return -1;
    }
    
    if (length > E1000_TX_BUFFER_SIZE) {
        printf("E1000: Packet too large (%d bytes)\n", length);
        return -1;
    }
    
    printf("E1000: Sending packet of %d bytes\n", length);
    
    // Wait for a free transmit descriptor
    while (e1000_dev.tx_used >= E1000_NUM_TX_DESC) {
        // Check if any descriptors are done
        for (int i = 0; i < E1000_NUM_TX_DESC; i++) {
            if (e1000_dev.tx_descs[i].status & E1000_TXD_STAT_DD) {
                e1000_dev.tx_descs[i].status = 0;
                e1000_dev.tx_used--;
            }
        }
        
        if (e1000_dev.tx_used >= E1000_NUM_TX_DESC) {
            // Still no free descriptors, wait a bit
            for (volatile int i = 0; i < 1000; i++);
        }
    }
    
    // Copy data to transmit buffer
    memcpy(e1000_dev.tx_buffers[e1000_dev.tx_cur], data, length);
    
    // Setup descriptor
    e1000_dev.tx_descs[e1000_dev.tx_cur].length = length;
    e1000_dev.tx_descs[e1000_dev.tx_cur].cmd = 0x0B; // EOP | IFCS | RS (bits 0,1,3)
    e1000_dev.tx_descs[e1000_dev.tx_cur].status = 0;
    
    printf("E1000: Descriptor %d setup - length=%d, cmd=0x%02x, addr=0x%016llx\n", 
           e1000_dev.tx_cur, length, e1000_dev.tx_descs[e1000_dev.tx_cur].cmd, 
           e1000_dev.tx_descs[e1000_dev.tx_cur].addr);
    
    // Update tail pointer
    e1000_dev.tx_cur = (e1000_dev.tx_cur + 1) % E1000_NUM_TX_DESC;
    e1000_dev.tx_used++;
    e1000_write_reg32(&e1000_dev, E1000_TDT, e1000_dev.tx_cur);
    
    printf("E1000: Packet sent successfully, new TDT=%d\n", e1000_dev.tx_cur);
    return 0; // Return 0 for success, consistent with other drivers
}

// Receive packet
int e1000_receive_packet(uint8_t *buffer, uint32_t max_length) {
    if (!e1000_dev.initialized) {
        return -1;
    }
    
    // Check if we have a packet
    if (!(e1000_dev.rx_descs[e1000_dev.rx_cur].status & E1000_RXD_STAT_DD)) {
        return 0; // No packet available
    }
    
    // Get packet length
    uint16_t length = e1000_dev.rx_descs[e1000_dev.rx_cur].length;
    
    if (length > max_length) {
        NET_RX_LOG("E1000: Received packet too large (%d bytes)\n", length);
        return -1;
    }
    
    // Copy packet data
    memcpy(buffer, e1000_dev.rx_buffers[e1000_dev.rx_cur], length);
    
    // Clear descriptor status
    e1000_dev.rx_descs[e1000_dev.rx_cur].status = 0;
    
    // Update tail pointer
    e1000_dev.rx_cur = (e1000_dev.rx_cur + 1) % E1000_NUM_RX_DESC;
    e1000_write_reg32(&e1000_dev, E1000_RDT, (e1000_dev.rx_cur - 1) % E1000_NUM_RX_DESC);
    
    return length;
}

// Get MAC address
void e1000_get_mac_address(uint8_t *mac) {
    if (e1000_dev.initialized) {
        memcpy(mac, e1000_dev.mac_address, 6);
    } else {
        memset(mac, 0, 6);
    }
}

// Check if there are pending receive packets
int e1000_has_pending_receive() {
    if (!e1000_dev.initialized) {
        return 0;
    }
    
    return (e1000_dev.rx_descs[e1000_dev.rx_cur].status & E1000_RXD_STAT_DD) != 0;
}

// Handle interrupt
void e1000_handle_interrupt() {
    if (!e1000_dev.initialized) {
        return;
    }
    
    // Read interrupt status
    uint32_t icr = e1000_read_reg32(&e1000_dev, E1000_ICR);
    
    if (icr & E1000_ICR_LSC) {
        // Link status change
        uint32_t status = e1000_read_reg32(&e1000_dev, E1000_STATUS);
        if (status & E1000_STATUS_LU) {
            printf("E1000: Link is up\n");
        } else {
            printf("E1000: Link is down\n");
        }
    }
    
    if (icr & (E1000_ICR_RXT0 | E1000_ICR_RXDMT0)) {
        // RX work pending: wake the network thread instead of consuming the
        // descriptors here (the protocol stack must not run in IRQ context,
        // and draining inline starved the net thread's receive path).
        extern void net_thread_notify_rx(void);
        net_thread_notify_rx();
    }
    
    // Clear interrupt
    e1000_read_reg32(&e1000_dev, E1000_ICR);
    
    // Send EOI to APIC
    apic_eoi();
}

// E1000 network device wrapper functions for abstraction layer

static const char* e1000_get_driver_name(void) {
    return "E1000";
}

static int e1000_is_initialized_wrapper(void) {
    return e1000_dev.initialized;
}

static int e1000_init_wrapper(void) {
    e1000_init();
    return e1000_dev.initialized ? 0 : -1;
}

// E1000 network device instance
static net_device_t e1000_net_device = {
    .name = "E1000",
    .ops = {
        .init = e1000_init_wrapper,
        .send_packet = e1000_send_packet,
        .receive_packet = e1000_receive_packet,
        .get_mac_address = e1000_get_mac_address,
        .has_pending_receive = e1000_has_pending_receive,
        .handle_interrupt = e1000_handle_interrupt,
        .get_driver_name = e1000_get_driver_name,
        .is_initialized = e1000_is_initialized_wrapper
    },
    .driver_data = &e1000_dev
};

// Register E1000 with the network device abstraction layer
int e1000_register_net_device(void) {
    // Copy MAC and IP addresses to the net device structure
    if (e1000_dev.initialized) {
        memcpy(e1000_net_device.mac_address, e1000_dev.mac_address, 6);
        memcpy(e1000_net_device.ip_address, e1000_dev.ip_address, 4);
    }
    
    e1000_net_device.pci_dev = e1000_dev.pci_dev;
    e1000_net_device.initialized = e1000_dev.initialized;
    
    return net_device_register(&e1000_net_device);
} 