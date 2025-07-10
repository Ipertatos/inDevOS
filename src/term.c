#include "term.h"
#include "serial.h"
#include <stdarg.h>
#include <stddef.h>
#include "utils.h"
#include "limine.h"

// Framebuffer console backend

int vsnprintf(char *str, size_t size, const char *fmt, va_list ap);

void term_init(void) {
    // Use framebuffer console instead of VGA text mode
    fb_console_init();
    serial_puts("[term_init] Framebuffer console initialised\n");
}

void term_write(const char *buf, size_t len) {
    for(size_t i=0;i<len;i++) serial_putc(buf[i]);
    // Use framebuffer console instead of VGA
    fb_console_write(buf, len);
}

void term_putchar(char c) {
    term_write(&c, 1);
}

int printf(const char *fmt, ...) {
    char tmp[512];
    va_list ap; va_start(ap, fmt);
    int len = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (len > 0) term_write(tmp, (size_t)len);
    return len;
}

/* ------------------------------------------------------------ */
/* Minimal vsnprintf implementation (supports %s %c %d %u %x)  */
/* ------------------------------------------------------------ */

static void append_char(char **buf, size_t *remaining, char c, int *written){
    if(*remaining > 1){
        **buf = c;
        (*buf)++;
        (*remaining)--;
    }
    (*written)++;
}

static void append_str(char **buf, size_t *remaining, const char *s, int *written){
    while(*s){
        append_char(buf, remaining, *s++, written);
    }
}

static void append_uint(char **buf, size_t *remaining, unsigned long long val, int base, int uppercase,
                       int width, int zero_pad, int *written){
    char tmp[32];
    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    int i = 0;
    if(val == 0){
        tmp[i++] = '0';
    } else {
        while(val && i < (int)sizeof(tmp)){
            tmp[i++] = digits[val % base];
            val /= base;
        }
    }
    int num_len = i;
    int pad_len = (width > num_len) ? width - num_len : 0;
    char pad_char = zero_pad ? '0' : ' ';
    while(pad_len-- > 0){
        append_char(buf, remaining, pad_char, written);
    }
    while(i-- > 0){
        append_char(buf, remaining, tmp[i], written);
    }
}

int vsnprintf(char *str, size_t size, const char *fmt, va_list ap){
    char *buf = str;
    size_t remaining = size;
    int written = 0;
    while(*fmt){
        if(*fmt != '%'){
            append_char(&buf, &remaining, *fmt++, &written);
            continue;
        }
        fmt++; // skip '%'
        int long_mod = 0; // 0=none,1=l,2=ll
        int zero_pad = 0;
        int width = 0;

        if(*fmt == '0'){
            zero_pad = 1;
            fmt++;
        }
        while(*fmt >= '0' && *fmt <= '9'){
            width = width*10 + (*fmt - '0');
            fmt++;
        }

        if(*fmt == 'l'){
            long_mod = 1;
            fmt++;
            if(*fmt == 'l'){
                long_mod = 2;
                fmt++;
            }
        }
        char spec = *fmt++;
        switch(spec){
            case 's': {
                const char *s = va_arg(ap, const char*);
                if(!s) s = "(null)";
                append_str(&buf, &remaining, s, &written);
                break;
            }
            case 'c': {
                int c = va_arg(ap, int);
                append_char(&buf, &remaining, (char)c, &written);
                break;
            }
            case 'd':
            case 'i': {
                long long val;
                if(long_mod)
                    val = va_arg(ap, long long);
                else
                    val = va_arg(ap, int);
                if(val < 0){
                    append_char(&buf, &remaining, '-', &written);
                    append_uint(&buf, &remaining, (unsigned long long)(-val), 10, 0, width, zero_pad, &written);
                } else {
                    append_uint(&buf, &remaining, (unsigned long long)val, 10, 0, width, zero_pad, &written);
                }
                break;
            }
            case 'u': {
                unsigned long long val;
                if(long_mod)
                    val = va_arg(ap, unsigned long long);
                else
                    val = va_arg(ap, unsigned int);
                append_uint(&buf, &remaining, val, 10, 0, width, zero_pad, &written);
                break;
            }
            case 'x': {
                unsigned long long val;
                if(long_mod)
                    val = va_arg(ap, unsigned long long);
                else
                    val = va_arg(ap, unsigned int);
                append_uint(&buf, &remaining, val, 16, 0, width, zero_pad, &written);
                break;
            }
            case 'X': {
                unsigned long long val;
                if(long_mod)
                    val = va_arg(ap, unsigned long long);
                else
                    val = va_arg(ap, unsigned int);
                append_uint(&buf, &remaining, val, 16, 1, width, zero_pad, &written);
                break;
            }
            case 'p': {
                unsigned long long val = (unsigned long long)va_arg(ap, void*);
                append_str(&buf, &remaining, "0x", &written);
                append_uint(&buf, &remaining, val, 16, 0, width, zero_pad, &written);
                break;
            }
            case '%': {
                append_char(&buf, &remaining, '%', &written);
                break;
            }
            default:
                // unknown specifier, print it verbatim
                append_char(&buf, &remaining, '%', &written);
                append_char(&buf, &remaining, spec, &written);
                break;
        }
    }
    // null terminate if space
    if(remaining > 0){
        *buf = '\0';
    } else if(size > 0){
        str[size-1] = '\0';
    }
    return written;
} 