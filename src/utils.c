#include "utils.h"
#include "flanterm/flanterm.h"
#include "flanterm/backends/fb.h"
#include <stdarg.h>
#define MAX_INT_SIZE 128

#define FORMAT_LENTGH       1
#define NORMAL              0
#define STATE_SHORT         2
#define STATE_LONG          3
#define FORMAT_SPECIFIER    1

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


struct flanterm_context *initFB(){
    if(fb_rq.response == NULL
    || fb_rq.response->framebuffer_count < 1){
        //printk("No framebuffer found\n");
    }
    fb = fb_rq.response->framebuffers[0];
    struct flanterm_context *ft_ctx = flanterm_fb_init(
        NULL,
        NULL,
        fb->address,fb->width,fb->height,fb->pitch,
        fb->red_mask_size,fb->red_mask_shift,
        fb->green_mask_size,fb->green_mask_shift,
        fb->blue_mask_size,fb->blue_mask_shift,
        NULL,
        NULL,NULL,
        NULL,NULL,
        NULL,NULL,
        NULL, 0,0,1,
        0,0,
        0
    );
    return ft_ctx;
}
int printf(const char *format, ...) {
    extern struct flanterm_context *ft_ctx;
    int state = NORMAL;
    va_list args;
    va_start(args, format);
    for(int i=0; i< strlen(format); i++){
        char cur = format[i];
        switch (state){
        case NORMAL:
            switch (cur){
            case '{':
                state = FORMAT_SPECIFIER;
                break;
            default:
                printch(ft_ctx,cur);
                break;
            }
            break;
        case FORMAT_SPECIFIER:
            switch (cur){
            case 'n':
                printch(ft_ctx,'\n');
                break;
            case 'k':
                printstr(ft_ctx,va_arg(args,char*));
                break;
            case 'd':
            case 'i':
                printint(ft_ctx,va_arg(args,long long));
                break;
            case 's':
                printstr(ft_ctx,va_arg(args,char*));
                break;
            case 'c':
                ;
                char c = va_arg(args,int);
                printch(ft_ctx,c);
                break;
            case 'x':
                printhex(ft_ctx,va_arg(args,uint64_t));
                break;
            case 'l':
                cur++;
                switch (cur)
                {
                case 'd':
                    printint(ft_ctx,va_arg(args,long));
                    break;
                }
                break;
            case '}':
                state = NORMAL;
                break;
            }
            break;  // This break statement should be here, not outside the for loop.
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
    volatile uint32_t *where = (uint32_t*)fb->address;

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
    flanterm_write(ft_ctx,&c,1);
}

void printstr(struct flanterm_context *ft_ctx, char *str)
{
    flanterm_write(ft_ctx,str,strlen(str));
}

void printint(struct flanterm_context *ft_ctx, uint64_t i)
{
    flanterm_write(ft_ctx,int2string(i),strlen(int2string(i)));
}

void printhex(struct flanterm_context *ft_ctx, uint64_t num){
    flanterm_write(ft_ctx,int2hex(num),strlen(int2hex(num)));
}

void log_panic(char *msg){
    printf("{k}kern_panic: {ksn}", ANSI_COLOR_RED, ANSI_COLOR_RESET, msg);
    __asm__ volatile ("cli; hlt"); // Completely hangs the computer
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
