#include "types.h"
#include "idt.h"
#include "cpuid.h"
#include "keyboard.h"
#include "apic.h"
#include "pic.h"
#include "utils.h"
#include "timer.h"
#include "limine.h"

static volatile LIMINE_BASE_REVISION(1);

void kmain(void) {
    //printk("Kernel started\n");

    if(LIMINE_BASE_REVISION_SUPPORTED == false) {
        //printk("Limine revision not supported\n");
        return;
    }

    //printk("Limine revision supported\n");

    if(fb_rq.response == NULL
    || fb_rq.response->framebuffer_count < 1){
        //printk("No framebuffer found\n");
        return;
    }

    initFB();

    printk("Initializing Interrupts\n");
    
  /*  initIDT();

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
    //print(int2string(get_ram_size()));
    print(" \n");

    //initialize keyboard
    printk("Initializing keyboard\n");

    keyboard_init();


    initTimer();


    while(1) {
        __asm__("nop");
    }*/

}