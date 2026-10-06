#pragma once

#include "types.h"
#include "pci.h"

// Realtek PCI IDs for RTL810xE family (Fast Ethernet PCIe)
#define RTL810XE_VENDOR_ID 0x10EC
#define RTL810XE_DEVICE_ID 0x8136 // RTL8101E/RTL8102E/RTL8103E

// Minimal device context
typedef struct {
	struct pci_device *pci_dev;
	uint8_t *io_base;
	int use_io; // 1 = port I/O, 0 = MMIO
	int initialized;
} rtl810x_device_t;

// Public API
void rtl810x_init();
void rtl810x_handle_interrupt();

// Global instance
extern rtl810x_device_t rtl810x_dev;
















