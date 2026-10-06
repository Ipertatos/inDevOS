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

/* Calibrate TSC rate for boot-relative serial timestamps; call from hpet_init()
 * after hpet_freq is known, with the HPET counter running. */
void serial_time_calibrate_via_hpet(void);
/* t0/t1 are rdtsc values spanning elapsed_us of a known-rate counter. */
void serial_time_note_tsc_span(uint64_t t0, uint64_t t1, uint64_t elapsed_us);

#ifdef DEBUG_PRINT
#define DEBUG_SERIAL(...) serial_printf(__VA_ARGS__)
#else
#define DEBUG_SERIAL(...) do {} while(0)
#endif

#endif 