#include "pci.h"
#include "nvme.h"

void nvme_init(){
    struct pci_device* dev = pci_find_device(0x01, 0x08);
    if(dev == NULL){
        printf("NVMe: Device not found.{n}");
        return;
    }
    printf("NVME Header Type : 0x{X}{n}", dev->header_type);
    uint32_t bar0 = pci_device_read(dev, 0x10, 4);
    pci_device_write(dev, 0x10, 4, ~0);
    uint32_t size_low = pci_device_read(dev, 0x10, 4);
    pci_device_write(dev, 0x10, 4, bar0);

    if(bar0 & 1){
        printf("NVMe: Using IO Space{n}");
    } else {
        printf("NVMe: Using Memory Space{n}");
    }

    if((bar0 & 0b111) == 0b100){
        printf("NVMe: 64-bit addressing{n}");
    }

    uint32_t base_high = pci_device_read(dev, 0x14, 4);
    printf("NVMe: Base High: 0x{x}{n}", base_high);

    uint32_t base_low = bar0 & 0xfffffff0;
    uint64_t bar0_base_addr = base_low;
    bar0_base_addr |= ((uint64_t)base_high) << 32;

    uint32_t len = ~(size_low & ~0b1111) + 1;
    printf("NVMe: Length: 0x{x}{n}", len);

    printf("NVMe: Base Low: 0x{x}{n}", base_low);

    printf("NVMe: Initialized{n}");
}