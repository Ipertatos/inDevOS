#include "types.h"
#include "idt.h"
#include "cpuid.h"
#include "keyboard.h"
#include "apic.h"
#include "pic.h"
#include "utils.h"
#include "timer.h"

struct multiboot_header {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t num;
    uint32_t size;
    uint32_t addr;
    uint32_t shndx;
    uint32_t mmap_length;
    uint32_t mmap_addr;
    uint32_t drives_length;
    uint32_t drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
    uint32_t vbe_control_info;
    uint32_t vbe_mode_info;
    uint32_t vbe_mode;
    uint32_t vbe_interface_seg;
    uint32_t vbe_interface_off;
    uint32_t vbe_interface_len;
} __attribute__((section(".multiboot")));

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
    //print(int2string(get_ram_size()));
    print(" \n");

    //initialize keyboard
    printk("Initializing keyboard\n");

    keyboard_init();


    initTimer();


    while(1) {
        __asm__("nop");
    }

}