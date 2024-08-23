#include "pci.h"
#include "acpi.h"
#include "mem.h"

struct pci_device *pci_devices[32];
uint8_t pci_device_count = 0;

// Get device string representation of the specified class code and subclass code
const char* get_device_str(uint8_t class_code, uint8_t sub_class_code){
    switch(class_code){
        case 0x01:
            switch(sub_class_code){
                case 0x06:
                    return "Mass Storage Controller - Serial ATA Controller";
                case 0x08:
                    return "Mass Storage Controller - Non-Volatile Memory Controller";
                default:
                    printf("PCI: {d}:{d} not found{n}", class_code, sub_class_code);
                    return "Unknown Device";
            }
        case 0x02:
            switch (sub_class_code){
                case 0x00:
                    printf("PCI: {x}:{xn}", class_code, sub_class_code);
                    return "Network Controller - Ethernet Controller";
                default:
                    printf("PCI: {d}:{d} not found{n}", class_code, sub_class_code);
                    return "Unknown Device";
            }
        case 0x03:
            switch (sub_class_code){
                case 0x00:
                    return "Display Controller - VGA Compatible Controller";
                default:
                    printf("PCI: {d}:{d} not found{n}", class_code, sub_class_code);
                    return "Unknown Device";
            }
        case 0x06:
            switch(sub_class_code){
                case 0x00:
                    return "Bridge - Host Bridge";
                case 0x01:
                    return "Bridge - ISA Bridge";
                default:
                    printf("PCI: {d}:{d} not found{n}", class_code, sub_class_code);
                    return "Unknown Device";
            }
        case 0x0C:
            switch(sub_class_code){
                case 0x05:
                    return "Serial Bus Controller - USB Controller";
                default:
                    printf("PCI: {d}:{d} not found{n}", class_code, sub_class_code);
                    return "Unknown Device";
            }
    }
    printf("PCI: {d}:{d} not found{n}", class_code, sub_class_code);
    return "Unknown Device";
}

// read from the pci configuration mm space
uint32_t pci_mm_read(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint8_t size){
    uint64_t addr = mcfg->base_addr + vmm_higher_half_offset + offset + 
    ((bus - mcfg->start_bus) << 20) + (device << 15) + (function << 12);

    switch (size)
    {
    case 1:
        return *(uint8_t*)addr;
    case 2:
        return *(uint16_t*)addr;
    case 4:
        return *(uint32_t*)addr;
    }
    return 0;
}

// read from pci configuration for a pci device
uint32_t pci_device_read(struct pci_device *dev, uint32_t offset, uint8_t size){
    return pci_mm_read(dev->bus, dev->device, dev->function, offset, size);
}

//write to pci configuration for a pci device
void pci_device_write(struct pci_device *dev, uint32_t offset, uint8_t size, uint32_t val)
{
    uint64_t addr = mcfg->base_addr + vmm_higher_half_offset + offset + 
    ((dev->bus - mcfg->start_bus) << 20) + (dev->device << 15) + (dev->function << 12);

    switch (size)
    {
    case 1:
        *(uint8_t*)addr = (uint8_t)val;
        return;
    case 2:
        *(uint16_t*)addr = (uint16_t)val;
        return;
    case 4:
        *(uint32_t*)addr = (uint32_t)val;
        return;
    }
}

uint16_t pci_mm_read_vendor_id(uint8_t bus, uint8_t device, uint8_t function)
{
    uint16_t reg0 = pci_mm_read(bus, device, function, 0x0, 2);
    return reg0;
}

uint8_t pci_mm_read_header_type(uint8_t bus, uint8_t device, uint8_t function)
{   
    uint32_t reg3 = pci_mm_read(bus, device, function, 0xC, 4);
    return (uint8_t)(reg3 >> 16);
}

/*
 * Reads from PCI MM config for a devices class code.
*/
uint8_t pci_mm_read_class_code(uint8_t bus, uint8_t device, uint8_t function)
{
    uint32_t reg2 = pci_mm_read(bus, device, function, 0x8, 4);
    uint8_t classcode = (uint8_t)(reg2 >> 24);

    return classcode;
}

/*
 * Reads from PCI MM config for a devices sub-class code.
*/
uint8_t pci_mm_read_subclass_code(uint8_t bus, uint8_t device, uint8_t function)
{
    uint32_t reg2 = pci_mm_read(bus, device, function, 0x8, 4);
    uint8_t subclass = (uint8_t)(reg2 >> 16);
    return subclass;
}

/*
 * Reads from PCI MM config for a devices prog IF.
*/
uint8_t pci_mm_read_prog_if(uint8_t bus, uint8_t device, uint8_t function)
{
    uint32_t reg2 = pci_mm_read(bus, device, function, 0x8, 4);
    uint8_t progif = (uint8_t)(reg2 >> 8);
    return progif;
}

/*
 * Inspects a found device at the specific PCI bus, device, function location.
*/
void check_function(uint8_t bus, uint8_t device, uint8_t function, uint16_t header_type)
{
    uint8_t classcode = pci_mm_read_class_code(bus, device, function);
    uint8_t subclass = pci_mm_read_subclass_code(bus, device, function);
    uint8_t progif = pci_mm_read_prog_if(bus, device, function);
    // Create the new device and store it in the device list.
    struct pci_device *dev = kalloc(sizeof(struct pci_device));//kalloc(sizeof(struct pci_device));
    pci_devices[pci_device_count++] = dev;
    dev->class_code = classcode;
    dev->subclass_code = subclass;
    dev->prog_if = progif;
    dev->description = get_device_str(classcode, subclass);
    dev->bus = bus;
    dev->function = function;
    dev->device = device;
    dev->header_type = header_type;
    dev->address = mcfg->base_addr + vmm_higher_half_offset + ((bus - mcfg->start_bus) << 20 | device << 15 | function << 12);

    printf("PCI: {sn}", dev->description);
}

/*
 * Checks if a device is present at the specified bus and device. 
*/
void check_device(uint8_t bus, uint8_t device)
{
    uint8_t function = 0;

    uint16_t vendorID = pci_mm_read_vendor_id(bus, device, function);
    if (vendorID == 0xFFFF) {
        return;
    }

    uint16_t headerType = pci_mm_read_header_type(bus, device, function);

    check_function(bus, device, function, headerType);

    if ((headerType & 0x80) != 0) {
        // It's a multi-function device, so check remaining functions.
        for (function = 1; function < 8; function++) {
            if (pci_mm_read_vendor_id(bus, device, function) != 0xFFFF) {
                check_function(bus, device, function, headerType);
            }
        }
    }
}

/*
 * Scans every device ID on every bus ID looking for present devices.
*/
void scan_all_buses()
{
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t device = 0; device < 32; device++) {
            check_device(bus, device);
        }
    }
}

void pci_init()
{
    scan_all_buses();
    printf("PCI: Found {d} PCI devices.{n}", pci_device_count);
    printf("PCI: Initialized.{n}");
}

/*
 * Finds a device by its class and sub-class code.
*/
struct pci_device* pci_find_device(uint8_t class, uint8_t subclass)
{
    for (int i = 0; i < pci_device_count; i++) {
        struct pci_device *dev = pci_devices[i];
        if (dev->class_code == class && dev->subclass_code == subclass) {
            return dev;
        }
    }

    return NULL;
}