#define FLANTERM_IN_FLANTERM
#include "utils.h"
#include "flanterm/src/flanterm.h"
#include "flanterm/src/flanterm_backends/fb.h"

#include "hpet.h"
#include <stdarg.h>
#include "types.h"
#include "limine.h"
#include "mem.h"
#include "serial.h"
#define MAX_INT_SIZE 128

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
struct flanterm_context *ft_ctx = NULL;


struct flanterm_context *initFB(){
    // Debug: Try to output to serial or some other method if available
    // For now, we'll assume this might work even without display
    
    if(fb_rq.response == NULL) {
        // No framebuffer response from bootloader
        return NULL;
    }
    
    if(fb_rq.response->framebuffer_count < 1) {
        // No framebuffers available
        return NULL;
    }
    
    fb = fb_rq.response->framebuffers[0];
    
    // Validate framebuffer parameters
    if(fb->address == NULL || fb->width == 0 || fb->height == 0) {
        return NULL;
    }
    
    // More validation
    if(fb->pitch == 0 || fb->bpp < 24) {
        return NULL;
    }
    
    // Clear framebuffer first to ensure clean state
    volatile uint32_t *framebuffer = (uint32_t*)fb->address;
    for(size_t i = 0; i < (fb->height * fb->pitch) / sizeof(uint32_t); i++) {
        framebuffer[i] = 0x00000000; // Black background
    }
    
    ft_ctx = flanterm_fb_init(
        NULL, NULL, // Use bump allocator (_malloc and _free both NULL)
        fb->address, fb->width, fb->height, fb->pitch,
        fb->red_mask_size, fb->red_mask_shift,
        fb->green_mask_size, fb->green_mask_shift,
        fb->blue_mask_size, fb->blue_mask_shift,
        NULL, // canvas - NULL for no canvas
        NULL, NULL, // ansi_colours, ansi_bright_colours - NULL for defaults
        NULL, NULL, // default_bg, default_fg - NULL for defaults
        NULL, NULL, // default_bg_bright, default_fg_bright - NULL for defaults
        NULL, 0, 0, 1, // font, font_width, font_height, font_spacing - NULL font uses default
        0, 0, // font_scale_x, font_scale_y - 0 for auto-scale
        0 // margin
    );
    
    // Test if framebuffer context was created
    if(ft_ctx != NULL) {
        // Based on Codeberg issue #14: ensure proper initialization
        // Disable autoflush initially to prevent issues during setup
        flanterm_set_autoflush(ft_ctx, false);
        
        // Send a simple test message with proper string handling
        const char test_msg[] = "inDevOS Terminal Ready\n";
        flanterm_write(ft_ctx, test_msg, sizeof(test_msg) - 1); // -1 to exclude null terminator
        flanterm_flush(ft_ctx);
        
        // Re-enable autoflush
        flanterm_set_autoflush(ft_ctx, true);
    }
    
    return ft_ctx;
}

int printf(const char *format, ...) {
    extern struct flanterm_context *ft_ctx;
    if(ft_ctx == NULL) {
        // Fallback: Write directly to VGA text mode buffer at 0xB8000
        // This is a basic fallback for when flanterm is not available
        return -1; // For now, just return -1 to avoid hanging
    }
    
    int state = NORMAL;
    va_list args;
    va_start(args, format);
    uint32_t format_len = strlen(format);
    
    for(uint32_t i = 0; i < format_len; i++){
        char cur = format[i];
        switch (state){
        case NORMAL:
            switch (cur){
            case '{':
                state = FORMAT_SPECIFIER;
                break;
            case '%':
                state = PERCENT_FORMAT;
                break;
            default:
                printch(ft_ctx, cur);
                break;
            }
            break;
        case FORMAT_SPECIFIER: // Handle {format} style
            switch (cur){
            case 'n':
                printch(ft_ctx, '\n');
                break;
            case 'k':
                printstr(ft_ctx, va_arg(args, char*));
                break;
            case 'd':
            case 'i':
                printint(ft_ctx, va_arg(args, long long));
                break;
            case 's':
                {
                    char *str = va_arg(args, char*);
                    if(str != NULL) {
                        printstr(ft_ctx, str);
                    }
                }
                break;
            case 'c':
                ;
                char c = va_arg(args, int);
                printch(ft_ctx, c);
                break;
            case 'x':
                printhex(ft_ctx, va_arg(args, uint64_t));
                break;
            case 'l':
                i++; // Skip to next character
                if(i < format_len) {
                    cur = format[i];
                    switch (cur) {
                    case 'd':
                        printint(ft_ctx, va_arg(args, long));
                        break;
                    case 'u':
                        printint(ft_ctx, va_arg(args, unsigned long));
                        break;
                    case 'x':
                        printhex(ft_ctx, va_arg(args, unsigned long));
                        break;
                    }
                }
                break;
            case '}':
                state = NORMAL;
                break;
            }
            break;
        case PERCENT_FORMAT: // Handle %format style
            switch (cur){
            case 'd':
                printint(ft_ctx, va_arg(args, int));
                state = NORMAL;
                break;
            case 's':
                {
                    char *str = va_arg(args, char*);
                    if(str != NULL) {
                        printstr(ft_ctx, str);
                    }
                }
                state = NORMAL;
                break;
            case 'c':
                ;
                char c2 = va_arg(args, int);
                printch(ft_ctx, c2);
                state = NORMAL;
                break;
            case 'x':
            case 'X':
                printhex(ft_ctx, va_arg(args, unsigned int));
                state = NORMAL;
                break;
            case 'u':
                printint(ft_ctx, va_arg(args, unsigned int));
                state = NORMAL;
                break;
            case 'l':
                i++; // Skip to next character
                if(i < format_len) {
                    cur = format[i];
                    switch (cur) {
                    case 'd':
                        printint(ft_ctx, va_arg(args, long));
                        break;
                    case 'u':
                        printint(ft_ctx, va_arg(args, unsigned long));
                        break;
                    case 'x':
                    case 'X':
                        printhex(ft_ctx, va_arg(args, unsigned long));
                        break;
                    case 'l':
                        i++; // Handle %llu, %lld, etc.
                        if(i < format_len) {
                            cur = format[i];
                            switch(cur) {
                            case 'u':
                                printint(ft_ctx, va_arg(args, unsigned long long));
                                break;
                            case 'd':
                                printint(ft_ctx, va_arg(args, long long));
                                break;
                            case 'x':
                            case 'X':
                                printhex(ft_ctx, va_arg(args, unsigned long long));
                                break;
                            }
                        }
                        break;
                    }
                }
                state = NORMAL;
                break;
            case '%':
                printch(ft_ctx, '%');
                state = NORMAL;
                break;
            default:
                // Unknown format, just print it
                printch(ft_ctx, '%');
                printch(ft_ctx, cur);
                state = NORMAL;
                break;
            }
            break;
        }
    }
    va_end(args);
    return 0;
}

void putpixel(uint64_t x, uint64_t y,uint32_t color){
    volatile uint32_t *screen = (uint32_t*)fb->address;
    screen[y *fb->pitch/4 + x] = color;
}

void fillrect(uint32_t color,uint64_t x, uint64_t y, uint64_t w, uint64_t h){
    uint64_t i,j;

    for(j=0; j< h; j++){
        for(i=0; i<w; i++){
            putpixel(x+i, y+j, color);
        }
    }
}

uint32_t get_pixel_color(uint8_t pixel){
    uint32_t color = pixel;
    color |= pixel << 8;
    color |= pixel << 16;
    return color;
}

void printch(struct flanterm_context *ft_ctx,char c) {
    if(ft_ctx != NULL) {
        flanterm_write(ft_ctx,&c,1);
    }
    // Also output to serial
    serial_putc(c);
}

void printstr(struct flanterm_context *ft_ctx, char *str)
{
    if(ft_ctx != NULL && str != NULL) {
        size_t len = strlen(str);
        if(len > 0) {
            flanterm_write(ft_ctx,str,len);
        }
    }
    // Also output to serial
    if(str != NULL) {
        serial_puts(str);
    }
}

void printbin(struct flanterm_context *ft_ctx, uint64_t num){
    char bin[MAX_INT_SIZE];
    int i = 0;
    while(num > 0){
        bin[i] = num % 2 + '0';
        num /= 2;
        i++;
    }
    for(int j = i-1; j >= 0; j--){
        printch(ft_ctx,bin[j]);
    }
}

void printint(struct flanterm_context *ft_ctx, uint64_t i)
{
    if(ft_ctx != NULL) {
        char *str = int2string(i);
        if(str != NULL) {
            size_t len = strlen(str);
            if(len > 0) {
                flanterm_write(ft_ctx, str, len);
            }
        }
    }
}

void printhex(struct flanterm_context *ft_ctx, uint64_t num){
    if(ft_ctx != NULL) {
        char *str = int2hex(num);
        if(str != NULL) {
            size_t len = strlen(str);
            if(len > 0) {
                flanterm_write(ft_ctx, str, len);
            }
        }
    }
}

void log_panic(char *msg){
    printf("{k}kern_panic: {ksn}", ANSI_COLOR_RED, ANSI_COLOR_RESET, msg);
    __asm__ volatile ("cli"); 
    __asm__ volatile ("hlt");
}

void log_err(char *msg){
    printf("{k}kern_err: {ksn}", ANSI_COLOR_RED, ANSI_COLOR_RESET, msg);

}

void log_warn(char *msg){
    printf("{k}kern_warn: {ksn}", ANSI_COLOR_YELLOW, ANSI_COLOR_RESET, msg);
}

void log_info(char *msg){
    printf("{k}kern_info: {ksn}", ANSI_COLOR_CYAN, ANSI_COLOR_RESET, msg);
}

void log_success(char *msg){
    printf("{k}kern_success: {ksn}", ANSI_COLOR_GREEN, ANSI_COLOR_RESET, msg);
}

void printk(struct flanterm_context *ft_ctx, char* str) {
    printstr(ft_ctx,"[KERNEL] ");
    printstr(ft_ctx,str);
}

void printex(struct flanterm_context *ft_ctx, char* str) {
    printstr(ft_ctx,"[EXCEPTION] ");
    printstr(ft_ctx,str);
}

char* int2string(uint32_t value) {
    static char buf[32] = {0};
    int i = 30;
    for(; value && i ; --i, value /= 10) {
        buf[i] = "0123456789"[value % 10];
    }
    return &buf[i+1];
}

char* int2hex(uint64_t value) {
    static char buf[64] = {0};
    int i = 30;
    for(; value && i ; --i, value /= 16) {
        buf[i] = "0123456789ABCDEF"[value % 16];
    }
    return &buf[i+1];
}


char* strtok(char* str, const char* delim) {
    static char* pos = NULL;
    if(str != NULL) pos = str;
    if(pos == NULL || *pos == '\0') return NULL;
    
    uint16_t len = strcspn(pos, delim);
    char* token = pos;
    pos += len;
    if(*pos != '\0')
        *pos++ = '\0';
    
    return token;
}

uint16_t strcspn(const char* ch1, const char* ch2){
    uint16_t len = 0;
    while(*ch1){
        const char* tmp = ch2;
        while(*tmp){
            if(*ch1 == *tmp) return len;
            tmp++;
        }
        ++len;
        ++ch1;
    }
    return len;
}

uint32_t strlen(const char* str) {
    uint32_t len = 0;
    while(str[len] != '\0') len++;
    return len;
}

char* remove_nl(char* str){
    uint32_t str_len = strlen(str);
    for(uint32_t i = 0; i < str_len; i++) {
        if(str[i] == '\n'){
            str[i] = '\0';
        }
    }
    return str;
}

bool strEql(const char* str1, const char* str2) {
    uint32_t str1_len = strlen(str1);
    for(uint32_t i = 0; i < str1_len; i++) {
        if(str1[i] != str2[i]) {
            return false;
        }
    }
    return true;
}

void dump_hex(char *stack) {
            int x = 0;
    for (int i = 0; i < 64; i++) {
        x++;
        printf("{x]",stack[i]);
    }
    printf("\n");
}

void dump_str(char *stack) {
    int i = 0;
    while (i < 64) {
        printf("{k}",&stack[i]);
        while (i < 64 && stack[i]) i++;
        i++;
    }
    printf("\n");
}

void wait(uint64_t ms){
    uint64_t curticks = _ticks;
    while(_ticks - curticks < ms){
        asm("nop");
    }
}
// Stack protection function
void __stack_chk_fail(void) {
    log_panic("Stack smashing detected");
}

void clear_screen() {
    extern struct flanterm_context *ft_ctx;
    if(ft_ctx != NULL) {
        ft_ctx->clear(ft_ctx, true);
    }
}
