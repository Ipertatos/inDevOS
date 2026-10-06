#pragma once

#include <stddef.h>
#include <stdbool.h>

void term_init(void);
void term_write(const char *buf, size_t len);
int printf(const char *fmt, ...);   /* replaces old flanterm printf */
void term_putchar(char c);

/* Output capture for shell piping / redirection */
void   term_capture_start(char *buf, size_t size, bool suppress_output);
size_t term_capture_stop(void);

/* Suppress serial output (e.g. when TUI is drawing - avoids spam) */
void term_serial_suppress(bool enable);

/* Output hook: if set, every term_write call ALSO invokes this function.
 * Used by the telnet server to mirror shell output to a TCP connection.
 * Pass NULL to remove the hook. */
void term_set_output_hook(void (*hook)(const char *buf, size_t len));

/* Bulk write: avoids per-character printf overhead in syscall paths */
int term_write_buf(const char *buf, size_t len);