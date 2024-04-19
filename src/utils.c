#include "utils.h"

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

void printch(char c) {
    char str[2] = {c, '\0'};
    print(str);
}

void printk(const char* str) {
    print("[KERNEL] ");
    print(str);
}

void printex(const char* str) {
    print("[EXCEPTION] ");
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

char* int2hex(uint32_t value) {
    static char buf[32] = {0};
    int i = 30;
    for(; value && i ; --i, value /= 16) {
        buf[i] = "0123456789ABCDEF"[value % 16];
    }
    return &buf[i+1];
}

void dump_hex(char *stack) {
            int x = 0;
    for (int i = 0; i < 64; i++) {
        x++;
        char* hex = int2hex((unsigned char)stack[i]);
        print(hex[0] == '\0' ? "00" : hex);
        if (x == 8) {
            print("\n");
            x = 0;
        } else {
            print(" ");
        }
    }
    print("\n");
}

void dump_str(char *stack) {
    int i = 0;
    while (i < 64) {
        print(&stack[i]);
        while (i < 64 && stack[i]) i++;
        i++;
    }
    print("\n");
}

void memset(void *dest, char val, uint32_t count)
{
    char *temp = (char *)dest;
    for( ; count != 0; count--) *temp++ = val;
}

//get RAM size of the system
uint32_t get_total_memory() {
    uint32_t mem = 0;
    uint32_t* mem_ptr = (uint32_t*)0x8000;
    while(*mem_ptr != 0) {
        mem += *mem_ptr;
        mem_ptr++;
    }
    return mem;
}