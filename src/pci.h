#pragma once

#include "types.h"

#define pci_reg0_offset 0x00 // device id and vendor id
#define pci_reg1_offset 0x04 // status and command
#define pci_reg2_offset 0x08 // class code , subclass , prog if , rev id
#define pci_reg3_offset 0x0C // BIST , header type , latency timer , cache line size
#define pci_reg4_offset 0x10 // BAR0
#define pci_reg5_offset 0x14 // BAR1

struct pci_device {
    uint8_t class_code;
    uint8_t subclass_code;
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint8_t prog_if;
    const char* description;
    uint64_t address;
    uint32_t header_type;
};

extern struct pci_device *pci_devices[];
extern uint8_t pci_device_count;

void pci_init();
struct pci_device* pci_find_device(uint8_t class_code, uint8_t subclass_code);

uint32_t pci_device_read(struct pci_device *dev, uint32_t offset, uint8_t size);
void pci_device_write(struct pci_device *dev, uint32_t offset, uint8_t size, uint32_t val);