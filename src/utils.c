#include "utils.h"

struct limine_framebuffer *fb;

void initFB(){
    fb = fb_rq.response->framebuffers[0];
}

void print(const char* str){
    static unsigned char x = 0, y = 0;
    unsigned short attrib = 0x07;
    volatile unsigned short* vidmem;
    uint32_t *fb_ptr = fb->address; 

    for(int i = 0; str[i] != '\0'; i++) {
        vidmem = fb_ptr + (y * 80 + x);
        switch (str[i])
        {
            case '\b':
                vidmem = fb_ptr + (y * 80 + x);
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
                        vidmem = fb_ptr + (y * 80 + x);
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
					
					vidmem = fb_ptr + (80*y+x);
					scroll_temp = *vidmem;
						
					vidmem -= 80;
					*vidmem = scroll_temp;
					
					if (y == 24) {
						
						vidmem = fb_ptr + (1920+x);
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

uint32_t strlen(const char* str) {
    uint32_t len = 0;
    while(str[len] != '\0') len++;
    return len;
}

bool strEql(const char* str1, const char* str2) {
    if(strlen(str1) != strlen(str2)) return false;
    for(int i = 0; i < strlen(str1); i++) {
        if(str1[i] != str2[i]) return false;
    }
    return true;
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

void *memset(void *dest, int val, uint32_t count)
{
    uint8_t *p = (uint8_t *)dest;
    for(uint32_t i = 0; i < count; i++)
        p[i] = (uint8_t)val;

    return dest;
}

void *memcpy(void *dest, const void *src, uint32_t count)
{
    uint8_t *pdest = (uint8_t *)dest;
    const uint8_t *psrc = (const uint8_t *)src;
    for(uint32_t i = 0; i < count; i++)
        pdest[i] = psrc[i];
    
    return dest;
}

void *memmove(void *dest, const void *src, uint32_t count)
{
    uint8_t *pdest = (uint8_t *)dest;
    const uint8_t *psrc = (const uint8_t *)src;
    if(pdest < psrc)
    {
        for(uint32_t i = 0; i < count; i++)
            pdest[i] = psrc[i];
    }
    else
    {
        for(uint32_t i = count; i != 0; i--)
            pdest[i-1] = psrc[i-1];
    }
    return dest;
}

int memcmp(const void *a, const void *b, uint32_t count)
{
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    for(uint32_t i = 0; i < count; i++)
    {
        if(pa[i] != pb[i])
            return pa[i] < pb[i] ? -1 : 1;
    }
    return 0;
}

void call(char* buffer, uint32_t buff_len)
{

    if(strEql(buffer, "ping"))
    {
        print("pong\n");
    }
    else if (strEql(buffer, "clear")) {
        print("\v");
    }
    if(!strEql(buffer,""))
        print("supreme@IpertatOS:~$ ");
    else
        print("\n");
}