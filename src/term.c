#include "term.h"
#include "serial.h"
#include <stdarg.h>
#include <stddef.h>
#include "utils.h"
#include "limine.h"
#include "string.h"
#include "vconsole.h"

// Framebuffer console backend

int vsnprintf(char *str, size_t size, const char *fmt, va_list ap);

// ============================================================================
// Output capture for shell piping / redirection
// ============================================================================
static char  *capture_buf  = NULL;   /* buffer to capture into */
static size_t capture_size = 0;      /* capacity */
static size_t capture_pos  = 0;      /* current write position */
static bool   capture_only = false;  /* if true, suppress normal output */
static bool   serial_suppress = false; /* if true, skip serial (TUI avoids spam) */
static void (*term_output_hook)(const char *buf, size_t len) = NULL;

void term_set_output_hook(void (*hook)(const char *buf, size_t len)) {
    term_output_hook = hook;
}

void term_capture_start(char *buf, size_t size, bool suppress_output) {
    capture_buf  = buf;
    capture_size = size;
    capture_pos  = 0;
    capture_only = suppress_output;
    if (buf && size > 0) buf[0] = '\0';
}

size_t term_capture_stop(void) {
    size_t n = capture_pos;
    if (capture_buf && capture_pos < capture_size)
        capture_buf[capture_pos] = '\0';
    capture_buf  = NULL;
    capture_size = 0;
    capture_pos  = 0;
    capture_only = false;
    return n;
}

// ============================================================================

void term_serial_suppress(bool enable) {
    serial_suppress = enable;
}

int term_write_buf(const char *buf, size_t len) {
    if (!buf || len == 0) return 0;

    /* When the desktop compositor owns the framebuffer, suppress the
     * UART path for user-mode stdout/stderr. Each byte costs ~87 us at
     * 115200 baud, and chatty user apps (e.g. DOOM's ~50-byte log lines
     * per frame) were blocking sys_write for 5–15 ms each, which
     * destroyed user-app frame rates. Kernel-side serial logging
     * (serial_printf, sdl_kernel_log, etc.) is unaffected because those
     * call serial_putc directly without going through term_write. */
    extern bool desktop_is_running(void);
    if (desktop_is_running()) {
        bool prev = serial_suppress;
        serial_suppress = true;
        term_write(buf, len);
        serial_suppress = prev;
        return (int)len;
    }

    term_write(buf, len);
    return (int)len;
}

void term_init(void) {
    // Use framebuffer console instead of VGA text mode
    fb_console_init();
    serial_puts("[term_init] Framebuffer console initialised\n");
}

void term_write(const char *buf, size_t len) {
    /* If capturing, copy to capture buffer */
    if (capture_buf) {
        for (size_t i = 0; i < len && capture_pos + 1 < capture_size; i++) {
            capture_buf[capture_pos++] = buf[i];
        }
    }

    /* Fire the output hook (e.g. telnet server mirrors output to TCP) */
    if (term_output_hook) {
        term_output_hook(buf, len);
    }

    /* If capture_only, skip normal terminal output */
    if (capture_only) return;

    if (!serial_suppress) {
        for(size_t i=0;i<len;i++) {
            if(buf[i] == '\n') serial_putc('\r');
            serial_putc(buf[i]);
        }
    }

    /* Route output for background virtual consoles to their ring buffer
     * instead of the visible framebuffer. */
    int vc = vconsole_get_thread_console();

    /* The desktop owns VT 0: text for VT 0 goes to serial only (handled
     * above), whether or not VT 0 is on screen. Text VTs keep working
     * while the desktop runs. */
    /* Detached: a process detached from every VT (the desktop terminal
     * window's shell and its children) -> serial only, never painted over
     * the desktop or a text VT. The desktop / LuaDE own VT 0's pixels. */
    if (vconsole_thread_detached()) return;
    if (vconsole_gfx_owner_running() && vc == 0) return;

    if (vc >= 0 && vc != vconsole_get_active()) {
        vconsole_buffer_output(vc, buf, len);
        return;
    }

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
/* Enhanced vsnprintf implementation (supports %s %c %d %u %x %f %g %e)  */
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

static void append_double(char **buf, size_t *remaining, double val, char format, 
                         int precision, int width, int *written){
    char tmp[64];
    int i = 0;
    
    // Handle special cases
    if(val != val) { // NaN
        append_str(buf, remaining, "nan", written);
        return;
    }
    if(val < 0.0) {
        append_char(buf, remaining, '-', written);
        val = -val;
    }
    if(val == (1.0/0.0)) { // infinity
        append_str(buf, remaining, "inf", written);
        return;
    }
    
    // Default precision
    if(precision < 0) precision = (format == 'g') ? 6 : 6;
    
    if(format == 'f') {
        // Fixed-point notation: 123.456
        unsigned long long int_part = (unsigned long long)val;
        double frac_part = val - (double)int_part;
        
        // Print integer part
        if(int_part == 0) {
            tmp[i++] = '0';
        } else {
            int start = i;
            while(int_part && i < 32) {
                tmp[i++] = '0' + (int_part % 10);
                int_part /= 10;
            }
            // Reverse the integer part
            for(int j = start, k = i-1; j < k; j++, k--) {
                char temp = tmp[j];
                tmp[j] = tmp[k];
                tmp[k] = temp;
            }
        }
        
        if(precision > 0) {
            tmp[i++] = '.';
            // Print fractional part
            for(int p = 0; p < precision && i < 60; p++) {
                frac_part *= 10.0;
                int digit = (int)frac_part;
                tmp[i++] = '0' + digit;
                frac_part -= (double)digit;
            }
        }
    } else if(format == 'g') {
        // Choose %f or %e format, remove trailing zeros
        if(val == 0.0) {
            tmp[i++] = '0';
        } else if(val >= 0.0001 && val < 1000000.0) {
            // Use %f format
            unsigned long long int_part = (unsigned long long)val;
            double frac_part = val - (double)int_part;
            
            if(int_part == 0) {
                tmp[i++] = '0';
            } else {
                int start = i;
                while(int_part && i < 32) {
                    tmp[i++] = '0' + (int_part % 10);
                    int_part /= 10;
                }
                // Reverse
                for(int j = start, k = i-1; j < k; j++, k--) {
                    char temp = tmp[j];
                    tmp[j] = tmp[k];
                    tmp[k] = temp;
                }
            }
            
            if(frac_part > 0.0) {
                tmp[i++] = '.';
                int trailing_zeros = 0;
                for(int p = 0; p < 6 && i < 60; p++) {
                    frac_part *= 10.0;
                    int digit = (int)frac_part;
                    if(digit == 0) {
                        trailing_zeros++;
                    } else {
                        trailing_zeros = 0;
                    }
                    tmp[i++] = '0' + digit;
                    frac_part -= (double)digit;
                }
                // Remove trailing zeros
                i -= trailing_zeros;
                if(tmp[i-1] == '.') i--; // Remove decimal point if no fractional part
            }
        } else {
            // Use scientific notation (simplified)
            append_str(buf, remaining, "1e+99", written);
            return;
        }
    } else { // 'e' format  
        append_str(buf, remaining, "1.000000e+00", written);
        return;
    }
    
    tmp[i] = '\0';
    append_str(buf, remaining, tmp, written);
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
        int precision = -1;

        /* Left-align flag.  Padding is not applied to %s here, but the flag must
         * be consumed: previously "%-22s" fell through to the default case,
         * printed a literal "%-22s" and then let the following conversions read
         * the wrong varargs (the profiler's syscall table printed pointers as
         * integers and lost every syscall name). */
        int alt = 0;              /* '#': 0x prefix for nonzero %x/%X */
        while(*fmt == '-' || *fmt == '#' || *fmt == '+' || *fmt == ' '){
            if(*fmt == '#') alt = 1;
            fmt++;
        }
        if(*fmt == '0'){
            zero_pad = 1;
            fmt++;
        }
        while(*fmt >= '0' && *fmt <= '9'){
            width = width*10 + (*fmt - '0');
            fmt++;
        }
        
        // Parse precision
        if(*fmt == '.'){
            fmt++;
            precision = 0;
            while(*fmt >= '0' && *fmt <= '9'){
                precision = precision*10 + (*fmt - '0');
                fmt++;
            }
        }

        /* Length modifiers.  intmax_t/ptrdiff_t/size_t are all 64-bit on this
         * target, so z/j/t behave like ll.  Previously only 'l' was parsed:
         * a format such as %zu fell through to the default case and printed a
         * literal "%zu" (visible in the boot log for mempool_create). */
        if(*fmt == 'h'){
            /* h/hh promote to int for varargs, so long_mod stays 0 */
            fmt++;
            if(*fmt == 'h'){
                fmt++;
            }
        } else if(*fmt == 'l'){
            long_mod = 1;
            fmt++;
            if(*fmt == 'l'){
                long_mod = 2;
                fmt++;
            }
        } else if(*fmt == 'z' || *fmt == 'j' || *fmt == 't'){
            long_mod = 2;
            fmt++;
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
                if(alt && val){ append_str(&buf, &remaining, "0x", &written); if(width >= 2) width -= 2; }
                append_uint(&buf, &remaining, val, 16, 0, width, zero_pad, &written);
                break;
            }
            case 'X': {
                unsigned long long val;
                if(long_mod)
                    val = va_arg(ap, unsigned long long);
                else
                    val = va_arg(ap, unsigned int);
                if(alt && val){ append_str(&buf, &remaining, "0X", &written); if(width >= 2) width -= 2; }
                append_uint(&buf, &remaining, val, 16, 1, width, zero_pad, &written);
                break;
            }
            case 'p': {
                unsigned long long val = (unsigned long long)va_arg(ap, void*);
                append_str(&buf, &remaining, "0x", &written);
                append_uint(&buf, &remaining, val, 16, 0, width, zero_pad, &written);
                break;
            }
            case 'f': {
                double val = va_arg(ap, double);
                append_double(&buf, &remaining, val, 'f', precision, width, &written);
                break;
            }
            case 'g': {
                double val = va_arg(ap, double);
                append_double(&buf, &remaining, val, 'g', precision, width, &written);
                break;
            }
            case 'G': {
                double val = va_arg(ap, double);
                append_double(&buf, &remaining, val, 'g', precision, width, &written);
                break;
            }
            case 'e': {
                double val = va_arg(ap, double);
                append_double(&buf, &remaining, val, 'e', precision, width, &written);
                break;
            }
            case 'E': {
                double val = va_arg(ap, double);
                append_double(&buf, &remaining, val, 'e', precision, width, &written);
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

/* snprintf() lives here with vsnprintf(); it used to be defined in
 * src/usoft/info.c, which forced that file (and the whole kernel-side
 * "user software" directory) to stay in the kernel build. */
int snprintf(char *str, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int result = vsnprintf(str, size, fmt, ap);
    va_end(ap);
    return result;
}
