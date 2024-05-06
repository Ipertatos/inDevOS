#pragma once

#include "types.h"
#include "limine.h"
#include "flanterm/flanterm.h"
#include "flanterm/backends/fb.h"


#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_GREEN   "\x1b[32m"
#define ANSI_COLOR_YELLOW  "\x1b[33m"
#define ANSI_COLOR_BLUE    "\x1b[34m"
#define ANSI_COLOR_MAGENTA "\x1b[35m"
#define ANSI_COLOR_CYAN    "\x1b[36m"
#define ANSI_COLOR_RESET   "\x1b[0m"

typedef struct position{
    uint8_t x;
    uint8_t y;
}position;

extern volatile struct limine_framebuffer_request fb_rq;
extern struct limine_framebuffer *fb;

struct flanterm_context *ft_ctx;

struct flanterm_context *initFB();

const unsigned char* get_bitmap_for_char(char c);

void putpixel(uint64_t x, uint64_t y,uint32_t color);
void fillrect(uint32_t color,uint64_t x , uint64_t y,uint64_t w, uint64_t h);



void printch(struct flanterm_context *ft_ctx, char c);
void printstr(struct flanterm_context *ft_ctx, char* str);

void printint(struct flanterm_context *ft_ctx, uint64_t i);
void printhex(struct flanterm_context *ft_ctx, uint64_t num);

void log_panic(char *msg);
void log_err(char *msg);
void log_warn(char *msg);
void log_info(char *msg);
void log_success(char *msg);

uint32_t get_pixel_color(uint8_t pixel);

void printk(struct flanterm_context *ft_ctx,char* str);

void printex(struct flanterm_context *ft_ctx,char* str);

char* int2string(uint32_t n);

char* int2hex(uint32_t value);

uint32_t strlen(const char* str);

bool strEql(const char* str1, const char* str2);
extern void stack_dump();

void dump_hex(char *stack);
void dump_str(char *stack);

void *memset(void *dest, int val, uint32_t count);

void call(char* buffer, uint32_t buff_len);

void *memcpy(void *dest, const void *src, uint32_t count);
void *memmove(void *dest, const void *src, uint32_t count);
int memcmp(const void *a, const void *b, uint32_t count);