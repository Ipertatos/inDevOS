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


static volatile LIMINE_BASE_REVISION(1);


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
    __asm__("sti");
    while(1) {
        __asm__("nop");
    }

}