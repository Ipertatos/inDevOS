#ifndef SERIAL_H
#define SERIAL_H

#include "types.h"
#include <stdbool.h>

// Global flag (defined in kernel.c) controlling whether serial output is enabled.
extern bool serial_enabled;

void serial_init(void);
void serial_putc(char c);
void serial_puts(const char *str);
void serial_printf(const char *format, ...);

#ifdef DEBUG_PRINT
#define DEBUG_SERIAL(...) serial_printf(__VA_ARGS__)
#else
#define DEBUG_SERIAL(...) do {} while(0)
#endif

#endif 