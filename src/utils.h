#pragma once

#include "types.h"

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

void putpixel(uint64_t x, uint64_t y, uint32_t color);
void fillrect(uint32_t color, uint64_t x, uint64_t y, uint64_t w, uint64_t h);

void log_panic(char *msg);
void log_err(char *msg);
void log_warn(char *msg);
void log_info(char *msg);
void log_success(char *msg);

char *int2string(uint32_t n);
char *int2hex(uint64_t value);

char *remove_nl(char *str);
char *strtok(char *str, const char *delim);
bool strEql(const char *str1, const char *str2);

void dump_hex(char *stack);
void dump_str(char *stack);

void clear_screen();

// Interrupt context tracking functions
void enter_interrupt_context(void);
void exit_interrupt_context(void);
int in_interrupt_context(void);

// Framebuffer Console Functions
void fb_console_init(void);
void fb_console_putchar(char c);
void fb_console_write(const char *str, size_t len);
void fb_console_clear(void);
void fb_console_scroll(void);
void fb_cursor_blink_tick(void);
void fb_set_font_scale(uint32_t scale);
void fb_test_pattern(void);

// Framebuffer initialization
void debug_framebuffer_request(void);

void wait(uint64_t ms);

void console_set_fg(uint32_t color);
uint32_t console_get_fg(void);
uint32_t console_get_width(void);
uint32_t console_get_height(void);
