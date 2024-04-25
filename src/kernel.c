#include "types.h"
#include "idt.h"
#include "cpuid.h"
#include "keyboard.h"
#include "apic.h"
#include "pic.h"
#include "utils.h"
#include "timer.h"

void wait(uint64_t nops) {
    uint64_t i =0;
    while(i++ < nops*115) __asm__("nop");
}

void kmain() {
    printk("Kernel started\n");
    printk("Initializing Interrupts\n");
    
    initIDT();
    //I HAVE NO FUCKING IDEA 
    wait(100000);
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
        wait(8000);

    initTimer();





    while(1) {
        __asm__("nop");
    }

}