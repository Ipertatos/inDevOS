#include "idt.h"

void printex(const char* str){
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

char* int2strings(uint32_t value) {
    static char buf[32] = {0};
    int i = 30;
    for(; value && i ; --i, value /= 10) {
        buf[i] = "0123456789"[value % 10];
    }
    return &buf[i+1];
}
void exception_handler(int i) {
    printex("Exception");
    printex(int2strings(i));
    printex("\n");

    __asm__ volatile ("cli; hlt"); // Completely hangs the computer
}

void idt_set_descriptor(uint8_t vector, void* isr, uint8_t flags) {
    idt_entry_t* descriptor = &idt[vector];
 
    descriptor->isr_low        = (uint64_t)isr & 0xFFFF;
    descriptor->kernel_cs      = GDT_OFFSET_KERNEL_CODE;
    descriptor->ist            = 0;
    descriptor->attributes     = flags;
    descriptor->isr_mid        = ((uint64_t)isr >> 16) & 0xFFFF;
    descriptor->isr_high       = ((uint64_t)isr >> 32) & 0xFFFFFFFF;
    descriptor->reserved       = 0;
}

void idt_init() {
    idtr.base = (uintptr_t)&idt[0];
    idtr.limit = (uint16_t)sizeof(idt_entry_t) * 256 - 1;
 
    __asm__ volatile ("lidtq %0" : : "m"(idtr)); // load the new IDT
    __asm__ volatile ("sti"); // set the interrupt flag
}