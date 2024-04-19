#include "types.h"
#include "idt.h"
#include "cpuid.h"
#include "keyboard.h"
#include "apic.h"
#include "pic.h"
#include "utils.h"
#include "timer.h"
void kmain() {
    printk("Kernel started\n");
    printk("Initializing Interrupts\n");
    
    initIDT();

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

    printk("Total memory: ");
    print(int2string(get_total_memory()));
    print(" bytes\n");

    //initialize keyboard
    printk("Initializing keyboard\n");
    keyboard_init();
    initTimer();

}