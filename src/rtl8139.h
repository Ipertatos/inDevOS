#pragma once

#include "types.h"
#include "pci.h"

// RTL8139 PCI Vendor and Device IDs
#define RTL8139_VENDOR_ID    0x10EC
#define RTL8139_DEVICE_ID    0x8139

// RTL8139 Register Offsets
#define RTL8139_MAC          0x00    // MAC address (6 bytes)
#define RTL8139_MAR          0x08    // Multicast address (8 bytes)
#define RTL8139_TSD0         0x10    // Transmit status descriptor 0
#define RTL8139_TSD1         0x14    // Transmit status descriptor 1
#define RTL8139_TSD2         0x18    // Transmit status descriptor 2
#define RTL8139_TSD3         0x1C    // Transmit status descriptor 3
#define RTL8139_TSAD0        0x20    // Transmit start address 0
#define RTL8139_TSAD1        0x24    // Transmit start address 1
#define RTL8139_TSAD2        0x28    // Transmit start address 2
#define RTL8139_TSAD3        0x2C    // Transmit start address 3
#define RTL8139_RBSTART      0x30    // Receive buffer start address
#define RTL8139_CMD          0x37    // Command register
#define RTL8139_CAPR         0x38    // Current address of packet read
#define RTL8139_CBR          0x3A    // Current buffer address (2 bytes)
#define RTL8139_IMR          0x3C    // Interrupt mask register
#define RTL8139_ISR          0x3E    // Interrupt status register
#define RTL8139_CONFIG1      0x40    // Configuration register 1
#define RTL8139_CONFIG3      0x43    // Configuration register 3
#define RTL8139_CONFIG4      0x44    // Configuration register 4 (RCR - Receive Configuration Register)
#define RTL8139_CONFIG1_POWER 0x52   // CONFIG_1 register for power-on (0x52)
#define RTL8139_HLTCLK       0x54    // HLTCLK register
#define RTL8139_BMCR         0x62    // Basic mode control register
#define RTL8139_BMSR         0x64    // Basic mode status register
#define RTL8139_ANAR         0x66    // Auto-negotiation advertisement register
#define RTL8139_ANLPAR       0x68    // Auto-negotiation link partner ability register
#define RTL8139_ANER         0x6A    // Auto-negotiation expansion register
#define RTL8139_DIS          0x6C    // Disconnect counter
#define RTL8139_FCSC         0x6E    // False carrier sense counter
#define RTL8139_NWAYTR       0x70    // NWAY test register
#define RTL8139_RXERR        0x72    // Receive error counter
#define RTL8139_CSCR         0x74    // CS configuration register
#define RTL8139_PHY1         0x78    // PHY register 1
#define RTL8139_TW           0x7C    // Transmit window
#define RTL8139_PHY2         0x80    // PHY register 2
#define RTL8139_WOLCR        0x84    // Wake-on-LAN control register
#define RTL8139_MPC          0x86    // Missed packet counter
#define RTL8139_9346CR       0x50    // 93C46 command register

// Command Register Bits
#define RTL8139_CMD_RST      0x10    // Reset
#define RTL8139_CMD_RE       0x08    // Receiver enable
#define RTL8139_CMD_TE       0x04    // Transmitter enable
#define RTL8139_CMD_BUFE     0x02    // Buffer empty
#define RTL8139_CMD_RXON     0x01    // Receiver on

// RCR (Receive Configuration Register) Bits
#define RTL8139_RCR_AAP      0x01    // Accept All Packets (promiscuous mode)
#define RTL8139_RCR_APM      0x02    // Accept Physical Match
#define RTL8139_RCR_AM       0x04    // Accept Multicast
#define RTL8139_RCR_AB       0x08    // Accept Broadcast
#define RTL8139_RCR_WRAP     0x80    // Wrap mode (efficient buffer handling)
// Additional RCR helper masks
// MXDMA (bits 8-10): Maximum DMA burst size; 0b111 = unlimited
#define RTL8139_RCR_MXDMA_UNL   (7 << 8)
// RBLEN (bits 11-12): Receive Buffer Length base (00=8K, 01=16K, 10=32K, 11=64K)
#define RTL8139_RCR_RBLEN_8K    (0 << 11)

// Interrupt Status Register Bits
#define RTL8139_ISR_ROK      0x0001  // Receive OK
#define RTL8139_ISR_RER      0x0002  // Receive error
#define RTL8139_ISR_TOK      0x0004  // Transmit OK
#define RTL8139_ISR_TER      0x0008  // Transmit error
#define RTL8139_ISR_RXOVW    0x0010  // Receive buffer overflow
#define RTL8139_ISR_PUN      0x0020  // Packet underrun/link change
#define RTL8139_ISR_FOVW     0x0040  // FIFO overflow
#define RTL8139_ISR_TSERR    0x0080  // Time stamp error
#define RTL8139_ISR_LENCHG   0x0100  // Cable length change
#define RTL8139_ISR_TIMEOUT  0x0200  // Time out
#define RTL8139_ISR_SERR     0x0400  // System error
#define RTL8139_ISR_PCSTIMEOUT 0x0800 // PCI clock run timeout
#define RTL8139_ISR_SWI      0x1000  // Software interrupt
#define RTL8139_ISR_MDP      0x2000  // Media delivery priority
#define RTL8139_ISR_MDPINT   0x4000  // Media delivery priority interrupt
#define RTL8139_ISR_RXFOVW   0x8000  // Receive FIFO overflow

// Transmit Status Descriptor Bits
#define RTL8139_TSD_OWN      0x2000  // Owner bit
#define RTL8139_TSD_TUN      0x4000  // Transmit underrun (Bit 14, not 0x0400)
#define RTL8139_TSD_TOK      0x8000  // Transmit OK (Bit 15, not 0x0200)
#define RTL8139_TSD_ERTXTH_MASK 0x3F0000 // Early Tx Threshold (Bits 16-21)
#define RTL8139_TSD_SIZE_MASK 0x1FFF // Size mask (0-12)

// Receive Status Bits
#define RTL8139_RXSTAT_ROK   0x0001  // Receive OK
#define RTL8139_RXSTAT_FAE   0x0002  // Frame alignment error
#define RTL8139_RXSTAT_CRC   0x0004  // CRC error
#define RTL8139_RXSTAT_LONG  0x0008  // Long packet
#define RTL8139_RXSTAT_RUNT  0x0010  // Runt packet
#define RTL8139_RXSTAT_ISE   0x0020  // Invalid symbol error
#define RTL8139_RXSTAT_MAR   0x8000  // Multicast address received

// Buffer sizes (OSDev wiki recommendations)
#define RTL8139_RX_BUFFER_SIZE 8192  // Use 8K buffer WITH WRAP mode (total allocation: 8K+16+1500)
#define RTL8139_TX_BUFFER_SIZE 1518
#define RTL8139_NUM_TX_BUFFERS 4  // Use all 4 transmit buffers like TwistedOS

// RTL8139 device structure
typedef struct {
    struct pci_device *pci_dev;
    uint8_t *io_base;
    volatile uint8_t *rx_buffer;
    uint8_t *tx_buffers[RTL8139_NUM_TX_BUFFERS];
    uint32_t current_packet_ptr;
    uint32_t tx_cur;
    uint8_t mac_address[6];
    int initialized;
    int use_io; // 1 for port I/O, 0 for MMIO
    uint8_t ip_address[4]; // Static IPv4 address
    volatile int rx_pending_flag; // Software flag for pending receive interrupts
} rtl8139_device_t;

// 93C46 EEPROM access bits (used in 9346CR register at offset 0x50)
#define RTL8139_EEPROM_MODE_NORMAL   0x00  // Normal mode (bits 7:6 = 00)
#define RTL8139_EEPROM_MODE_AUTOLOAD 0x40  // Auto-load (bits 7:6 = 01)
#define RTL8139_EEPROM_MODE_PROGRAM  0x80  // 93C46 programming (bits 7:6 = 10)
#define RTL8139_EEPROM_MODE_CONFIG   0xC0  // Config register write enable (bits 7:6 = 11)
#define RTL8139_EEPROM_EECS         0x08   // Chip Select
#define RTL8139_EEPROM_EESK         0x04   // Clock
#define RTL8139_EEPROM_EEDI         0x02   // Data In (host -> EEPROM)
#define RTL8139_EEPROM_EEDO         0x01   // Data Out (EEPROM -> host)

// EEPROM word addresses for MAC on RTL8139
#define RTL8139_EEPROM_MAC_WORD0     7     // MAC bytes 0-1
#define RTL8139_EEPROM_MAC_WORD1     8     // MAC bytes 2-3
#define RTL8139_EEPROM_MAC_WORD2     9     // MAC bytes 4-5

// Function declarations
void rtl8139_init(void);
int rtl8139_send_packet(const uint8_t *data, uint32_t length);
int rtl8139_receive_packet(uint8_t *buffer, uint32_t max_length);
void rtl8139_get_mac_address(uint8_t *mac);
int rtl8139_has_pending_receive(void);
void rtl8139_handle_interrupt(void);
int rtl8139_register_net_device(void);

// Read a 16-bit word from the 93C46 EEPROM
uint16_t rtl8139_eeprom_read(rtl8139_device_t *dev, uint8_t address);
// Read MAC address from EEPROM (returns 1 on success, 0 on failure)
int rtl8139_read_mac_from_eeprom(rtl8139_device_t *dev, uint8_t mac[6]);

// Register access functions
uint8_t rtl8139_read_reg8(rtl8139_device_t *dev, uint32_t offset);
uint16_t rtl8139_read_reg16(rtl8139_device_t *dev, uint32_t offset);
uint32_t rtl8139_read_reg32(rtl8139_device_t *dev, uint32_t offset);
void rtl8139_write_reg8(rtl8139_device_t *dev, uint32_t offset, uint8_t value);
void rtl8139_write_reg16(rtl8139_device_t *dev, uint32_t offset, uint16_t value);
void rtl8139_write_reg32(rtl8139_device_t *dev, uint32_t offset, uint32_t value);

// External device instance
extern rtl8139_device_t rtl8139_dev;

// Internal functions
static void rtl8139_reset(rtl8139_device_t *dev);
static void rtl8139_setup_rx_buffer(rtl8139_device_t *dev);
static void rtl8139_configure_rx_buffer(rtl8139_device_t *dev);
static void rtl8139_setup_tx_buffers(rtl8139_device_t *dev);
static void rtl8139_set_mac_address(rtl8139_device_t *dev, const uint8_t *mac);
static void rtl8139_enable_interrupts(rtl8139_device_t *dev);
static void rtl8139_disable_interrupts(rtl8139_device_t *dev); 