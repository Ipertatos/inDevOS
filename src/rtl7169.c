#include "rtl8169.h"
#include "pci.h"
#include "registers.h"

//offset size name
// 0x00 6 mac0-5 
// 0x08 8 mar0-7
// 0x30 4 rbstart
// 0x37 1 cmd
// 0x3c 2 imr
// 0x3e 2 isr

uint64_t rx_buffer;

void rtl8169_init(){
    struct pci_device *dev = pci_find_device(0x02,0x00);
    printf("RTL8139: Checking for RTL8139 device.\n");
    printf("{x}:{xn}",pci_device_read(dev,pci_reg0_offset,2),pci_device_read(dev,pci_reg0_offset+2,2));
    if(pci_device_read(dev,pci_reg0_offset,2) == RTL8169_VENDOR_ID && pci_device_read(dev,pci_reg0_offset+2,2) == RTL8169_DEVICE_ID){
        printf("{d}:{d}", dev->bus, dev->device);
    }
    else{
        printf("RTL8139: RTL8139 device not found.\n");
        return 0;
    }
    //enable the device
    outb(dev->address + 0x52, 0x0);
    
    //reset the device
    outb(dev->address + 0x37, 0x10);
    while((inb(dev->address + 0x37) & 0x10) != 0);

    outb(dev->address + 0x30, (uint32_t)rx_buffer);

}