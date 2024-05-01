#pragma once

#include "types.h"
#include "limine.h"

extern volatile struct limine_framebuffer_request fb_rq;
extern struct limine_framebuffer *fb;

void initFB();

const unsigned char* get_bitmap_for_char(char c);

void putpixel(uint64_t x, uint64_t y,uint32_t color);
void fillrect(uint32_t color,uint64_t x , uint64_t y,uint64_t w, uint64_t h);

void print(const char* str);

uint32_t get_pixel_color(uint8_t pixel);

void printk(const char* str);

void printex(const char* str);

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