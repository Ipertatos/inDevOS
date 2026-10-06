#include "rtl810x.h"
#include "serial.h"
#include "pci.h"
#include "apic.h"
#include "idt.h"
#include "string.h"

rtl810x_device_t rtl810x_dev = {0};

// Very small bring-up: detect, enable bus mastering, map BAR0, route IRQ
void rtl810x_init() {
	// Guard against double init (driver_manager + legacy kernel.c bringup).
	if (rtl810x_dev.initialized) {
		serial_printf("RTL810xE: Already initialized, skipping\n");
		return;
	}
	serial_printf("RTL810xE: Initializing driver\n");

	// Find device 10ec:8136 on scanned PCI devices
	struct pci_device *pci_dev = NULL;
	for (int i = 0; i < pci_device_count; i++) {
		struct pci_device *dev = pci_devices[i];
		if (dev->vendor_id == RTL810XE_VENDOR_ID && dev->device_id == RTL810XE_DEVICE_ID) {
			pci_dev = dev;
			break;
		}
	}
	if (!pci_dev) {
		serial_printf("RTL810xE: Device not found\n");
		return;
	}

	serial_printf("RTL810xE: Found device at bus %d, device %d, function %d\n",
	             pci_dev->bus, pci_dev->device, pci_dev->function);

	// Enable memory space + bus mastering
	uint16_t cmd = pci_device_read(pci_dev, 0x04, 2);
	cmd |= 0x06;
	pci_device_write(pci_dev, 0x04, 2, cmd);
	uint16_t cmd_after = pci_device_read(pci_dev, 0x04, 2);
	serial_printf("RTL810xE: PCI CMD after = 0x%04x\n", cmd_after);

	// BAR0
	uint32_t bar0 = pci_device_read(pci_dev, 0x10, 4);
	uint32_t base_addr;
	int use_io = 0;
	if (bar0 & 1) {
		base_addr = bar0 & ~3u;
		use_io = 1;
		serial_printf("RTL810xE: Using I/O space at 0x%x\n", base_addr);
	} else {
		base_addr = bar0 & ~0xFu;
		serial_printf("RTL810xE: Using MMIO space at 0x%x\n", base_addr);
	}

	rtl810x_dev.pci_dev = pci_dev;
	rtl810x_dev.io_base = (uint8_t*)(uint64_t)base_addr;
	rtl810x_dev.use_io = use_io;

	// Read PCI interrupt line (legacy INTx); prefer IOAPIC routing like RTL8139
	uint8_t int_line = pci_device_read(pci_dev, 0x3C, 1);
	serial_printf("RTL810xE: Interrupt line: %u\n", int_line);

	// Route legacy IRQ to an IDT vector reserved for this driver (use 46)
	uint32_t lapic_id = 0;
	ioapic_redirect_irq(lapic_id, 46, int_line, true);
	serial_printf("RTL810xE: Configured interrupt routing: IRQ %u -> vector 46\n", int_line);

	rtl810x_dev.initialized = 0;
	serial_printf("RTL810xE: PCI probe only — MAC/RX/TX not implemented\n");
}

void rtl810x_handle_interrupt() {
	// Acknowledge and EOI; full ISR handling not implemented yet
	serial_printf("RTL810xE: IRQ handled (stub)\n");
	apic_eoi();
}
















