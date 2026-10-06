#ifndef KERNEL_STDIO_H
#define KERNEL_STDIO_H

/*
 * Canonical kernel stdio header.
 *
 * printf/vsnprintf/snprintf are all implemented in term.c.
 * Include this header instead of the host <stdio.h>.
 */

#include <stddef.h>
#include <stdarg.h>

int printf(const char *fmt, ...);
int snprintf(char *str, size_t size, const char *fmt, ...);
int vsnprintf(char *str, size_t size, const char *fmt, va_list ap);

#endif /* KERNEL_STDIO_H */
