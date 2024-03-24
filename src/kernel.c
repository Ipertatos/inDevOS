#include "types.h"
#include "idt.h"
#include "isr.h"
#include "cpuid.h"
#include "keyboard.h"
#include "apic.h"
#include "pic.h"
#include "utils.h"

void kmain() {
    printk("Kernel started\n");
    printk("Initializing IDT\n");
    isr_install();
    idt_init();
    printk("IDT initialized\n");
    printk("Checking APIC\n");

    if(check_apic()) printk("APIC found\n"); 
    else             printk("APIC not found\n");

    printk("Enabling APIC\n");
    enable_apic();
    printk("APIC enabled\n");

    printk("APIC Base: ");
    print("0x");
    print(int2hex(cpu_get_apic_base()));
    print("\n");

    printk("Enabling NMIs\n");
    NMI_enable();
    printk("NMIs enabled\n");

    printk("PIC remapping\n");
    pic_remap(0x20, 0x28);
    printk("PIC remapped\n");

    printk("Total memory: ");
    print(int2string(get_total_memory()));
    print(" bytes\n");

    //initialize keyboard
    printk("Initializing keyboard\n");
    if(keyboard_init() == 0)    printk("Keyboard initialized\n");
    else                        printk("Keyboard initialization failed\n");  
    while (true)
    {
        keyboard_handler();
        //USE NOPS to make a delay
        for (size_t i = 0; i < 37000000; i++)
        {
            __asm__ volatile("nop");
        }
        
    }
    

}