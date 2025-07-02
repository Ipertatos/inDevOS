#include "serial.h"
#include <stdarg.h>

#define SERIAL_PORT 0x3F8  // COM1

// I/O port functions (these should be in your existing code)
static inline void outb(uint16_t port, uint8_t data) {
    __asm__ volatile("outb %0, %1" : : "a"(data), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t data;
    __asm__ volatile("inb %1, %0" : "=a"(data) : "Nd"(port));
    return data;
}

void serial_init(void) {
    outb(SERIAL_PORT + 1, 0x00);    // Disable all interrupts
    outb(SERIAL_PORT + 3, 0x80);    // Enable DLAB (set baud rate divisor)
    outb(SERIAL_PORT + 0, 0x01);    // Set divisor to 1 (lo byte) 115200 baud
    outb(SERIAL_PORT + 1, 0x00);    //                  (hi byte)
    outb(SERIAL_PORT + 3, 0x03);    // 8 bits, no parity, one stop bit
    outb(SERIAL_PORT + 2, 0xC7);    // Enable FIFO, clear them, with 14-byte threshold
    outb(SERIAL_PORT + 4, 0x0B);    // IRQs enabled, RTS/DSR set
}

void serial_putc(char c) {
    // Wait for transmit to be ready
    while ((inb(SERIAL_PORT + 5) & 0x20) == 0);
    outb(SERIAL_PORT, c);
}

void serial_puts(const char *str) {
    while (*str) {
        if (*str == '\n') {
            serial_putc('\r');
        }
        serial_putc(*str);
        str++;
    }
}

// Simple printf implementation for serial
void serial_printf(const char *format, ...) {
    // Simple implementation - just handle basic cases
    const char *p = format;
    va_list args;
    va_start(args, format);
    
    while (*p) {
        if (*p == '%' && *(p + 1)) {
            p++; // Skip '%'
            switch (*p) {
                case 'd': {
                    int val = va_arg(args, int);
                    char buf[12];
                    int i = 0;
                    if (val == 0) {
                        serial_putc('0');
                    } else {
                        if (val < 0) {
                            serial_putc('-');
                            val = -val;
                        }
                        while (val > 0) {
                            buf[i++] = '0' + (val % 10);
                            val /= 10;
                        }
                        while (i > 0) {
                            serial_putc(buf[--i]);
                        }
                    }
                    break;
                }
                case 'x': {
                    unsigned int val = va_arg(args, unsigned int);
                    char buf[9];
                    int i = 0;
                    if (val == 0) {
                        serial_putc('0');
                    } else {
                        while (val > 0) {
                            int digit = val % 16;
                            buf[i++] = (digit < 10) ? ('0' + digit) : ('a' + digit - 10);
                            val /= 16;
                        }
                        while (i > 0) {
                            serial_putc(buf[--i]);
                        }
                    }
                    break;
                }
                case '0': {
                    // Handle %02x format
                    if (*(p+1) == '2' && *(p+2) == 'x') {
                        p += 2; // Skip "02"
                        unsigned int val = va_arg(args, unsigned int);
                        if (val < 0x10) serial_putc('0');
                        char buf[9];
                        int i = 0;
                        if (val == 0) {
                            serial_putc('0');
                        } else {
                            while (val > 0) {
                                int digit = val % 16;
                                buf[i++] = (digit < 10) ? ('0' + digit) : ('a' + digit - 10);
                                val /= 16;
                            }
                            while (i > 0) {
                                serial_putc(buf[--i]);
                            }
                        }
                    } else {
                        serial_putc('%');
                        serial_putc(*p);
                    }
                    break;
                }
                case 's': {
                    const char *str = va_arg(args, const char*);
                    serial_puts(str);
                    break;
                }
                case 'c': {
                    char c = (char)va_arg(args, int);
                    serial_putc(c);
                    break;
                }
                default:
                    serial_putc('%');
                    serial_putc(*p);
                    break;
            }
        } else if (*p == '\n') {
            serial_putc('\r');
            serial_putc('\n');
        } else {
            serial_putc(*p);
        }
        p++;
    }
    
    va_end(args);
} 