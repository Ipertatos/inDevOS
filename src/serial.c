#include "serial.h"
#include "atomic.h"
#include "hpet.h"
#include "bootlog.h"
#include <stdarg.h>
#include <stddef.h>

#define SERIAL_PORT 0x3F8  // COM1

static spinlock_t serial_out_lock = {0};

/* IRQ-safe serial lock.  ISR paths print too (exception/irqdiag/scheduler
 * logs): with IRQs enabled, an interrupt arriving while this CPU holds the
 * lock spins on it forever, and every other CPU that prints piles up behind
 * it (ostest freeze: CPU3 ISR spinning in serial_printf on top of
 * fork_child_zombie_exit's own serial_printf, CPU0 stuck in serial_putc).
 * Hold it with IRQs off; the spin is bounded so an exception/NMI that
 * re-enters on the holder's CPU still gets its text out. */
static inline uint64_t serial_lock_irq(void) {
    uint64_t fl;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(fl) :: "memory");
    uint64_t spins = 0;
    while (__sync_lock_test_and_set(&serial_out_lock.lock, 1)) {
        while (serial_out_lock.lock) {
            __asm__ volatile("pause");
            if (++spins > 200000000ULL)
                return fl;          /* holder re-entered/stuck: print anyway */
        }
    }
    return fl;
}

static inline void serial_unlock_irq(uint64_t fl) {
    __sync_lock_release(&serial_out_lock.lock);
    if (fl & 0x200)
        __asm__ volatile("sti" ::: "memory");
}

/* dmesg-style line prefix: [    sec.usec] */
#define SERIAL_LINE_PEEK 6
#define TSC_PER_US_DEFAULT 2500ULL  /* ~2.5 GHz until HPET calibration */

static inline void outb(uint16_t port, uint8_t data) {
    __asm__ volatile("outb %0, %1" : : "a"(data), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t data;
    __asm__ volatile("inb %1, %0" : "=a"(data) : "Nd"(port));
    return data;
}

static inline uint64_t serial_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

extern bool serial_enabled;

static uint64_t serial_boot_tsc;
static uint64_t tsc_per_us;

static bool at_line_start = true;
static bool in_timestamp;
static char line_peek[SERIAL_LINE_PEEK];
static int line_peek_len;

static void serial_hw_putc(char c) {
    bootlog_putc(c);           /* RAM boot log, written to /BOOTLOG.TXT */
    while ((inb(SERIAL_PORT + 5) & 0x20) == 0)
        ;
    outb(SERIAL_PORT, c);
}

static void serial_hw_put_u64_padded(uint64_t val, int min_width) {
    char buf[20];
    int n = 0;

    if (val == 0) {
        buf[n++] = '0';
    } else {
        while (val > 0) {
            buf[n++] = (char)('0' + (val % 10));
            val /= 10;
        }
    }

    for (int i = n; i < min_width; i++)
        serial_hw_putc(' ');
    while (n > 0)
        serial_hw_putc(buf[--n]);
}

static uint64_t serial_boot_us_now(void) {
    uint64_t per_us = tsc_per_us ? tsc_per_us : TSC_PER_US_DEFAULT;
    return (serial_rdtsc() - serial_boot_tsc) / per_us;
}

static void serial_print_timestamp(void) {
    uint64_t us = serial_boot_us_now();
    uint64_t sec = us / 1000000ULL;
    uint64_t frac = us % 1000000ULL;
    static const uint64_t frac_div[6] = {
        100000ULL, 10000ULL, 1000ULL, 100ULL, 10ULL, 1ULL
    };

    in_timestamp = true;
    serial_hw_putc('[');
    serial_hw_put_u64_padded(sec, 4);
    serial_hw_putc('.');
    for (int i = 0; i < 6; i++)
        serial_hw_putc((char)('0' + ((frac / frac_div[i]) % 10)));
    serial_hw_putc(']');
    serial_hw_putc(' ');
    in_timestamp = false;
}

static int serial_line_is_profiler_csv(const char *buf, int len) {
    static const char prefix[] = "KP,v1,";
    if (len < (int)sizeof(prefix) - 1)
        return 0;
    for (int i = 0; i < (int)sizeof(prefix) - 1; i++) {
        if (buf[i] != prefix[i])
            return 0;
    }
    return 1;
}

static void serial_flush_peek(bool with_timestamp) {
    if (line_peek_len <= 0)
        return;
    if (with_timestamp)
        serial_print_timestamp();
    for (int i = 0; i < line_peek_len; i++)
        serial_hw_putc(line_peek[i]);
    line_peek_len = 0;
}

static void serial_putc_inner(char c);

/* Unlocked putc — caller must hold serial_out_lock. */
static void serial_putc_unlocked(char c) {
    if (!serial_enabled)
        return;
    if (c == '\n')
        serial_putc_inner('\r');
    serial_putc_inner(c);
}

static void serial_puts_unlocked(const char *str) {
    if (!str) str = "(null)";
    while (*str)
        serial_putc_unlocked(*str++);
}


static void serial_putc_inner(char c) {
    if (!serial_enabled)
        return;

    if (in_timestamp) {
        serial_hw_putc(c);
        return;
    }

    if (line_peek_len > 0) {
        if (c == '\n') {
            serial_flush_peek(true);
            at_line_start = true;
            serial_hw_putc('\n');
            return;
        }

        if (line_peek_len < SERIAL_LINE_PEEK) {
            line_peek[line_peek_len++] = c;
            if (line_peek_len == SERIAL_LINE_PEEK) {
                bool raw = serial_line_is_profiler_csv(line_peek, line_peek_len);
                serial_flush_peek(!raw);
                at_line_start = false;
            }
            return;
        }

        serial_hw_putc(c);
        return;
    }

    if (at_line_start && c != '\r' && c != '\n') {
        line_peek[0] = c;
        line_peek_len = 1;
        if (SERIAL_LINE_PEEK == 1) {
            serial_flush_peek(true);
            at_line_start = false;
        }
        return;
    }

    if (c == '\n')
        at_line_start = true;

    serial_hw_putc(c);
}

void serial_time_note_tsc_span(uint64_t t0, uint64_t t1, uint64_t elapsed_us) {
    if (t1 <= t0 || elapsed_us == 0)
        return;

    uint64_t per = (t1 - t0) / elapsed_us;
    if (per == 0)
        per = 1;

    uint64_t old_per_us = tsc_per_us ? tsc_per_us : TSC_PER_US_DEFAULT;
    uint64_t displayed_us = 0;
    if (t1 > serial_boot_tsc)
        displayed_us = (t1 - serial_boot_tsc) / old_per_us;
    tsc_per_us = per;
    serial_boot_tsc = t1 - displayed_us * tsc_per_us;
}

void serial_time_calibrate_via_hpet(void) {
    if (!hpet_main_cnt || !hpet_freq)
        return;

    uint64_t c0 = *hpet_main_cnt;
    uint64_t t0 = serial_rdtsc();
    uint64_t target = c0 + hpet_freq / 100; /* 10 ms */

    while (*hpet_main_cnt < target)
        __asm__ volatile("pause");

    serial_time_note_tsc_span(t0, serial_rdtsc(), 10000ULL);
}

void serial_init(void) {
    serial_boot_tsc = serial_rdtsc();

    if (!serial_enabled)
        return;

    outb(SERIAL_PORT + 1, 0x00);
    outb(SERIAL_PORT + 3, 0x80);
    outb(SERIAL_PORT + 0, 0x01);
    outb(SERIAL_PORT + 1, 0x00);
    outb(SERIAL_PORT + 3, 0x03);
    outb(SERIAL_PORT + 2, 0xC7);
    outb(SERIAL_PORT + 4, 0x0B);
}

void serial_putc(char c) {
    if (!serial_enabled)
        return;

    uint64_t sfl_ = serial_lock_irq();
    serial_putc_unlocked(c);
    serial_unlock_irq(sfl_);
}

void serial_puts(const char *str) {
    if (!serial_enabled)
        return;
    uint64_t sfl_ = serial_lock_irq();
    while (*str)
        serial_putc_unlocked(*str++);
    serial_unlock_irq(sfl_);
}

// Simple printf implementation for serial
void serial_printf(const char *format, ...) {
    if (!serial_enabled) return;
    uint64_t sfl_ = serial_lock_irq();
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
            
            /* Length modifiers.  long/size_t/intmax_t/ptrdiff_t are all 64-bit
             * on this target, so %l/%ll/%z/%j/%t read a 64-bit value; h/hh
             * promote to int.  Without this, %zu fell through to the default
             * case and printed a literal "%zu" (e.g. the desktop's
             * "backbuffer allocated ... (%zu bytes)" line). */
            bool is_long_long = false;
            if (*p == 'l' && *(p+1) == 'l') {
                is_long_long = true;
                p += 2;
            } else if (*p == 'l' || *p == 'z' || *p == 'j' || *p == 't') {
                is_long_long = true;
                p += 1;
            } else if (*p == 'h') {
                p += 1;
                if (*p == 'h') p += 1;
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
                                    serial_putc_unlocked('0');
                                }
                            } else if (pad_width > 1) {
                                for (int k = 0; k < pad_width - 1; k++) {
                                    serial_putc_unlocked(' ');
                                }
                            }
                            serial_putc_unlocked('0');
                        } else {
                            if (val < 0) {
                                serial_putc_unlocked('-');
                                val = -val;
                            }
                            while (val > 0) {
                                buf[i++] = '0' + (val % 10);
                                val /= 10;
                            }
                            
                            // Apply padding
                            if (zero_pad && pad_width > i) {
                                for (int k = 0; k < pad_width - i; k++) {
                                    serial_putc_unlocked('0');
                                }
                            } else if (pad_width > i) {
                                for (int k = 0; k < pad_width - i; k++) {
                                    serial_putc_unlocked(' ');
                                }
                            }
                            
                            while (i > 0) {
                                serial_putc_unlocked(buf[--i]);
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
                                    serial_putc_unlocked('0');
                                }
                            } else if (pad_width > 1) {
                                for (int k = 0; k < pad_width - 1; k++) {
                                    serial_putc_unlocked(' ');
                                }
                            }
                            serial_putc_unlocked('0');
                        } else {
                            if (val < 0) {
                                serial_putc_unlocked('-');
                                val = -val;
                            }
                            while (val > 0) {
                                buf[i++] = '0' + (val % 10);
                                val /= 10;
                            }
                            
                            // Apply padding
                            if (zero_pad && pad_width > i) {
                                for (int k = 0; k < pad_width - i; k++) {
                                    serial_putc_unlocked('0');
                                }
                            } else if (pad_width > i) {
                                for (int k = 0; k < pad_width - i; k++) {
                                    serial_putc_unlocked(' ');
                                }
                            }
                            
                            while (i > 0) {
                                serial_putc_unlocked(buf[--i]);
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
                                    serial_putc_unlocked('0');
                                }
                            }
                            serial_putc_unlocked('0');
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
                                    serial_putc_unlocked('0');
                                }
                            }
                            
                            // Print digits in reverse order
                            while (i > 0) {
                                serial_putc_unlocked(buf[--i]);
                            }
                        }
                    } else {
                        unsigned int val = va_arg(args, unsigned int);
                        char buf[9];
                        int i = 0;
                        if (val == 0) {
                            serial_putc_unlocked('0');
                        } else {
                            while (val > 0) {
                                int digit = val % 16;
                                buf[i++] = (digit < 10) ? ('0' + digit) : ('a' + digit - 10);
                                val /= 16;
                            }
                            while (i > 0) {
                                serial_putc_unlocked(buf[--i]);
                            }
                        }
                    }
                    break;
                }
                case 's': {
                    const char *str = va_arg(args, const char*);
                    if (str) {
                        serial_puts_unlocked(str);
                    } else {
                        serial_puts_unlocked("(null)");
                    }
                    break;
                }
                case 'c': {
                    char c = (char)va_arg(args, int);
                    serial_putc_unlocked(c);
                    break;
                }
                case 'u': {
                    if (is_long_long) {
                        unsigned long long val = va_arg(args, unsigned long long);
                        char buf[21];
                        int i = 0;
                        if (val == 0) {
                            if (zero_pad && pad_width > 1) {
                                for (int k = 0; k < pad_width - 1; k++)
                                    serial_putc_unlocked('0');
                            } else if (pad_width > 1) {
                                for (int k = 0; k < pad_width - 1; k++)
                                    serial_putc_unlocked(' ');
                            }
                            serial_putc_unlocked('0');
                        } else {
                            while (val > 0) {
                                buf[i++] = '0' + (val % 10);
                                val /= 10;
                            }
                            if (zero_pad && pad_width > i) {
                                for (int k = 0; k < pad_width - i; k++)
                                    serial_putc_unlocked('0');
                            } else if (pad_width > i) {
                                for (int k = 0; k < pad_width - i; k++)
                                    serial_putc_unlocked(' ');
                            }
                            while (i > 0)
                                serial_putc_unlocked(buf[--i]);
                        }
                    } else {
                        unsigned int val = va_arg(args, unsigned int);
                        char buf[11];
                        int i = 0;
                        if (val == 0) {
                            if (zero_pad && pad_width > 1) {
                                for (int k = 0; k < pad_width - 1; k++)
                                    serial_putc_unlocked('0');
                            } else if (pad_width > 1) {
                                for (int k = 0; k < pad_width - 1; k++)
                                    serial_putc_unlocked(' ');
                            }
                            serial_putc_unlocked('0');
                        } else {
                            while (val > 0) {
                                buf[i++] = '0' + (val % 10);
                                val /= 10;
                            }
                            if (zero_pad && pad_width > i) {
                                for (int k = 0; k < pad_width - i; k++)
                                    serial_putc_unlocked('0');
                            } else if (pad_width > i) {
                                for (int k = 0; k < pad_width - i; k++)
                                    serial_putc_unlocked(' ');
                            }
                            while (i > 0)
                                serial_putc_unlocked(buf[--i]);
                        }
                    }
                    break;
                }
                case 'p': {
                    void *ptr = va_arg(args, void*);
                    unsigned long long val = (unsigned long long)ptr;
                    serial_puts_unlocked("0x");
                    
                    char buf[17];
                    int i = 0;
                    if (val == 0) {
                        serial_putc_unlocked('0');
                    } else {
                        while (val > 0) {
                            int digit = val % 16;
                            buf[i++] = (digit < 10) ? ('0' + digit) : ('a' + digit - 10);
                            val /= 16;
                        }
                        while (i > 0) {
                            serial_putc_unlocked(buf[--i]);
                        }
                    }
                    break;
                }
                default:
                    serial_putc_unlocked('%');
                    if (pad_width > 0) {
                        // Debug: show what we parsed
                        serial_putc_unlocked('0' + (pad_width % 10));
                    }
                    if (is_long_long) {
                        serial_putc_unlocked('l'); serial_putc_unlocked('l');
                    }
                    serial_putc_unlocked(*p);
                    break;
            }
        } else {
            serial_putc_unlocked(*p);
        }
        p++;
    }
    
    va_end(args);
    serial_unlock_irq(sfl_);
}
