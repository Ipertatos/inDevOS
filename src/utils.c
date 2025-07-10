#include "term.h"
#include "hpet.h"
#include <stdarg.h>
#include "types.h"
#include "limine.h"
#include "mem.h"
#include "serial.h"
#include "atomic.h"
#include <string.h>
#include <stdio.h>  // For sprintf
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#define MAX_INT_SIZE 128

// Flanterm protection - simple spinlock
static volatile int flanterm_lock = 0;
static volatile int interrupt_context = 0;
// Global flag to allow disabling flanterm permanently at runtime
volatile int flanterm_enabled = 1;

// Simple atomic test-and-set for flanterm lock
static inline int flanterm_trylock(void) {
    int old = __sync_val_compare_and_swap(&flanterm_lock, 0, 1);
    return old == 0;  // Returns 1 if we got the lock, 0 if already locked
}

static inline void flanterm_unlock(void) {
    __sync_lock_release(&flanterm_lock);
}

// Functions to track interrupt context
void enter_interrupt_context(void) {
    __sync_fetch_and_add(&interrupt_context, 1);
}

void exit_interrupt_context(void) {
    __sync_fetch_and_sub(&interrupt_context, 1);
}

int in_interrupt_context(void) {
    return interrupt_context > 0;
}

#define FORMAT_LENTGH       1
#define NORMAL              0
#define STATE_SHORT         2
#define STATE_LONG          3
#define FORMAT_SPECIFIER    1
#define PERCENT_FORMAT      2

#define LENGTH_DEFAULT      0
#define LENGTH_SHORT_SHORT  1
#define LENGTH_SHORT        2
#define LENGTH_LONG         3
#define LENGTH_LONG_LONG    4


volatile struct limine_framebuffer_request fb_rq = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0
};

struct limine_framebuffer *fb;


/* flanterm framebuffer code removed */

/* legacy custom printf removed */
#if 0
int printf(const char *format, ...) { return 0; }
#endif

void putpixel(uint64_t x, uint64_t y, uint32_t color){
    if (!fb) return;
    
    // Bounds checking
    if (x >= fb->width || y >= fb->height) return;
    
    // Calculate pixel position based on framebuffer format
    if (fb->bpp == 32) {
        volatile uint32_t *screen = (uint32_t*)fb->address;
        uint32_t pixels_per_line = fb->pitch / 4;
        screen[y * pixels_per_line + x] = color;
    } else if (fb->bpp == 24) {
        volatile uint8_t *screen = (uint8_t*)fb->address;
        uint64_t offset = y * fb->pitch + x * 3;
        screen[offset] = color & 0xFF;         // Blue
        screen[offset + 1] = (color >> 8) & 0xFF;  // Green  
        screen[offset + 2] = (color >> 16) & 0xFF; // Red
    } else {
        // Fallback for other formats - assume 32-bit
        volatile uint32_t *screen = (uint32_t*)fb->address;
        screen[y * (fb->pitch / 4) + x] = color;
    }
}

void fillrect(uint32_t color, uint64_t x, uint64_t y, uint64_t w, uint64_t h){
    if (!fb) return;
    
    // Bounds checking and clipping
    if (x >= fb->width || y >= fb->height) return;
    if (x + w > fb->width) w = fb->width - x;
    if (y + h > fb->height) h = fb->height - y;
    
    if (fb->bpp == 32) {
        volatile uint32_t *screen = (uint32_t*)fb->address;
        uint32_t pixels_per_line = fb->pitch / 4;
        
        for (uint64_t j = 0; j < h; j++) {
            for (uint64_t i = 0; i < w; i++) {
                screen[(y + j) * pixels_per_line + (x + i)] = color;
            }
        }
    } else {
        // Fallback to putpixel for other formats
        for (uint64_t j = 0; j < h; j++) {
            for (uint64_t i = 0; i < w; i++) {
                putpixel(x + i, y + j, color);
            }
        }
    }
}

uint32_t get_pixel_color(uint8_t pixel){
    uint32_t color = pixel;
    color |= pixel << 8;
    color |= pixel << 16;
    return color;
}

void printch(void* ctx,char c){ (void)ctx; term_putchar(c);}

void printstr(void* ctx,char *str){ (void)ctx; if(str) term_write(str,strlen(str)); }

void printk(void* ctx, char* str){ (void)ctx; term_write("[KERNEL] ",9); term_write(str,strlen(str)); }

void printex(void* ctx, char* str){ (void)ctx; term_write("[EXCEPTION] ",12); term_write(str,strlen(str)); }

/* binary/int/hex helpers obsolete with kprintf */

void log_panic(char *msg){
    printf("%skern_panic: %s%s\n", ANSI_COLOR_RED, ANSI_COLOR_RESET, msg);
    __asm__ volatile ("cli"); 
    __asm__ volatile ("hlt");
}

void log_err(char *msg){
    printf("%skern_err: %s%s\n", ANSI_COLOR_RED, ANSI_COLOR_RESET, msg);

}

void log_warn(char *msg){
    printf("%skern_warn: %s%s\n", ANSI_COLOR_YELLOW, ANSI_COLOR_RESET, msg);
}

void log_info(char *msg){
    printf("%skern_info: %s%s\n", ANSI_COLOR_CYAN, ANSI_COLOR_RESET, msg);
}

void log_success(char *msg){
    printf("%skern_success: %s%s\n", ANSI_COLOR_GREEN, ANSI_COLOR_RESET, msg);
}

char* int2string(uint32_t value) {
    static char buf[32] = {0};
    int i = 30;
    if (value == 0) {
        buf[30] = '0';
        return &buf[30];
    }
    for(; value && i ; --i, value /= 10) {
        buf[i] = "0123456789"[value % 10];
    }
    return &buf[i+1];
}

char* int2hex(uint64_t value) {
    static char buf[64] = {0};
    int i = 62;
    if (value == 0) {
        buf[62] = '0';
        return &buf[62];
    }
    for(; value && i ; --i, value /= 16) {
        buf[i] = "0123456789ABCDEF"[value % 16];
    }
    return &buf[i+1];
}


char* strtok(char* str, const char* delim) {
    static char* pos = NULL;
    if (str) pos = str;           // Reset static position when new string passed
    if (!pos) return NULL;

    /* Skip any leading delimiters so we don’t return empty tokens */
    pos += strspn(pos, delim);
    if (*pos == '\0') {
        return NULL;              // Reached end after skipping delimiters
    }

    /* Find length until next delimiter */
    size_t len = strcspn(pos, delim);
    char *token = pos;
    pos += len;
    if (*pos != '\0') {
        *pos++ = '\0';           // Null-terminate token and advance past delimiter
    }
    return token;
}

/* Use compiler built-ins from <string.h> instead of local versions */

char* remove_nl(char* str){
    if (str == NULL) {
        return NULL;
    }
    uint32_t str_len = strlen(str);
    for(uint32_t i = 0; i < str_len; i++) {
        if(str[i] == '\n'){
            str[i] = '\0';
        }
    }
    return str;
}

bool strEql(const char* str1, const char* str2) {
    if (str1 == NULL || str2 == NULL) {
        return (str1 == str2); // Both NULL = equal, one NULL = not equal
    }
    uint32_t str1_len = strlen(str1);
    uint32_t str2_len = strlen(str2);
    if (str1_len != str2_len) {
        return false;
    }
    for(uint32_t i = 0; i < str1_len; i++) {
        if(str1[i] != str2[i]) {
            return false;
        }
    }
    return true;
}

void dump_hex(char *stack) {
    for (int i = 0; i < 64; i++) {
        printf("%02x ", (unsigned char)stack[i]);
    }
    printf("\n");
}

void dump_str(char *stack) {
    int i = 0;
    while (i < 64) {
        printf("%s", &stack[i]);
        while (i < 64 && stack[i]) i++;
        i++;
    }
    printf("\n");
}

void wait(uint64_t ms){
    uint64_t curticks = hpet_get_ticks();
    while(hpet_get_ticks() - curticks < ms){
        asm("nop");
    }
}
// Stack protection function
void __stack_chk_fail(void) {
    log_panic("Stack smashing detected");
}

void clear_screen(){
    // Use framebuffer console clear instead of ANSI escape sequences
    fb_console_clear();
}

// VGA backend: no flanterm context needed

/* Framebuffer Console Implementation */

// Simple 8x8 font data (minimal set for basic characters)
static const uint8_t font_8x8[128][8] = {
    // ASCII 32 (space) to 127 (~)
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, // space
    {0x00, 0x00, 0x5F, 0x00, 0x00, 0x00, 0x00, 0x00}, // !
    {0x00, 0x00, 0x07, 0x00, 0x07, 0x00, 0x00, 0x00}, // "
    {0x14, 0x7F, 0x14, 0x7F, 0x14, 0x00, 0x00, 0x00}, // #
    {0x24, 0x2A, 0x7F, 0x2A, 0x12, 0x00, 0x00, 0x00}, // $
    {0x23, 0x13, 0x08, 0x64, 0x62, 0x00, 0x00, 0x00}, // %
    {0x36, 0x49, 0x55, 0x22, 0x50, 0x00, 0x00, 0x00}, // &
    {0x00, 0x05, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00}, // '
    {0x00, 0x1C, 0x22, 0x41, 0x00, 0x00, 0x00, 0x00}, // (
    {0x00, 0x41, 0x22, 0x1C, 0x00, 0x00, 0x00, 0x00}, // )
    {0x08, 0x2A, 0x1C, 0x2A, 0x08, 0x00, 0x00, 0x00}, // *
    {0x08, 0x08, 0x3E, 0x08, 0x08, 0x00, 0x00, 0x00}, // +
    {0x00, 0x50, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00}, // ,
    {0x08, 0x08, 0x08, 0x08, 0x08, 0x00, 0x00, 0x00}, // -
    {0x00, 0x60, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00}, // .
    {0x20, 0x10, 0x08, 0x04, 0x02, 0x00, 0x00, 0x00}, // /
    {0x3E, 0x51, 0x49, 0x45, 0x3E, 0x00, 0x00, 0x00}, // 0
    {0x00, 0x42, 0x7F, 0x40, 0x00, 0x00, 0x00, 0x00}, // 1
    {0x42, 0x61, 0x51, 0x49, 0x46, 0x00, 0x00, 0x00}, // 2
    {0x21, 0x41, 0x45, 0x4B, 0x31, 0x00, 0x00, 0x00}, // 3
    {0x18, 0x14, 0x12, 0x7F, 0x10, 0x00, 0x00, 0x00}, // 4
    {0x27, 0x45, 0x45, 0x45, 0x39, 0x00, 0x00, 0x00}, // 5
    {0x3C, 0x4A, 0x49, 0x49, 0x30, 0x00, 0x00, 0x00}, // 6
    {0x01, 0x71, 0x09, 0x05, 0x03, 0x00, 0x00, 0x00}, // 7
    {0x36, 0x49, 0x49, 0x49, 0x36, 0x00, 0x00, 0x00}, // 8
    {0x06, 0x49, 0x49, 0x29, 0x1E, 0x00, 0x00, 0x00}, // 9
    {0x00, 0x36, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00}, // :
    {0x00, 0x56, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00}, // ;
    {0x00, 0x08, 0x14, 0x22, 0x41, 0x00, 0x00, 0x00}, // <
    {0x14, 0x14, 0x14, 0x14, 0x14, 0x00, 0x00, 0x00}, // =
    {0x41, 0x22, 0x14, 0x08, 0x00, 0x00, 0x00, 0x00}, // >
    {0x02, 0x01, 0x51, 0x09, 0x06, 0x00, 0x00, 0x00}, // ?
    {0x32, 0x49, 0x79, 0x41, 0x3E, 0x00, 0x00, 0x00}, // @
    {0x7E, 0x11, 0x11, 0x11, 0x7E, 0x00, 0x00, 0x00}, // A
    {0x7F, 0x49, 0x49, 0x49, 0x36, 0x00, 0x00, 0x00}, // B
    {0x3E, 0x41, 0x41, 0x41, 0x22, 0x00, 0x00, 0x00}, // C
    {0x7F, 0x41, 0x41, 0x22, 0x1C, 0x00, 0x00, 0x00}, // D
    {0x7F, 0x49, 0x49, 0x49, 0x41, 0x00, 0x00, 0x00}, // E
    {0x7F, 0x09, 0x09, 0x01, 0x01, 0x00, 0x00, 0x00}, // F
    {0x3E, 0x41, 0x41, 0x51, 0x32, 0x00, 0x00, 0x00}, // G
    {0x7F, 0x08, 0x08, 0x08, 0x7F, 0x00, 0x00, 0x00}, // H
    {0x00, 0x41, 0x7F, 0x41, 0x00, 0x00, 0x00, 0x00}, // I
    {0x20, 0x40, 0x41, 0x3F, 0x01, 0x00, 0x00, 0x00}, // J
    {0x7F, 0x08, 0x14, 0x22, 0x41, 0x00, 0x00, 0x00}, // K
    {0x7F, 0x40, 0x40, 0x40, 0x40, 0x00, 0x00, 0x00}, // L
    {0x7F, 0x02, 0x04, 0x02, 0x7F, 0x00, 0x00, 0x00}, // M
    {0x7F, 0x04, 0x08, 0x10, 0x7F, 0x00, 0x00, 0x00}, // N
    {0x3E, 0x41, 0x41, 0x41, 0x3E, 0x00, 0x00, 0x00}, // O
    {0x7F, 0x09, 0x09, 0x09, 0x06, 0x00, 0x00, 0x00}, // P
    {0x3E, 0x41, 0x51, 0x21, 0x5E, 0x00, 0x00, 0x00}, // Q
    {0x7F, 0x09, 0x19, 0x29, 0x46, 0x00, 0x00, 0x00}, // R
    {0x46, 0x49, 0x49, 0x49, 0x31, 0x00, 0x00, 0x00}, // S
    {0x01, 0x01, 0x7F, 0x01, 0x01, 0x00, 0x00, 0x00}, // T
    {0x3F, 0x40, 0x40, 0x40, 0x3F, 0x00, 0x00, 0x00}, // U
    {0x1F, 0x20, 0x40, 0x20, 0x1F, 0x00, 0x00, 0x00}, // V
    {0x7F, 0x20, 0x18, 0x20, 0x7F, 0x00, 0x00, 0x00}, // W
    {0x63, 0x14, 0x08, 0x14, 0x63, 0x00, 0x00, 0x00}, // X
    {0x03, 0x04, 0x78, 0x04, 0x03, 0x00, 0x00, 0x00}, // Y
    {0x61, 0x51, 0x49, 0x45, 0x43, 0x00, 0x00, 0x00}, // Z
    {0x00, 0x08, 0x36, 0x41, 0x00, 0x00, 0x00, 0x00}, // {
    {0x00, 0x00, 0x7F, 0x00, 0x00, 0x00, 0x00, 0x00}, // |
    {0x00, 0x41, 0x36, 0x08, 0x00, 0x00, 0x00, 0x00}, // }
    {0x08, 0x08, 0x2A, 0x1C, 0x08, 0x00, 0x00, 0x00}, // ~
    // Add more characters if needed
};

// after the existing font_8x8 definition add a dedicated lowercase font table (row-wise encoded)
static const uint8_t font_8x8_lower[26][8] = {
    /* a */ {0x00,0x00,0x3C,0x06,0x3E,0x66,0x3E,0x00},
    /* b */ {0x60,0x60,0x7C,0x66,0x66,0x66,0x7C,0x00},
    /* c */ {0x00,0x00,0x3C,0x60,0x60,0x60,0x3C,0x00},
    /* d */ {0x06,0x06,0x3E,0x66,0x66,0x66,0x3E,0x00},
    /* e */ {0x00,0x00,0x3C,0x66,0x7E,0x60,0x3C,0x00},
    /* f */ {0x1C,0x36,0x30,0x7C,0x30,0x30,0x78,0x00},
    /* g */ {0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x7C},
    /* h */ {0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0x00},
    /* i */ {0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00},
    /* j */ {0x06,0x00,0x06,0x06,0x06,0x06,0x66,0x3C},
    /* k */ {0x60,0x60,0x66,0x6C,0x78,0x6C,0x66,0x00},
    /* l */ {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00},
    /* m */ {0x00,0x00,0x6C,0xFE,0xFE,0xD6,0xD6,0x00},
    /* n */ {0x00,0x00,0x7C,0x66,0x66,0x66,0x66,0x00},
    /* o */ {0x00,0x00,0x3C,0x66,0x66,0x66,0x3C,0x00},
    /* p */ {0x00,0x00,0x7C,0x66,0x66,0x7C,0x60,0x60},
    /* q */ {0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x06},
    /* r */ {0x00,0x00,0x6E,0x70,0x60,0x60,0xF0,0x00},
    /* s */ {0x00,0x00,0x3E,0x60,0x3C,0x06,0x7C,0x00},
    /* t */ {0x30,0x30,0x7C,0x30,0x30,0x36,0x1C,0x00},
    /* u */ {0x00,0x00,0x66,0x66,0x66,0x66,0x3E,0x00},
    /* v */ {0x00,0x00,0x66,0x66,0x66,0x3C,0x18,0x00},
    /* w */ {0x00,0x00,0xC6,0xD6,0xFE,0xFE,0x6C,0x00},
    /* x */ {0x00,0x00,0x66,0x3C,0x18,0x3C,0x66,0x00},
    /* y */ {0x00,0x00,0x66,0x66,0x66,0x3E,0x06,0x7C},
    /* z */ {0x00,0x00,0x7E,0x0C,0x18,0x30,0x7E,0x00}
};

// Pre-computed byte-reverse LUT (populated at runtime)
// (bit reversal no longer needed)

// Console dimensions and state
static uint32_t console_x = 0;
static uint32_t console_y = 0;
static uint32_t console_width = 80;  // Default characters per line
static uint32_t console_height = 25; // Default lines
static const uint32_t char_base = 8; // base glyph size in font table
static uint32_t font_scale = 1;      // 1 = 8x8, 2 = 16x16 etc.
static uint32_t char_width  = 8;
static uint32_t char_height = 8;

static void recalc_console_dims(){
    char_width  = char_base * font_scale;
    char_height = char_base * font_scale;
    if(fb){
        console_width  = fb->width  / char_width;
        console_height = fb->height / char_height;
    }
}

uint32_t console_get_width(void){
    return console_width;
}

uint32_t console_get_height(void){
    return console_height;
}

void fb_set_font_scale(uint32_t scale){
    if(scale == 0 || scale > 4) return;
    font_scale = scale;
    recalc_console_dims();
    fb_console_clear();
    console_x = console_y = 0;
}

static uint32_t fg_color = 0xFFFFFF; // White
static uint32_t bg_color = 0x000000; // Black

void console_set_fg(uint32_t color){
    fg_color = color;
}

uint32_t console_get_fg(void){
    return fg_color;
}

// Test function to verify framebuffer works
void fb_test_pattern(void) {
    if (!fb) {
        serial_puts("[fb_test] No framebuffer available\n");
        return;
    }
    
    serial_puts("[fb_test] Starting simple pixel test\n");
    
    // Test just a few individual pixels
    putpixel(0, 0, 0xFF0000);      // Red pixel at top-left
    putpixel(1, 0, 0x00FF00);      // Green pixel 
    putpixel(2, 0, 0x0000FF);      // Blue pixel
    putpixel(3, 0, 0xFFFFFF);      // White pixel
    
    serial_puts("[fb_test] Individual pixels set\n");
    
    // Test a small 10x10 rectangle
    for(int y = 10; y < 20; y++) {
        for(int x = 10; x < 20; x++) {
            putpixel(x, y, 0xFFFF00);  // Yellow square
        }
    }
    
    serial_puts("[fb_test] Small rectangle drawn\n");
    serial_puts("[fb_test] Basic pixel test completed\n");
}

void fb_console_init(void) {
    if (!fb) {
        serial_puts("[fb_console] Framebuffer not available!\n");
        return;
    }

    
    // Log actual framebuffer details for debugging
    serial_printf("[fb_console] FB address: 0x%p\n", fb->address);
    serial_printf("[fb_console] FB dimensions: %dx%d\n", (uint32_t)fb->width, (uint32_t)fb->height);
    serial_printf("[fb_console] FB pitch: %d, bpp: %d\n", (uint32_t)fb->pitch, fb->bpp);
    
    recalc_console_dims();
    
    // Initialize with clean black screen
    fb_console_clear();
    console_x = 0;
    console_y = 0;
    
    serial_printf("[fb_console] Console chars: %dx%d\n", console_width, console_height);
    serial_puts("[fb_console] Initialized framebuffer console.\n");
}

void fb_console_clear(void) {
    if (!fb) return;
    fillrect(bg_color, 0, 0, fb->width, fb->height);
    console_x = 0;
    console_y = 0;
}

static bool cursor_visible = false;
static uint32_t blink_counter = 0;
#define CURSOR_BLINK_INTERVAL 60

static void fb_draw_cursor(bool show){
    if(!fb) return;
    uint32_t start_x = console_x * char_width;
    uint32_t start_y = console_y * char_height;
    uint32_t cursor_color = fg_color;
    if(show){
        // inverse video: swap fg/bg for block bottom line
        fillrect(cursor_color, start_x, start_y + char_height-1, char_width, 1);
    }else{
        fillrect(bg_color, start_x, start_y + char_height-1, char_width, 1);
    }
}

void fb_cursor_blink_tick(){
    if(++blink_counter < CURSOR_BLINK_INTERVAL) return;
    blink_counter = 0;
    cursor_visible = !cursor_visible;
    fb_draw_cursor(cursor_visible);
}

void fb_console_putchar(char c) {
    if (!fb) return;

    // remove old cursor before modifying text
    if(cursor_visible){ fb_draw_cursor(false); cursor_visible=false; }

    if (c == '\n') {
        console_x = 0;
        console_y++;
    } else if (c == '\r') {
        console_x = 0;
    } else if (c == '\b') {
        if (console_x > 0) {
            console_x--;
            // Clear the character area
            uint32_t start_x = console_x * char_width;
            uint32_t start_y = console_y * char_height;
            fillrect(bg_color, start_x, start_y, char_width, char_height);
        }
    } else if (c >= 32 && c <= 126) {  // Printable ASCII characters
        unsigned char ch = (unsigned char)c;

        uint32_t start_x = console_x * char_width;
        uint32_t start_y = console_y * char_height;

        if(ch >= 'a' && ch <= 'z') {
            // Draw lowercase glyphs (stored row-wise in font_8x8_lower)
            const uint8_t *row_bitmap = font_8x8_lower[ch - 'a'];
            for(uint32_t row = 0; row < char_base; row++){
                uint8_t bits = row_bitmap[row];
                for(uint32_t col = 0; col < char_base; col++){
                    bool on = bits & (0x80 >> col); // MSB = left-most pixel
                    for(uint32_t dx = 0; dx < font_scale; dx++){
                        for(uint32_t dy = 0; dy < font_scale; dy++){
                            uint32_t pixel_x = start_x + col*font_scale + dx;
                            uint32_t pixel_y = start_y + row*font_scale + dy;
                            if(pixel_x >= fb->width || pixel_y >= fb->height) continue;
                            putpixel(pixel_x, pixel_y, on ? fg_color : bg_color);
                        }
                    }
                }
            }
        } else {
            // Draw existing glyphs (column-wise in font_8x8)
            const uint8_t *char_bitmap = font_8x8[ch - 32];
            for (uint32_t col = 0; col < char_base; col++) {
                uint8_t col_byte = char_bitmap[col];
                for (uint32_t row = 0; row < char_base; row++) {
                    bool on = col_byte & (1 << row);
                    for(uint32_t dx=0; dx<font_scale; dx++){
                        for(uint32_t dy=0; dy<font_scale; dy++){
                            uint32_t pixel_x = start_x + col*font_scale + dx;
                            uint32_t pixel_y = start_y + row*font_scale + dy;
                            if (pixel_x >= fb->width || pixel_y >= fb->height) continue;
                            putpixel(pixel_x, pixel_y, on ? fg_color : bg_color);
                        }
                    }
                }
            }
        }
        console_x++;
    } else if (c > 0) {  // Handle other characters with a simple block
        uint32_t start_x = console_x * char_width;
        uint32_t start_y = console_y * char_height;
        
        // Draw a simple hollow rectangle for unknown characters
        for (uint32_t y = 0; y < char_height; y++) {
            for (uint32_t x = 0; x < char_width; x++) {
                uint32_t pixel_x = start_x + x;
                uint32_t pixel_y = start_y + y;
                
                if (pixel_x >= fb->width || pixel_y >= fb->height) {
                    continue;
                }
                
                // Draw border for unknown characters
                if (x == 0 || x == char_width-1 || y == 0 || y == char_height-1) {
                    putpixel(pixel_x, pixel_y, fg_color);
                } else {
                    putpixel(pixel_x, pixel_y, bg_color);
                }
            }
        }
        console_x++;
    }

    // Handle line wrapping
    if (console_x >= console_width) {
        console_x = 0;
        console_y++;
    }

    // Handle scrolling
    if (console_y >= console_height) {
        fb_console_scroll();
        console_y = console_height - 1;
    }
    // draw cursor after processing
    fb_draw_cursor(true);
    cursor_visible=true;
}

void fb_console_scroll(void) {
    if (!fb) return;
    
    // Move all lines up by one character height
    uint32_t bytes_per_pixel = fb->bpp / 8;
    uint32_t pixels_per_line = fb->pitch / bytes_per_pixel;
    
    for (uint32_t y = char_height; y < fb->height; y++) {
        for (uint32_t x = 0; x < fb->width; x++) {
            uint32_t *src = (uint32_t*)fb->address + (y * pixels_per_line) + x;
            uint32_t *dst = (uint32_t*)fb->address + ((y - char_height) * pixels_per_line) + x;
            *dst = *src;
        }
    }
    
    // Clear the last line
    uint32_t last_line_y = (console_height - 1) * char_height;
    fillrect(bg_color, 0, last_line_y, fb->width, char_height);
}

void fb_console_write(const char *str, size_t len) {
    if (!fb) {
        // Fallback to serial-only if framebuffer not available
        return;
    }
    for (size_t i = 0; i < len; i++) {
        fb_console_putchar(str[i]);
    }
}

// Debug framebuffer request during initialization
void debug_framebuffer_request(void) {
    if(fb_rq.response && fb_rq.response->framebuffer_count > 0){
        fb = fb_rq.response->framebuffers[0];
        serial_printf("[utils] Framebuffer available. Address=0x%p\n", fb->address);
        serial_printf("[utils] Framebuffer dimensions: width=%u height=%u\n", fb->width, fb->height);
    } else {
        serial_puts("[utils] Framebuffer request failed or no framebuffers available.\n");
        serial_printf("[utils] Debug: fb_rq.response = %p\n", fb_rq.response);
        if (fb_rq.response) {
            serial_printf("[utils] Debug: framebuffer_count = %llu\n", fb_rq.response->framebuffer_count);
        } else {
            serial_puts("[utils] Debug: No response from framebuffer request.\n");
        }
        fb = NULL;
    }
}
