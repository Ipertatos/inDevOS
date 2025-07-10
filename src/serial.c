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

extern bool serial_enabled; // defined in kernel.c

void serial_init(void) {
    if (!serial_enabled) return; // Skip hardware init if disabled
    outb(SERIAL_PORT + 1, 0x00);    // Disable all interrupts
    outb(SERIAL_PORT + 3, 0x80);    // Enable DLAB (set baud rate divisor)
    outb(SERIAL_PORT + 0, 0x01);    // Set divisor to 1 (lo byte) 115200 baud
    outb(SERIAL_PORT + 1, 0x00);    //                  (hi byte)
    outb(SERIAL_PORT + 3, 0x03);    // 8 bits, no parity, one stop bit
    outb(SERIAL_PORT + 2, 0xC7);    // Enable FIFO, clear them, with 14-byte threshold
    outb(SERIAL_PORT + 4, 0x0B);    // IRQs enabled, RTS/DSR set
}

void serial_putc(char c) {
    if (!serial_enabled) return;
    // Wait for transmit to be ready
    while ((inb(SERIAL_PORT + 5) & 0x20) == 0);
    outb(SERIAL_PORT, c);
}

void serial_puts(const char *str) {
    if (!serial_enabled) return;
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
    if (!serial_enabled) return;
    const char *p = format;
    va_list args;
    va_start(args, format);
    
    while (*p) {
        if (*p == '%' && *(p + 1)) {
            p++; // Skip '%'
            
            // Handle padding
            int pad_width = 0;
            bool zero_pad = false;
            
            // Check for zero padding
            if (*p == '0') {
                zero_pad = true;
                p++;
            }
            
            // Get padding width (works for both %03d and %3d)
            while (*p >= '0' && *p <= '9') {
                pad_width = pad_width * 10 + (*p - '0');
                p++;
            }
            
            // Handle long long modifier
            bool is_long_long = false;
            if (*p == 'l' && *(p+1) == 'l') {
                is_long_long = true;
                p += 2;
            }
            
            switch (*p) {
                case 'd': {
                    if (is_long_long) {
                        long long val = va_arg(args, long long);
                        char buf[32];
                        int i = 0;
                        if (val == 0) {
                            // Apply padding for zero
                            if (zero_pad && pad_width > 1) {
                                for (int k = 0; k < pad_width - 1; k++) {
                                    serial_putc('0');
                                }
                            } else if (pad_width > 1) {
                                for (int k = 0; k < pad_width - 1; k++) {
                                    serial_putc(' ');
                                }
                            }
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
                            
                            // Apply padding
                            if (zero_pad && pad_width > i) {
                                for (int k = 0; k < pad_width - i; k++) {
                                    serial_putc('0');
                                }
                            } else if (pad_width > i) {
                                for (int k = 0; k < pad_width - i; k++) {
                                    serial_putc(' ');
                                }
                            }
                            
                            while (i > 0) {
                                serial_putc(buf[--i]);
                            }
                        }
                    } else {
                        int val = va_arg(args, int);
                        char buf[12];
                        int i = 0;
                        if (val == 0) {
                            // Apply padding for zero
                            if (zero_pad && pad_width > 1) {
                                for (int k = 0; k < pad_width - 1; k++) {
                                    serial_putc('0');
                                }
                            } else if (pad_width > 1) {
                                for (int k = 0; k < pad_width - 1; k++) {
                                    serial_putc(' ');
                                }
                            }
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
                            
                            // Apply padding
                            if (zero_pad && pad_width > i) {
                                for (int k = 0; k < pad_width - i; k++) {
                                    serial_putc('0');
                                }
                            } else if (pad_width > i) {
                                for (int k = 0; k < pad_width - i; k++) {
                                    serial_putc(' ');
                                }
                            }
                            
                            while (i > 0) {
                                serial_putc(buf[--i]);
                            }
                        }
                    }
                    break;
                }
                case 'x': {
                    if (is_long_long) {
                        unsigned long long val = va_arg(args, unsigned long long);
                        char buf[17]; // 16 hex digits + null terminator
                        int i = 0;
                        
                        if (val == 0) {
                            // Handle padding for zero
                            if (zero_pad && pad_width > 1) {
                                for (int k = 0; k < pad_width - 1; k++) {
                                    serial_putc('0');
                                }
                            }
                            serial_putc('0');
                        } else {
                            // Convert to hex string
                            while (val > 0) {
                                int digit = val % 16;
                                buf[i++] = (digit < 10) ? ('0' + digit) : ('a' + digit - 10);
                                val /= 16;
                            }
                            
                            // Apply padding
                            if (zero_pad && pad_width > i) {
                                for (int k = 0; k < pad_width - i; k++) {
                                    serial_putc('0');
                                }
                            }
                            
                            // Print digits in reverse order
                            while (i > 0) {
                                serial_putc(buf[--i]);
                            }
                        }
                    } else {
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
                    }
                    break;
                }
                case 's': {
                    const char *str = va_arg(args, const char*);
                    if (str) {
                        serial_puts(str);
                    } else {
                        serial_puts("(null)");
                    }
                    break;
                }
                case 'c': {
                    char c = (char)va_arg(args, int);
                    serial_putc(c);
                    break;
                }
                case 'p': {
                    void *ptr = va_arg(args, void*);
                    unsigned long long val = (unsigned long long)ptr;
                    serial_puts("0x");
                    
                    char buf[17];
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
                default:
                    serial_putc('%');
                    if (pad_width > 0) {
                        // Debug: show what we parsed
                        serial_putc('0' + (pad_width % 10));
                    }
                    if (is_long_long) {
                        serial_puts("ll");
                    }
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