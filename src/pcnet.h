#pragma once

#include <stdint.h>
#include "pci.h"
#include "net_device.h"

// AMD PCnet-PCI II constants
#define PCNET_VENDOR_ID 0x1022
#define PCNET_DEVICE_ID 0x2000

// I/O offsets (16-bit)
#define PCNET_RDP 0x10  // Register Data Port
#define PCNET_RAP 0x12  // Register Address Port
#define PCNET_RESET 0x14
#define PCNET_BDP 0x16  // Bus Control Register Data Port

// CSR Registers (selected via RAP)
#define CSR0  0   // Controller Status and Control
#define CSR1  1   // Initialization Block Address 0 (Lower 16 bits)
#define CSR2  2   // Initialization Block Address 1 (Upper 16 bits)
#define CSR3  3   // Interrupt Masks and Deferral Control
#define CSR4  4   // Test and Features Control
#define CSR15 15  // Mode
#define CSR58 58  // Software Style
#define CSR88 88  // Chip ID Lower
#define CSR89 89  // Chip ID Upper

// CSR0 bits
#define CSR0_INIT 0x0001
#define CSR0_STRT 0x0002
#define CSR0_STOP 0x0004
#define CSR0_TDMD 0x0008
#define CSR0_TXON 0x0010
#define CSR0_RXON 0x0020
#define CSR0_INEA 0x0040
#define CSR0_INTR 0x0080
#define CSR0_IDON 0x0100
#define CSR0_TINT 0x0200
#define CSR0_RINT 0x0400
#define CSR0_MERR 0x0800
#define CSR0_MISS 0x1000
#define CSR0_CERR 0x2000
#define CSR0_BABL 0x4000
#define CSR0_ERR  0x8000

// Initialization Block (must be 32-bit aligned, usually 16-bit mode structures)
// The PCnet-PCI II supports 32-bit mode (Software Style 2 or 3).
// We'll use 32-bit mode (SWSTYLE=2).

typedef struct {
    uint32_t mode;
    uint32_t phys_addr_lower; // MAC address 0-3
    uint32_t phys_addr_upper; // MAC address 4-5 + reserved
    uint32_t logical_addr_filter_lower;
    uint32_t logical_addr_filter_upper;
    uint32_t rdra; // Receive Descriptor Ring Address
    uint32_t tdra; // Transmit Descriptor Ring Address
} __attribute__((packed)) pcnet_init_block_t;

// Descriptor Entry (32-bit mode)
typedef struct {
    uint32_t buffer_addr; // Buffer Address
    uint32_t status_length; // Status (16) + Length (16) 2s complement
    uint32_t reserved;
    uint32_t reserved2;
} __attribute__((packed)) pcnet_desc_t;

// PCnet device structure
typedef struct {
    struct pci_device *pci_dev;
    uint16_t io_base;
    uint8_t mac_address[6];
    uint8_t ip_address[4];
    
    // Memory structures
    pcnet_init_block_t *init_block;
    pcnet_desc_t *rx_ring;
    pcnet_desc_t *tx_ring;
    uint8_t **rx_buffers;
    uint8_t **tx_buffers;
    
    int rx_ring_size_log2; // Log2 of ring size (e.g., 4 for 16 entries)
    int tx_ring_size_log2;
    
    int rx_cur;
    int tx_cur;
    
    int initialized;
    int use_32bit; // 1 if using 32-bit mode
} pcnet_device_t;

// Function prototypes
void pcnet_init(void);
int pcnet_send_packet(const uint8_t *data, uint32_t length);
int pcnet_receive_packet(uint8_t *buffer, uint32_t max_length);
void pcnet_get_mac_address(uint8_t *mac);
int pcnet_has_pending_receive(void);
void pcnet_handle_interrupt(void);
int pcnet_register_net_device(void);

// Global instance
extern pcnet_device_t pcnet_dev;



