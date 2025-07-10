#include "../utils.h"
#include "../keyboard.h"
#include "../vga.h"
#include "matrix.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

// Simple linear congruential generator for pseudo-random numbers
static uint32_t lcg_state = 123456789;
static uint32_t lcg_rand(void) {
    lcg_state = lcg_state * 1103515245u + 12345u;
    return (lcg_state >> 16) & 0x7FFFu;
}

#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define TRAIL_LEN 8

// VGA color codes
#define COLOR_GREEN 0x02
#define COLOR_WHITE 0x0F
#define COLOR_BLACK 0x00

static inline uint16_t make_entry(char c, uint8_t color){
    return (uint16_t)color << 8 | (uint16_t)c;
}

// Column state arrays
static int head_y[VGA_WIDTH];
static bool active[VGA_WIDTH];
static int drop_len[VGA_WIDTH];

// Double buffers
static uint16_t bufA[VGA_HEIGHT][VGA_WIDTH];
static uint16_t bufB[VGA_HEIGHT][VGA_WIDTH];
static bool use_A = true;

void matrix_rain(void){
    keyboard_clear_buffer();
    vga_clear();

    for(int x=0;x<VGA_WIDTH;x++){ active[x]=false; head_y[x]=0; drop_len[x]=0; }

    while(1){
        // Select back buffer
        uint16_t (*back)[VGA_WIDTH] = use_A ? bufA : bufB;

        // Clear back buffer to black spaces
        for(int y=0;y<VGA_HEIGHT;y++){
            for(int x=0;x<VGA_WIDTH;x++) back[y][x] = make_entry(' ', COLOR_BLACK);
        }

        // Update drops and draw into back buffer
        for(int x=0;x<VGA_WIDTH;x++){
            if(!active[x]){
                if((lcg_rand() & 0x3FFu) < 20){
                    active[x]=true;
                    head_y[x]=0;
                    drop_len[x]=TRAIL_LEN + (lcg_rand() % (VGA_HEIGHT/2));
                }
            }
            if(active[x]){
                for(int i=0;i<TRAIL_LEN;i++){
                    int y = head_y[x]-i;
                    if(y<0 || y>=VGA_HEIGHT) continue;
                    char ch = (char)(33 + (lcg_rand()%94));
                    uint8_t col = (i==0)? COLOR_WHITE : COLOR_GREEN;
                    back[y][x] = make_entry(ch, col);
                }
                head_y[x]++;
                if(head_y[x] >= drop_len[x]) active[x]=false;
            }
        }

        // Blit back buffer to VGA memory
        uint16_t *vga = vga_get_buffer();
        memcpy(vga, back, sizeof(uint16_t)*VGA_WIDTH*VGA_HEIGHT);

        wait(1); // 1 ms delay

        // Swap buffers
        use_A = !use_A;

        // Exit on space
        uint32_t len = keyboard_get_buffer_length();
        if(len){
            char *bufp = keyboard_get_buffer();
            for(uint32_t i=0;i<len;i++) if(bufp[i]==' '){ keyboard_clear_buffer(); vga_clear(); return; }
            keyboard_clear_buffer();
        }
    }
} 