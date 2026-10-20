#pragma once

#include "types.h"
#include "pci.h"

// E1000 PCI Vendor and Device IDs
#define E1000_VENDOR_ID      0x8086
#define E1000_DEVICE_ID_82540EM 0x100E
#define E1000_DEVICE_ID_82573L  0x109A
#define E1000_DEVICE_ID_82574L  0x10D3

// E1000 Register Offsets
#define E1000_CTRL           0x0000  // Device Control
#define E1000_STATUS         0x0008  // Device Status
#define E1000_EERD           0x0014  // EEPROM Read
#define E1000_EERW           0x0018  // EEPROM Write
#define E1000_CTRLEXT        0x0018  // Extended Device Control
#define E1000_MDIC           0x0020  // MDI Control
#define E1000_FCAL           0x0028  // Flow Control Address Low
#define E1000_FCAH           0x002C  // Flow Control Address High
#define E1000_FCT            0x0030  // Flow Control Type
#define E1000_VET            0x0038  // VLAN Ether Type
#define E1000_ICR            0x00C0  // Interrupt Cause Read
#define E1000_ITR            0x00C4  // Interrupt Throttling
#define E1000_ICS            0x00C8  // Interrupt Cause Set
#define E1000_IMS            0x00D0  // Interrupt Mask Set
#define E1000_IMC            0x00D8  // Interrupt Mask Clear
#define E1000_RCTL           0x0100  // Receive Control
#define E1000_TCTL           0x0400  // Transmit Control
#define E1000_PBA            0x1000  // Packet Buffer Allocation
#define E1000_FCRTH          0x2160  // Flow Control Receive Threshold High
#define E1000_FCRTL          0x2168  // Flow Control Receive Threshold Low
#define E1000_RDBAL          0x2800  // Receive Descriptor Base Address Low
#define E1000_RDBAH          0x2804  // Receive Descriptor Base Address High
#define E1000_RDLEN          0x2808  // Receive Descriptor Length
#define E1000_RDH            0x2810  // Receive Descriptor Head
#define E1000_RDT            0x2818  // Receive Descriptor Tail
#define E1000_RDTR           0x2820  // Receive Delay Timer
#define E1000_RXDCTL         0x2828  // Receive Descriptor Control
#define E1000_RADV           0x282C  // Receive Interrupt Absolute Delay Timer
#define E1000_RSRPD          0x2C00  // Receive Small Packet Detect Interrupt
#define E1000_TDBAL          0x3800  // Transmit Descriptor Base Address Low
#define E1000_TDBAH          0x3804  // Transmit Descriptor Base Address High
#define E1000_TDLEN          0x3808  // Transmit Descriptor Length
#define E1000_TDH            0x3810  // Transmit Descriptor Head
#define E1000_TDT            0x3818  // Transmit Descriptor Tail
#define E1000_TIDV           0x3820  // Transmit Interrupt Delay Value
#define E1000_TXDCTL         0x3828  // Transmit Descriptor Control
#define E1000_TADV           0x382C  // Transmit Absolute Interrupt Delay Timer
#define E1000_TSPMT          0x3830  // Transmit Small Packet Detect Interrupt
#define E1000_RXCSUM         0x5000  // Receive Checksum Control
#define E1000_RA             0x5400  // Receive Address (MAC address registers)
#define E1000_MTA            0x5200  // Multicast Table Array

// Control Register Bits
#define E1000_CTRL_FD         0x00000001  // Full Duplex
#define E1000_CTRL_LRST       0x00000008  // Link Reset
#define E1000_CTRL_ASDE       0x00000020  // Auto-Speed Detection Enable
#define E1000_CTRL_SLU        0x00000040  // Set Link Up
#define E1000_CTRL_ILOS       0x00000080  // Invert Loss of Signal
#define E1000_CTRL_SPEED_10   0x00000000  // 10 Mbps
#define E1000_CTRL_SPEED_100  0x00000100  // 100 Mbps
#define E1000_CTRL_SPEED_1000 0x00000200  // 1000 Mbps
#define E1000_CTRL_FRCSPD     0x00000800  // Force Speed
#define E1000_CTRL_FRCDPX     0x00001000  // Force Duplex
#define E1000_CTRL_SDP0_DATA  0x00040000  // SDP0 Data
#define E1000_CTRL_SDP1_DATA  0x00080000  // SDP1 Data
#define E1000_CTRL_ADVD3WUC   0x00100000  // Advertise Wake Up Capability
#define E1000_CTRL_EN_PHY_PWR_MGMT 0x00200000  // Enable PHY Power Management
#define E1000_CTRL_SWDPIN0    0x00400000  // Software Definable Pin 0
#define E1000_CTRL_SWDPIN1    0x00800000  // Software Definable Pin 1
#define E1000_CTRL_SWDPIO0    0x40000000  // Software Definable Pin 0 I/O
#define E1000_CTRL_RST        0x04000000  // Device Reset
#define E1000_CTRL_RFCE       0x08000000  // Receive Flow Control Enable
#define E1000_CTRL_TFCE       0x10000000  // Transmit Flow Control Enable
#define E1000_CTRL_VME        0x40000000  // VLAN Mode Enable
#define E1000_CTRL_PHY_RST    0x80000000  // PHY Reset

// Status Register Bits
#define E1000_STATUS_FD       0x00000001  // Full Duplex
#define E1000_STATUS_LU       0x00000002  // Link Up
#define E1000_STATUS_FUNC_MASK 0x0000000C  // Function ID
#define E1000_STATUS_TXOFF    0x00000010  // Transmission Paused
#define E1000_STATUS_TBIMODE  0x00000020  // TBI Mode
#define E1000_STATUS_SPEED_10 0x00000000  // 10 Mbps
#define E1000_STATUS_SPEED_100 0x00000040  // 100 Mbps
#define E1000_STATUS_SPEED_1000 0x00000080  // 1000 Mbps
#define E1000_STATUS_ASDV     0x00000300  // Auto Speed Detection Value
#define E1000_STATUS_MTXCKOK  0x00000400  // MTX Clock Running OK
#define E1000_STATUS_PCI66    0x00000800  // PCI 66 MHz
#define E1000_STATUS_BUS64    0x00001000  // 64-bit Bus
#define E1000_STATUS_PCIX_MODE 0x00002000  // PCI-X Mode
#define E1000_STATUS_PCIX_SPEED 0x0000C000  // PCI-X Speed

// Receive Control Register Bits
#define E1000_RCTL_EN         0x00000002  // Receiver Enable
#define E1000_RCTL_SBP        0x00000004  // Store Bad Packets
#define E1000_RCTL_UPE        0x00000008  // Unicast Promiscuous Enable
#define E1000_RCTL_MPE        0x00000010  // Multicast Promiscuous Enable
#define E1000_RCTL_LPE        0x00000020  // Long Packet Reception Enable
#define E1000_RCTL_LBM_NONE   0x00000000  // No Loopback
#define E1000_RCTL_LBM_PHY    0x00000040  // PHY Loopback
#define E1000_RCTL_LBM_MAC    0x00000080  // MAC Loopback
#define E1000_RCTL_RDMTS_HALF 0x00000000  // Half Descriptor Minimum Threshold Size
#define E1000_RCTL_RDMTS_QUARTER 0x00000100  // Quarter Descriptor Minimum Threshold Size
#define E1000_RCTL_RDMTS_EIGHTH 0x00000200  // Eighth Descriptor Minimum Threshold Size
#define E1000_RCTL_MO_36      0x00000000  // Multicast Offset 47:36
#define E1000_RCTL_MO_35      0x00000400  // Multicast Offset 46:35
#define E1000_RCTL_MO_34      0x00000800  // Multicast Offset 45:34
#define E1000_RCTL_MO_32      0x00000C00  // Multicast Offset 43:32
#define E1000_RCTL_BAM        0x00008000  // Broadcast Accept Mode
#define E1000_RCTL_BSIZE_2048 0x00000000  // 2048 bytes
#define E1000_RCTL_BSIZE_1024 0x00010000  // 1024 bytes
#define E1000_RCTL_BSIZE_512  0x00020000  // 512 bytes
#define E1000_RCTL_BSIZE_256  0x00030000  // 256 bytes
#define E1000_RCTL_VFE        0x00040000  // VLAN Filter Enable
#define E1000_RCTL_CFIEN      0x00080000  // Canonical Form Indicator Enable
#define E1000_RCTL_CFI        0x00100000  // Canonical Form Indicator Bit Value
#define E1000_RCTL_DPF        0x00400000  // Discard Pause Frames
#define E1000_RCTL_PMCF       0x00800000  // Pass MAC Control Frames
#define E1000_RCTL_SECRC      0x04000000  // Strip Ethernet CRC

// Transmit Control Register Bits
#define E1000_TCTL_EN         0x00000002  // Transmit Enable
#define E1000_TCTL_PSP        0x00000008  // Pad Short Packets
#define E1000_TCTL_CT         0x00000FF0  // Collision Threshold
#define E1000_TCTL_COLD       0x003FF000  // Collision Distance
#define E1000_TCTL_SWXOFF     0x00400000  // Software XOFF Transmission
#define E1000_TCTL_PBE        0x00800000  // Packet Burst Enable
#define E1000_TCTL_RTLC       0x01000000  // Re-transmit on Late Collision
#define E1000_TCTL_NRTU       0x02000000  // No Re-transmit on Underrun
#define E1000_TCTL_MULR       0x10000000  // Multiple Request Support

// Interrupt Register Bits
#define E1000_ICR_TXDW        0x00000001  // Transmit Descriptor Written Back
#define E1000_ICR_TXQE        0x00000002  // Transmit Queue Empty
#define E1000_ICR_LSC         0x00000004  // Link Status Change
#define E1000_ICR_RXSEQ       0x00000008  // Receive Sequence Error
#define E1000_ICR_RXDMT0      0x00000010  // Receive Descriptor Minimum Threshold
#define E1000_ICR_RXO         0x00000040  // Receiver Overrun
#define E1000_ICR_RXT0        0x00000080  // Receiver Timer
#define E1000_ICR_MDAC        0x00000200  // MDIO Access Complete
#define E1000_ICR_RXCFG       0x00000400  // Receiver Configuration Change
#define E1000_ICR_GPI_EN0     0x00000800  // GPI 0
#define E1000_ICR_GPI_EN1     0x00001000  // GPI 1
#define E1000_ICR_GPI_EN2     0x00002000  // GPI 2
#define E1000_ICR_GPI_EN3     0x00004000  // GPI 3
#define E1000_ICR_TXDLY       0x00008000  // Transmit Delay Timer
#define E1000_ICR_SRPD        0x00010000  // Small Receive Packet Detection
#define E1000_ICR_ACK         0x00020000  // Receive ACK
#define E1000_ICR_MNG         0x00040000  // Management Event
#define E1000_ICR_DRSTA       0x00080000  // Device Reset Acknowledge
#define E1000_ICR_RDFH        0x00100000  // Receive Descriptor FIFO Head
#define E1000_ICR_RDFT        0x00200000  // Receive Descriptor FIFO Tail
#define E1000_ICR_RDFO        0x00400000  // Receive Descriptor FIFO Overflow
#define E1000_ICR_RDFE        0x00800000  // Receive Descriptor FIFO Empty
#define E1000_ICR_TDFH        0x01000000  // Transmit Descriptor FIFO Head
#define E1000_ICR_TDFT        0x02000000  // Transmit Descriptor FIFO Tail
#define E1000_ICR_TDFE        0x04000000  // Transmit Descriptor FIFO Empty
#define E1000_ICR_TDFO        0x08000000  // Transmit Descriptor FIFO Overflow
#define E1000_ICR_TDFA        0x10000000  // Transmit Descriptor FIFO Almost Empty
#define E1000_ICR_TDFC        0x20000000  // Transmit Descriptor FIFO Control
#define E1000_ICR_PI          0x80000000  // Power Management Interrupt

// EEPROM Read Register Bits
#define E1000_EERD_START      0x00000001  // Start Read
#define E1000_EERD_DONE       0x00000010  // Read Done
#define E1000_EERD_ADDR_SHIFT 8           // Address Shift
#define E1000_EERD_DATA_SHIFT 16          // Data Shift
// 82573/82574 (QEMU e1000e) EERD layout: DONE is bit 1, address starts at bit 2.
#define E1000_EERD_DONE_NEW       0x00000002
#define E1000_EERD_ADDR_SHIFT_NEW 2
#define E1000_RAH_AV              0x80000000  // Receive Address Valid


// Descriptor Control Bits
#define E1000_TXD_CMD_EOP     0x01000000  // End of Packet
#define E1000_TXD_CMD_IFCS    0x02000000  // Insert FCS
#define E1000_TXD_CMD_IC      0x04000000  // Insert Checksum
#define E1000_TXD_CMD_RS      0x08000000  // Report Status
#define E1000_TXD_CMD_RPS     0x10000000  // Report Packet Sent
#define E1000_TXD_CMD_DEXT    0x20000000  // Descriptor Extension
#define E1000_TXD_CMD_VLE     0x40000000  // VLAN Packet Enable
#define E1000_TXD_CMD_IDE     0x80000000  // Interrupt Delay Enable

#define E1000_TXD_STAT_DD     0x00000001  // Descriptor Done
#define E1000_TXD_STAT_EC     0x00000002  // Excess Collisions
#define E1000_TXD_STAT_LC     0x00000004  // Late Collision
#define E1000_TXD_STAT_TU     0x00000008  // Transmit Underrun
#define E1000_TXD_STAT_TC     0x00000004  // Transmit Complete

#define E1000_RXD_STAT_DD     0x00000001  // Descriptor Done
#define E1000_RXD_STAT_EOP    0x00000002  // End of Packet
#define E1000_RXD_STAT_IXSM   0x00000004  // Ignore Checksum Indication
#define E1000_RXD_STAT_VP     0x00000008  // VLAN Packet
#define E1000_RXD_STAT_TCPCS  0x00000020  // TCP Checksum Calculated on Packet
#define E1000_RXD_STAT_IPCS   0x00000040  // IP Checksum Calculated on Packet
#define E1000_RXD_STAT_PIF    0x00000080  // Passed in-exact filter
#define E1000_RXD_STAT_IPIDV  0x00000100  // IP Identification Valid
#define E1000_RXD_STAT_CRCE   0x00004000  // CRC Error
#define E1000_RXD_STAT_IPE    0x00008000  // IP Checksum Error
#define E1000_RXD_STAT_TCPE   0x00010000  // TCP Checksum Error
#define E1000_RXD_STAT_CE     0x00020000  // Checksum Error

// Buffer sizes and counts
#define E1000_NUM_RX_DESC     32
#define E1000_NUM_TX_DESC     32
#define E1000_RX_BUFFER_SIZE  2048
#define E1000_TX_BUFFER_SIZE  2048

// E1000 descriptor structures
typedef struct {
    uint64_t addr;        // Buffer address
    uint16_t length;      // Buffer length
    uint16_t csum;        // Checksum offset
    uint8_t status;       // Status
    uint8_t errors;       // Errors
    uint16_t special;     // Special
} __attribute__((packed)) e1000_rx_desc_t;

typedef struct {
    uint64_t addr;        // Buffer address
    uint16_t length;      // Buffer length
    uint8_t cso;          // Checksum offset
    uint8_t cmd;          // Command
    uint8_t status;       // Status
    uint8_t css;          // Checksum start
    uint16_t special;     // Special
} __attribute__((packed)) e1000_tx_desc_t;

// E1000 device structure
typedef struct {
    struct pci_device *pci_dev;
    uint8_t *io_base;
    uint8_t *rx_buffers[E1000_NUM_RX_DESC];
    uint8_t *tx_buffers[E1000_NUM_TX_DESC];
    e1000_rx_desc_t *rx_descs;
    e1000_tx_desc_t *tx_descs;
    uint32_t rx_cur;
    uint32_t tx_cur;
    uint32_t tx_used;
    uint8_t mac_address[6];
    int initialized;
    int use_io; // 1 for port I/O, 0 for MMIO
    uint8_t ip_address[4]; // Static IPv4 address
} e1000_device_t;

// Function declarations
void e1000_init();
int e1000_send_packet(const uint8_t *data, uint32_t length);
int e1000_receive_packet(uint8_t *buffer, uint32_t max_length);
void e1000_get_mac_address(uint8_t *mac);
int e1000_has_pending_receive();
void e1000_handle_interrupt();
int e1000_register_net_device(void);

// Register access functions
uint8_t e1000_read_reg8(e1000_device_t *dev, uint32_t offset);
uint16_t e1000_read_reg16(e1000_device_t *dev, uint32_t offset);
uint32_t e1000_read_reg32(e1000_device_t *dev, uint32_t offset);
void e1000_write_reg8(e1000_device_t *dev, uint32_t offset, uint8_t value);
void e1000_write_reg16(e1000_device_t *dev, uint32_t offset, uint16_t value);
void e1000_write_reg32(e1000_device_t *dev, uint32_t offset, uint32_t value);

// External device instance
extern e1000_device_t e1000_dev;
