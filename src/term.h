#pragma once

#include <stddef.h>

void term_init(void);
void term_write(const char *buf, size_t len);
int printf(const char *fmt, ...);   /* replaces old flanterm printf */
void term_putchar(char c); 