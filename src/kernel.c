#include "types.h"
#include "idt.h"
#include "cpuid.h"
#include "keyboard.h"
#include "apic.h"
#include "pic.h"
#include "utils.h"
#include "timer.h"
#include "gdt.h"
#include "acpi.h"
#include "hpet.h"
#include "mem.h"
#include "cpu.h"    
#include "pci.h"    
#include "nvme.h"
#include "sound.h"
#include "paratrooper.h"
static volatile LIMINE_BASE_REVISION(1);
uint8_t buffer_len;

void kmain(void) {
    //printk("Kernel started\n");

    if(LIMINE_BASE_REVISION_SUPPORTED == false) {
        //printk("Limine revision not supported\n");
        return;
    }

    gdt_init();
    initIDT();
    

    //printk("Limine revision supported\n");
    ft_ctx = initFB();

    printk(ft_ctx,"Initializing Interrupts\n");
    

    printk(ft_ctx,"IDT initialized\n");
    printk(ft_ctx,"Checking APIC\n");

    cpu_init();
    kmem_init();
    init_acpi();
    pci_init();

    nvme_init();   
    //beep();
    PlayWAV(44100, fsize, file);
    __asm__("sti");
    while(1) {
        if(execute) {
            cmd(buffer,buffer_len);
            memset(&buffer,'\0',sizeof(char)*256);
            buffer_len = 0;
            execute = true;
        }
        __asm__("nop");
    }

}