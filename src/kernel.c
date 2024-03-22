#include "types.h"
#include "idt.h"
#include "isr.h"
#include "cpuid.h"
#include "apic.h"
#include "pic.h"

void print(const char* str){
    static unsigned char x = 0, y = 0;
    unsigned short attrib = 0x07;
    volatile unsigned short* vidmem;

    for(int i = 0; str[i] != '\0'; i++) {
        vidmem = (volatile unsigned short*)0xb8000 + (y * 80 + x);
        switch (str[i])
        {
            case '\b':
                vidmem = (volatile unsigned short*)0xb8000 + (y * 80 + x);
                *vidmem = ' ' | (attrib << 8);
                vidmem--;
                *vidmem = '_' | (attrib << 8);
                x--;
                break;
            case '\n':
                *vidmem = ' ' | (attrib << 8);
                y++;
                x = 0;
                break;
            case '\v':
                for(y=0; y<25; y++) {
                    for(x=0; x<80; x++) {
                        vidmem = (volatile unsigned short*)0xb8000 + (y * 80 + x);
                        *vidmem = 0x00;
                    }
                }
                x=0;
                y=0;
                break;
            default:
                *vidmem = str[i] | (attrib << 8);
                x++;
                break;
        }
        if(x >= 80) {
            x = 0;
            y++;
        }
        if (y >= 25) {
			
			unsigned short scroll_temp;

			for (y = 1; y < 25; y++) {	
				for (x = 0; x < 80; x++) {
					
					vidmem = (volatile unsigned short*)0xb8000 + (80*y+x);
					scroll_temp = *vidmem;
						
					vidmem -= 80;
					*vidmem = scroll_temp;
					
					if (y == 24) {
						
						vidmem = (volatile unsigned short*)0xb8000 + (1920+x);
						*vidmem = ' ' | (attrib << 8);
					}
				}
			}
			x = 0;
			y = 24;
		}
    }
}

void printk(const char* str) {
    print("[KERNEL] ");
    print(str);
}

char* int2string(uint32_t value) {
    static char buf[32] = {0};
    int i = 30;
    for(; value && i ; --i, value /= 10) {
        buf[i] = "0123456789"[value % 10];
    }
    return &buf[i+1];
}

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
    print(int2string(cpu_get_apic_base()));
    print("\n");

    printk("Enabling NMIs\n");
    NMI_enable();
    printk("NMIs enabled\n");

    printk("PIC remapping\n");
    pic_remap(0x20, 0x28);
    printk("PIC remapped\n");

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