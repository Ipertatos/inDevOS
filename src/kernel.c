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

#include "pmm.h"
#include "vmm.h"

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
    initFB();

    //printk("Initializing Interrupts\n");
    

    //printk("IDT initialized\n");
    //printk("Checking APIC\n");

    fillrect(0x0000ff,0,0,32*3,32);
    pmm_init();
    fillrect(0x00ff00,0,32,32,32);
    init_acpi();
    fillrect(0x00ff00,32,32,32,32);
    vmm_init();
    fillrect(0x00ff00,64,32,32,32);
    //if(check_apic()) printk("APIC found\n"); 
    //else             printk("APIC not found\n");



    //printk("Enabling APIC\n");
    //printk("APIC enabled\n");



    //printk("APIC Base: ");
    //print("0x");
    //print(int2hex(cpu_get_apic_base()));
    //print("\n");

    //printk("Total memory: ");
    //print(int2string(get_ram_size()));
    //print(" \n");

    //initialize keyboard
    //printk("Initializing keyboard\n");

    keyboard_init();


    initTimer();


    while(1) {
        __asm__("nop");
    }

}