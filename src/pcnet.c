#include "net_ethernet.h"
#include "string.h"
#include "pcnet.h"
#include "pci.h"
#include "mem.h"
#include "utils.h"
#include "serial.h"
#include "string.h"
#include "net_ip.h"
#include "net_arp.h"
#include "net_icmp.h"
#include "apic.h"

pcnet_device_t pcnet_dev = {0};

// Helper functions for register access
static void pcnet_write_csr(pcnet_device_t *dev, uint16_t index, uint16_t value) {
    outw(dev->io_base + PCNET_RAP, index);
    outw(dev->io_base + PCNET_RDP, value);
}

static uint16_t pcnet_read_csr(pcnet_device_t *dev, uint16_t index) {
    outw(dev->io_base + PCNET_RAP, index);
    return inw(dev->io_base + PCNET_RDP);
}

static void pcnet_write_bcr(pcnet_device_t *dev, uint16_t index, uint16_t value) {
    outw(dev->io_base + PCNET_RAP, index);
    outw(dev->io_base + PCNET_BDP, value);
}

static uint16_t pcnet_read_bcr(pcnet_device_t *dev, uint16_t index) {
    outw(dev->io_base + PCNET_RAP, index);
    return inw(dev->io_base + PCNET_BDP);
}

static void pcnet_reset(pcnet_device_t *dev) {
    printf("PCnet: Resetting device\n");
    inw(dev->io_base + PCNET_RESET);
    inw(dev->io_base + PCNET_RESET);
    
    // Wait a bit
    for (volatile int i = 0; i < 10000; i++);
    
    // Check if reset successful (CSR0 should be 0x0004 - STOP)
    uint16_t csr0 = pcnet_read_csr(dev, CSR0);
    printf("PCnet: CSR0 after reset: 0x%04x\n", csr0);
    
    // Switch to 32-bit mode (Software Style 2)
    // Write 0 to CSR58, then write 2 to CSR58? Or just write 2?
    // According to docs, we should enable 32-bit mode.
    // VMware usually emulates Am79C970A which supports this.
    
    // Important: We must set SWSTYLE before anything else to ensure 32-bit structures are used.
    pcnet_write_csr(dev, CSR58, 0x0002);
    
    uint16_t csr58 = pcnet_read_csr(dev, CSR58);
    printf("PCnet: CSR58 (SWSTYLE): 0x%04x\n", csr58);
}

static void pcnet_setup_buffers(pcnet_device_t *dev) {
    // Ring sizes: 16 entries each (2^4)
    dev->rx_ring_size_log2 = 4;
    dev->tx_ring_size_log2 = 4;
    int ring_count = 1 << 4;
    
    // Allocate rings (16-byte aligned)
    // pcnet_desc_t is 16 bytes, so array is naturally aligned if allocation is aligned.
    // kalloc_high usually returns page-aligned memory or at least reasonably aligned.
    
    dev->rx_ring = (pcnet_desc_t*)kalloc_high(sizeof(pcnet_desc_t) * ring_count);
    dev->tx_ring = (pcnet_desc_t*)kalloc_high(sizeof(pcnet_desc_t) * ring_count);
    dev->init_block = (pcnet_init_block_t*)kalloc_high(sizeof(pcnet_init_block_t));
    
    dev->rx_buffers = (uint8_t**)kalloc_high(sizeof(uint8_t*) * ring_count);
    dev->tx_buffers = (uint8_t**)kalloc_high(sizeof(uint8_t*) * ring_count);
    
    memset(dev->rx_ring, 0, sizeof(pcnet_desc_t) * ring_count);
    memset(dev->tx_ring, 0, sizeof(pcnet_desc_t) * ring_count);
    memset(dev->init_block, 0, sizeof(pcnet_init_block_t));
    
    // Setup RX ring
    for (int i = 0; i < ring_count; i++) {
        dev->rx_buffers[i] = (uint8_t*)kalloc_high(1536); // standard Ethernet frame size + margin
        uint32_t phys_addr = (uint32_t)((uint64_t)dev->rx_buffers[i] - vmm_higher_half_offset);
        
        dev->rx_ring[i].buffer_addr = phys_addr;
        // Length is 2's complement of buffer size
        dev->rx_ring[i].status_length = (uint32_t)(short)(-1536) & 0xFFFF; // Buf size in lower 16 bits
        dev->rx_ring[i].status_length |= 0x80000000; // OWN bit (card owns it)
    }
    
    // Setup TX ring
    for (int i = 0; i < ring_count; i++) {
        dev->tx_buffers[i] = (uint8_t*)kalloc_high(1536);
        uint32_t phys_addr = (uint32_t)((uint64_t)dev->tx_buffers[i] - vmm_higher_half_offset);
        
        dev->tx_ring[i].buffer_addr = phys_addr;
        dev->tx_ring[i].status_length = 0; // Host owns it initially
    }
    
    // Setup Initialization Block
    // Mode: Promiscuous? No.
    // TLEN/RLEN are log2 of ring length << 4 for RLEN, << 12 for TLEN
    dev->init_block->mode = (dev->rx_ring_size_log2 << 4) | (dev->tx_ring_size_log2 << 12);
    
    // MAC address
    dev->init_block->phys_addr_lower = dev->mac_address[0] | (dev->mac_address[1] << 8) | 
                                      (dev->mac_address[2] << 16) | (dev->mac_address[3] << 24);
    dev->init_block->phys_addr_upper = dev->mac_address[4] | (dev->mac_address[5] << 8);
    
    // Logical Address Filter (Broadcast)
    dev->init_block->logical_addr_filter_lower = 0xFFFFFFFF;
    dev->init_block->logical_addr_filter_upper = 0xFFFFFFFF;
    
    // Ring Addresses (Physical)
    dev->init_block->rdra = (uint32_t)((uint64_t)dev->rx_ring - vmm_higher_half_offset);
    dev->init_block->tdra = (uint32_t)((uint64_t)dev->tx_ring - vmm_higher_half_offset);
    
    printf("PCnet: RX Ring Phys: 0x%08x, TX Ring Phys: 0x%08x\n", 
           dev->init_block->rdra, dev->init_block->tdra);
}

void pcnet_init() {
    // Guard against double init (driver_manager + legacy kernel.c bringup).
    if (pcnet_dev.initialized) {
        printf("PCnet: Already initialized, skipping\n");
        return;
    }
    printf("PCnet: Initializing driver\n");
    
    struct pci_device *pci_dev = NULL;
    
    // Find PCnet device
    for (int i = 0; i < pci_device_count; i++) {
        if (pci_devices[i]->vendor_id == PCNET_VENDOR_ID &&
            pci_devices[i]->device_id == PCNET_DEVICE_ID) {
            pci_dev = pci_devices[i];
            break;
        }
    }
    
    if (!pci_dev) {
        printf("PCnet: Device not found\n");
        return;
    }
    
    pcnet_dev.pci_dev = pci_dev;
    
    // Enable bus mastering
    uint16_t cmd = pci_device_read(pci_dev, pci_reg1_offset, 2);
    pci_device_write(pci_dev, pci_reg1_offset, 2, cmd | 0x0004); // Bus Master
    
    // Get I/O Base
    uint32_t bar0 = pci_device_read(pci_dev, pci_reg4_offset, 4);
    pcnet_dev.io_base = bar0 & ~3;
    printf("PCnet: I/O Base: 0x%04x\n", pcnet_dev.io_base);
    
    // Read MAC Address from PROM (offset 0x00-0x05)
    for (int i = 0; i < 6; i++) {
        pcnet_dev.mac_address[i] = inb(pcnet_dev.io_base + i);
    }
    printf("PCnet: MAC Address: %02x:%02x:%02x:%02x:%02x:%02x\n",
           pcnet_dev.mac_address[0], pcnet_dev.mac_address[1], pcnet_dev.mac_address[2],
           pcnet_dev.mac_address[3], pcnet_dev.mac_address[4], pcnet_dev.mac_address[5]);
           
    pcnet_reset(&pcnet_dev);
    
    // Setup buffers
    pcnet_setup_buffers(&pcnet_dev);
    
    // Write Init Block Address to CSR1/CSR2
    uint32_t init_block_phys = (uint32_t)((uint64_t)pcnet_dev.init_block - vmm_higher_half_offset);
    pcnet_write_csr(&pcnet_dev, CSR1, init_block_phys & 0xFFFF);
    pcnet_write_csr(&pcnet_dev, CSR2, (init_block_phys >> 16) & 0xFFFF);
    
    // Init command
    pcnet_write_csr(&pcnet_dev, CSR0, CSR0_INIT | CSR0_INEA);
    
    // Wait for Init Done
    int timeout = 10000;
    while (timeout--) {
        uint16_t csr0 = pcnet_read_csr(&pcnet_dev, CSR0);
        if (csr0 & CSR0_IDON) {
            break;
        }
    }
    
    if (timeout <= 0) {
        printf("PCnet: Initialization timeout\n");
        return;
    }
    
    // Start the card
    pcnet_write_csr(&pcnet_dev, CSR0, CSR0_IDON | CSR0_STRT | CSR0_INEA);
    
    printf("PCnet: Device started\n");
    
    // Configure interrupt
    uint8_t irq = pci_device_read(pci_dev, 0x3C, 1) & 0xFF;
    printf("PCnet: IRQ %d\n", irq);
    // Note: IOAPIC routing should be done here if needed, similar to E1000
    // Assuming generic interrupt handling or legacy PIC for now, 
    // but the system seems to use IOAPIC.
    // We'll route it to vector 45 just to be safe and distinct.
    ioapic_redirect_irq(0, 45, irq, true);
    
    // Set static IP for now
    pcnet_dev.ip_address[0] = 192;
    pcnet_dev.ip_address[1] = 168;
    pcnet_dev.ip_address[2] = 56;
    pcnet_dev.ip_address[3] = 3;
    
    pcnet_dev.initialized = 1;
}

int pcnet_send_packet(const uint8_t *data, uint32_t length) {
    if (!pcnet_dev.initialized) return -1;
    
    int cur = pcnet_dev.tx_cur;
    
    // Check if current descriptor is owned by host (OWN bit 31 clear)
    if (pcnet_dev.tx_ring[cur].status_length & 0x80000000) {
        printf("PCnet: TX Buffer full\n");
        return -1;
    }
    
    // Copy data
    if (length > 1518) length = 1518;
    memcpy(pcnet_dev.tx_buffers[cur], data, length);
    
    // Set status/length
    // Bits 0-11: 2's complement of length
    // Bit 25: STP (Start of Packet)
    // Bit 24: ENP (End of Packet)
    // Bit 31: OWN (Owner is Card)
    uint32_t status = 0;
    status |= (uint32_t)(short)(-length) & 0xFFFF;
    status |= 0x80000000; // OWN
    status |= 0x03000000; // STP | ENP
    
    pcnet_dev.tx_ring[cur].status_length = status;
    
    // Advance index
    pcnet_dev.tx_cur = (cur + 1) % (1 << pcnet_dev.tx_ring_size_log2);
    
    // Trigger transmission (TDMD in CSR0)
    uint16_t csr0 = pcnet_read_csr(&pcnet_dev, CSR0);
    pcnet_write_csr(&pcnet_dev, CSR0, csr0 | CSR0_TDMD);
    
    printf("PCnet: Packet sent (%d bytes)\n", length);
    return 0;
}

int pcnet_receive_packet(uint8_t *buffer, uint32_t max_length) {
    if (!pcnet_dev.initialized) return -1;
    
    int cur = pcnet_dev.rx_cur;
    
    // Check if card owns it (OWN bit 31)
    if (pcnet_dev.rx_ring[cur].status_length & 0x80000000) {
        return 0; // Still owned by card, no packet
    }
    
    // Packet received
    // Check for errors (ERR bit)
    if (pcnet_dev.rx_ring[cur].status_length & 0x40000000) {
        NET_RX_LOG("PCnet: RX Error\n");
    }
    
    // Read MCNT (Message Byte Count) - bits 0-11 of unaligned structure? 
    // In 32-bit mode:
    // status_length is word 1.
    // There is another field for msg length usually in descriptor 2 or 3?
    // Wait, pcnet_desc_t:
    // 0: address
    // 4: status (16) + length (16)
    // 8: msg_length (16) + ...
    // 12: user
    
    // I defined pcnet_desc_t with reserved fields.
    // The message length is in the third 32-bit word (offset 8), lower 16 bits?
    // Actually, in 32-bit mode:
    // Word 0: Buffer Address
    // Word 1: BCNT (16) | Ones (4) | Status (12) -- Wait, layout is specific.
    // Word 2: MCNT (16) | ...
    
    // Let's assume MCNT is available.
    // I need to update the struct in header or just cast.
    
    // Re-reading datasheet spec for 32-bit descriptor (SWSTYLE=2):
    // RMD0: RX buffer address
    // RMD1: BCNT (16-31) | STATUS (0-15) ?? No, usually [31:20] BCNT, [15:0] Status?
    // Actually:
    // RMD1:
    //   31: OWN
    //   30: ERR
    //   29: FRAM
    //   28: OFLO
    //   27: CRC
    //   26: BUFF
    //   25: STP
    //   24: ENP
    //   15-12: Ones (1111)
    //   11-0: BCNT (2's comp)
    
    // RMD2:
    //   31-24: RCV/RPC
    //   23-16: RCV/RCC
    //   11-0: MCNT (Message Byte Count)
    
    // So MCNT is in the third word.
    
    uint32_t *desc_words = (uint32_t*)&pcnet_dev.rx_ring[cur];
    uint32_t rmd2 = desc_words[2];
    uint32_t len = rmd2 & 0xFFF; // Lower 12 bits
    
    if (len > max_length) len = max_length;
    
    memcpy(buffer, pcnet_dev.rx_buffers[cur], len);
    
    // Reset descriptor
    // Status/Length = 0x8000 | (2's comp of buffer size)
    // Buffer size is 1536
    pcnet_dev.rx_ring[cur].status_length = 0x80000000 | (0xF000 | ((short)(-1536) & 0xFFF)); 
    // Wait, BCNT is 12 bits? Yes.
    
    pcnet_dev.rx_cur = (cur + 1) % (1 << pcnet_dev.rx_ring_size_log2);
    
    return len;
}

void pcnet_handle_interrupt() {
    if (!pcnet_dev.initialized) return;
    
    uint16_t csr0 = pcnet_read_csr(&pcnet_dev, CSR0);
    
    if (csr0 & CSR0_RINT) {
        // Receive interrupt: clear and wake the network thread to drain
        pcnet_write_csr(&pcnet_dev, CSR0, csr0 | CSR0_RINT); // Clear

        extern void net_thread_notify_rx(void);
        net_thread_notify_rx();
    }
    
    if (csr0 & CSR0_TINT) {
        // Transmit interrupt
        pcnet_write_csr(&pcnet_dev, CSR0, csr0 | CSR0_TINT); // Clear
    }
    
    // Acknowledge interrupt in local APIC
    apic_eoi();
}

int pcnet_has_pending_receive() {
    if (!pcnet_dev.initialized) return 0;
    int cur = pcnet_dev.rx_cur;
    return !(pcnet_dev.rx_ring[cur].status_length & 0x80000000);
}

void pcnet_get_mac_address(uint8_t *mac) {
    memcpy(mac, pcnet_dev.mac_address, 6);
}

// Net device wrapper
static const char* pcnet_get_driver_name(void) {
    return "PCnet";
}

static int pcnet_is_initialized_wrapper(void) {
    return pcnet_dev.initialized;
}

static int pcnet_init_wrapper(void) {
    pcnet_init();
    return pcnet_dev.initialized ? 0 : -1;
}

static net_device_t pcnet_net_device = {
    .name = "PCnet",
    .ops = {
        .init = pcnet_init_wrapper,
        .send_packet = pcnet_send_packet,
        .receive_packet = pcnet_receive_packet,
        .get_mac_address = pcnet_get_mac_address,
        .has_pending_receive = pcnet_has_pending_receive,
        .handle_interrupt = pcnet_handle_interrupt,
        .get_driver_name = pcnet_get_driver_name,
        .is_initialized = pcnet_is_initialized_wrapper
    },
    .driver_data = &pcnet_dev
};

int pcnet_register_net_device(void) {
    if (pcnet_dev.initialized) {
        memcpy(pcnet_net_device.mac_address, pcnet_dev.mac_address, 6);
        memcpy(pcnet_net_device.ip_address, pcnet_dev.ip_address, 4);
    }
    pcnet_net_device.pci_dev = pcnet_dev.pci_dev;
    pcnet_net_device.initialized = pcnet_dev.initialized;
    
    return net_device_register(&pcnet_net_device);
}

