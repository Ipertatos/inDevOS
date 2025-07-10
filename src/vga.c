#include "vga.h"
#include <stdint.h>
#include "limine.h"

#define VGA_WIDTH 80
#define VGA_HEIGHT 25

static volatile uint16_t *vga_mem = (uint16_t*)0xB8000; // temporary until vga_init remaps

#define VGA_MEMORY vga_mem

static size_t cursor_x = 0;
static size_t cursor_y = 0;
static uint8_t vga_color = 0x07; // light grey on black

static inline uint16_t vga_entry(char c, uint8_t color){
    return (uint16_t)color << 8 | (uint16_t)c;
}

void vga_clear(void){
    for(size_t y=0;y<VGA_HEIGHT;y++){
        for(size_t x=0;x<VGA_WIDTH;x++){
            VGA_MEMORY[y*VGA_WIDTH + x] = vga_entry(' ', vga_color);
        }
    }
    cursor_x = cursor_y = 0;
}

extern volatile struct limine_hhdm_request hhdm_request;

void vga_init(void){
    if(hhdm_request.response){
        uint64_t off = hhdm_request.response->offset;
        vga_mem = (uint16_t*)(off + 0xB8000);
    }
    vga_clear();
}

static void vga_put_entry(char c){
    if(c=='\n'){
        cursor_x = 0;
        cursor_y++;
    } else if(c=='\r'){
        cursor_x = 0;
    } else if(c=='\t'){
        cursor_x = (cursor_x + 4) & ~(size_t)3;
    } else if(c=='\b'){
        if(cursor_x>0){
            cursor_x--;
            VGA_MEMORY[cursor_y*VGA_WIDTH + cursor_x] = vga_entry(' ', vga_color);
        }
    } else {
        VGA_MEMORY[cursor_y*VGA_WIDTH + cursor_x] = vga_entry(c, vga_color);
        cursor_x++;
    }

    if(cursor_x >= VGA_WIDTH){
        cursor_x = 0;
        cursor_y++;
    }
    if(cursor_y >= VGA_HEIGHT){
        // scroll up one line
        for(size_t y=1; y<VGA_HEIGHT; y++){
            for(size_t x=0; x<VGA_WIDTH; x++){
                VGA_MEMORY[(y-1)*VGA_WIDTH + x] = VGA_MEMORY[y*VGA_WIDTH + x];
            }
        }
        // clear last line
        for(size_t x=0; x<VGA_WIDTH; x++){
            VGA_MEMORY[(VGA_HEIGHT-1)*VGA_WIDTH + x] = vga_entry(' ', vga_color);
        }
        cursor_y = VGA_HEIGHT-1;
    }
}

void vga_putchar(char c){
    vga_put_entry(c);
}

void vga_write(const char *buf, size_t len){
    for(size_t i=0;i<len;i++) vga_put_entry(buf[i]);
}

uint16_t *vga_get_buffer(void){
    return (uint16_t*)vga_mem;
} 