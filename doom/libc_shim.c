// libc_shim.c - Minimal C library implementation for DOOM on inDevOS
//
// This file provides standard C library functions by implementing them
// from scratch or wrapping inDevOS syscalls directly.

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#include "../include/indevos_syscall_numbers.h"
#include "../include/indevos_stat.h"

// Forward declarations
typedef struct _IO_FILE FILE;
typedef long time_t;
typedef long off_t;
typedef int pid_t;

// Low-level syscall function
static inline uint64_t syscall(uint64_t num, uint64_t arg1, uint64_t arg2, 
                              uint64_t arg3, uint64_t arg4, uint64_t arg5) {
    uint64_t result;
    register uint64_t r10_reg __asm__("r10") = arg4;
    register uint64_t r8_reg __asm__("r8") = arg5;
    __asm__ volatile (
        "int $0x80"
        : "=a" (result)
        : "a" (num), "D" (arg1), "S" (arg2), "d" (arg3), "r" (r10_reg), "r" (r8_reg)
        : "memory"
    );
    return result;
}

// Kernel function stubs (weak symbols - can be overridden by kernel)
uint64_t __attribute__((weak)) get_ticks(void) {
    /* SYS_TIME returns milliseconds (hpet_get_ticks). */
    return (uint64_t)syscall(SYS_TIME, 0, 0, 0, 0, 0);
}

void __attribute__((weak)) hpet_sleep_counter(uint64_t ms) {
    syscall(SYS_SDL_DELAY, ms, 0, 0, 0, 0);
}

void __attribute__((weak)) beep(void) {
    // Fallback: no-op
}

void __attribute__((weak)) nosound(void) {
    // Fallback: no-op
}

int __attribute__((weak)) keyboard_consume_char(void) {
    // Fallback: return no key
    return -1;
}

// Framebuffer pointer (weak symbol)
struct limine_framebuffer {
    void *address;
    uint64_t width;
    uint64_t height;
    uint64_t pitch;
    uint16_t bpp;
};

struct limine_framebuffer *__attribute__((weak)) fb = NULL;

// File streams
FILE *stdin = (FILE*)0;
FILE *stdout = (FILE*)1;
FILE *stderr = (FILE*)2;

// Ctype functions
int toupper(int c) {
    return (c >= 'a' && c <= 'z') ? (c - 32) : c;
}

int *__ctype_toupper_loc(void) {
    static int toupper_table[256];
    static int initialized = 0;
    
    if (!initialized) {
        for (int i = 0; i < 256; i++) {
            toupper_table[i] = (i >= 'a' && i <= 'z') ? (i - 32) : i;
        }
        initialized = 1;
    }
    
    return toupper_table;
}

// String functions
size_t strlen(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

char *strcpy(char *dest, const char *src) {
    char *d = dest;
    while ((*d++ = *src++));
    return dest;
}

char *strncpy(char *dest, const char *src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++)
        dest[i] = src[i];
    for (; i < n; i++)
        dest[i] = '\0';
    return dest;
}

int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(unsigned char*)s1 - *(unsigned char*)s2;
}

int strncmp(const char *s1, const char *s2, size_t n) {
    while (n && *s1 && (*s1 == *s2)) {
        ++s1;
        ++s2;
        --n;
    }
    if (n == 0) {
        return 0;
    } else {
        return (*(unsigned char *)s1 - *(unsigned char *)s2);
    }
}

char *strcat(char *dest, const char *src) {
    char *d = dest;
    while (*d) d++;
    while ((*d++ = *src++));
    return dest;
}

int strcasecmp(const char *s1, const char *s2) {
    while (*s1 && *s2) {
        char c1 = *s1, c2 = *s2;
        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        if (c1 != c2) return c1 - c2;
        s1++;
        s2++;
    }
    return *s1 - *s2;
}

int strncasecmp(const char *s1, const char *s2, size_t n) {
    while (n && *s1 && *s2) {
        char c1 = *s1, c2 = *s2;
        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        if (c1 != c2) return c1 - c2;
        s1++;
        s2++;
        n--;
    }
    if (n == 0) return 0;
    return *s1 - *s2;
}

// Memory functions
//
// These are on DOOM's hot path (screen/flat/texture setup and the per-frame
// 320x200 -> 960x600 upscale), and the whole OS is emulated in QEMU, where a
// byte-at-a-time loop costs roughly 8x more than it needs to.  Fill/copy a
// machine word at a time instead.
void *memset(void *s, int c, size_t n) {
    unsigned char *p = (unsigned char *)s;
    unsigned char b = (unsigned char)c;

    while (n && ((uintptr_t)p & 7u)) { *p++ = b; n--; }
    if (n >= 8u) {
        uint64_t pattern = 0x0101010101010101ULL * (uint64_t)b;
        uint64_t *q = (uint64_t *)p;
        while (n >= 32u) { q[0] = pattern; q[1] = pattern; q[2] = pattern; q[3] = pattern; q += 4; n -= 32u; }
        while (n >= 8u)  { *q++ = pattern; n -= 8u; }
        p = (unsigned char *)q;
    }
    while (n--) *p++ = b;
    return s;
}

void *memcpy(void *dest, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;

    while (n && (((uintptr_t)d | (uintptr_t)s) & 7u)) { *d++ = *s++; n--; }
    if (n >= 8u) {
        uint64_t *dq = (uint64_t *)d;
        const uint64_t *sq = (const uint64_t *)s;
        while (n >= 32u) { dq[0] = sq[0]; dq[1] = sq[1]; dq[2] = sq[2]; dq[3] = sq[3]; dq += 4; sq += 4; n -= 32u; }
        while (n >= 8u)  { *dq++ = *sq++; n -= 8u; }
        d = (unsigned char *)dq;
        s = (const unsigned char *)sq;
    }
    while (n--) *d++ = *s++;
    return dest;
}

void *__memset_chk(void *dest, int c, size_t len, size_t destlen) {
    return memset(dest, c, len);
}

// Memory allocation
//
// malloc must return storage aligned for any type; on x86-64 that means 16
// bytes.  This bump allocator used to advance by the exact requested size, so
// successive allocations were misaligned — which -O2 codegen turned into a
// general-protection fault the first time gcc used an aligned SSE move on a
// copied struct.
void *malloc(size_t size) {
    static void *current_break = NULL;
    if (current_break == NULL) {
        current_break = (void*)syscall(SYS_BRK, 0, 0, 0, 0, 0);
        current_break = (void*)(((uintptr_t)current_break + 15u) & ~(uintptr_t)15u);
    }
    void *old_break = current_break;
    uintptr_t next = ((uintptr_t)old_break + size + 15u) & ~(uintptr_t)15u;
    void *new_break = (void*)syscall(SYS_BRK, (uint64_t)next, 0, 0, 0, 0);
    if (new_break == NULL) {
        return NULL;
    }
    current_break = new_break;
    return old_break;
}

void free(void *ptr) {
    // Simple free - no-op for now
    (void)ptr;
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    void *new_ptr = malloc(size);
    if (new_ptr && ptr) {
        memcpy(new_ptr, ptr, size);
        free(ptr);
    }
    return new_ptr;
}

void *alloca(size_t size) {
    return malloc(size);
}

// File I/O
int open(const char *pathname, int flags, int mode) {
    return (int)syscall(SYS_OPEN, (uint64_t)pathname, (uint64_t)(unsigned)flags,
                        (uint64_t)(unsigned)mode, 0, 0);
}

int read(int fd, void *buf, size_t count) {
    return (int)syscall(SYS_READ, fd, (uint64_t)buf, count, 0, 0);
}

int write(int fd, const void *buf, size_t count) {
    return (int)syscall(SYS_WRITE, fd, (uint64_t)buf, count, 0, 0);
}

int close(int fd) {
    return (int)syscall(SYS_CLOSE, fd, 0, 0, 0, 0);
}

off_t lseek(int fd, off_t offset, int whence) {
    return (off_t)syscall(SYS_SEEK, fd, offset, whence, 0, 0);
}

int access(const char *pathname, int mode) {
    stat_t st;
    (void)mode;
    long r = (long)syscall(SYS_STAT, (uint64_t)pathname, (uint64_t)&st, 0, 0, 0);
    if (r < 0)
        return -1;
    return 0;
}

int fstat(int fd, stat_t *statbuf) {
    long r = (long)syscall(SYS_FSTAT, (uint64_t)(unsigned)fd, (uint64_t)statbuf, 0, 0, 0);
    if (r < 0)
        return -1;
    return 0;
}

FILE *fopen(const char *pathname, const char *mode) {
    return NULL;  // Not implemented
}

int fclose(FILE *stream) {
    return 0;
}

// Math functions
int abs(int n) {
    return n < 0 ? -n : n;
}

/* Append signed decimal with optional width / zero flag / .precision (enough for DOOM's %.3d, %03d, %.2d) */
static void fmt_append_di(char *str, size_t *np, size_t cap, int value,
                          int width, int prec, int flag_zero)
{
    size_t n = *np;
    char dbuf[24];
    int neg = (value < 0);
    unsigned mag;
    if (value < 0 && value == -2147483647 - 1)
        mag = 2147483648u;
    else if (value < 0)
        mag = (unsigned)(-value);
    else
        mag = (unsigned)value;
    int nd = 0;
    if (mag == 0)
        dbuf[nd++] = '0';
    else
        while (mag) {
            dbuf[nd++] = (char)('0' + (mag % 10u));
            mag /= 10u;
        }
    int min_d = (prec >= 0) ? prec : 1;
    int zf = (min_d > nd) ? min_d - nd : 0;
    int numcore = zf + nd;
    int body = numcore + (neg ? 1 : 0);
    int pad = (width > body) ? width - body : 0;
    if (flag_zero && prec < 0 && pad > 0) {
        zf += pad;
        pad = 0;
    }
    while (pad > 0 && n < cap) {
        str[n++] = ' ';
        pad--;
    }
    if (neg && n < cap)
        str[n++] = '-';
    while (zf > 0 && n < cap) {
        str[n++] = '0';
        zf--;
    }
    while (nd > 0 && n < cap)
        str[n++] = dbuf[--nd];
    *np = n;
}

// Minimal vsnprintf for %s, %d, %i, %u, %x, %X, %c, %%  (+ %03d, %.3d, etc.)
static int format_str(char *str, size_t size, const char *format, va_list args) {
    size_t n = 0;
    const char *f = format;
    while (*f && n < size) {
        if (*f == '%') {
            f++;
            int flag_zero = 0;
            if (*f == '0') {
                flag_zero = 1;
                f++;
            }
            int width = 0;
            while (*f >= '0' && *f <= '9')
                width = width * 10 + (*f++ - '0');
            int prec = -1;
            if (*f == '.') {
                f++;
                prec = 0;
                while (*f >= '0' && *f <= '9')
                    prec = prec * 10 + (*f++ - '0');
            }
            if (*f == 'd' || *f == 'i') {
                int arg = va_arg(args, int);
                fmt_append_di(str, &n, size, arg, width, prec, flag_zero);
                f++;
            } else if (*f == 's') {
                char *arg = va_arg(args, char *);
                if (arg) while (*arg && n < size) str[n++] = *arg++;
                f++;
            } else if (*f == 'u') {
                unsigned u = va_arg(args, unsigned);
                char buf[24];
                int i = 0;
                do { buf[i++] = '0' + (u % 10); u /= 10; } while (u != 0);
                while (i > 0 && n < size) str[n++] = buf[--i];
                f++;
            } else if (*f == 'x' || *f == 'X') {
                unsigned u = va_arg(args, unsigned);
                const char *hex = (*f == 'X') ? "0123456789ABCDEF" : "0123456789abcdef";
                char buf[16];
                int i = 0;
                do { buf[i++] = hex[u & 15]; u >>= 4; } while (u != 0);
                while (i > 0 && n < size) str[n++] = buf[--i];
                f++;
            } else if (*f == 'c') {
                int c = va_arg(args, int);
                if (n < size) str[n++] = (char)(unsigned char)c;
                f++;
            } else if (*f == '%') {
                if (n < size) str[n++] = '%';
                f++;
            } else {
                if (n < size) str[n++] = '%';
                if (*f && n < size)
                    str[n++] = *f++;
                else if (*f)
                    f++;
            }
        } else {
            str[n++] = *f++;
        }
    }
    if (n < size) str[n] = '\0'; else if (size > 0) str[size - 1] = '\0';
    return (int)n;
}

int vsnprintf(char *str, size_t size, const char *format, va_list ap) {
    return format_str(str, size, format, ap);
}

int snprintf(char *str, size_t size, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    int n = vsnprintf(str, size, format, ap);
    va_end(ap);
    return n;
}

int printf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    char buf[4096];
    int n = format_str(buf, sizeof(buf), format, args);
    va_end(args);
    buf[n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1] = '\0';
    (void)syscall(SYS_PRINT, (uint64_t)buf, 0, 0, 0, 0);
    return n;
}

int sprintf(char *str, const char *format, ...) {
    va_list args;
    va_start(args, format);
    int n = format_str(str, (size_t)4096, format, args);
    va_end(args);
    return n;
}

int fprintf(FILE *stream, const char *format, ...) {
    va_list args;
    va_start(args, format);
    char buf[4096];
    int n = format_str(buf, sizeof(buf), format, args);
    va_end(args);
    buf[n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1] = '\0';
    if (stream == stdout || stream == stderr)
        (void)syscall(SYS_PRINT, (uint64_t)buf, 0, 0, 0, 0);
    (void)stream;
    return n;
}

// Printf family (minimal implementations)
int __printf_chk(int flag, const char *format, ...) {
    (void)flag;
    va_list args;
    va_start(args, format);
    char buf[4096];
    int n = format_str(buf, sizeof(buf), format, args);
    va_end(args);
    buf[n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1] = '\0';
    (void)syscall(SYS_PRINT, (uint64_t)buf, 0, 0, 0, 0);
    return n;
}

int __fprintf_chk(FILE *stream, int flag, const char *format, ...) {
    if (stream == stderr) {
        return (int)syscall(SYS_PRINT, (uint64_t)format, 0, 0, 0, 0);
    }
    return 0;
}

int __sprintf_chk(char *str, int flag, size_t slen, const char *format, ...) {
    va_list args;
    va_start(args, format);
    
    char *s = str;
    const char *f = format;
    
    while (*f && slen > 1) {
        if (*f == '%') {
            f++;
            if (*f == 's') {
                // String argument
                char *arg = va_arg(args, char*);
                if (arg) {
                    while (*arg && slen > 1) {
                        *s++ = *arg++;
                        slen--;
                    }
                }
                f++;
            } else if (*f == 'd' || *f == 'i') {
                // Integer argument
                int arg = va_arg(args, int);
                char buf[32];
                int len = 0;
                int is_neg = 0;
                
                if (arg < 0) {
                    is_neg = 1;
                    arg = -arg;
                }
                
                // Convert to string (reverse order)
                do {
                    buf[len++] = '0' + (arg % 10);
                    arg /= 10;
                } while (arg > 0);
                
                if (is_neg) buf[len++] = '-';
                
                // Write in correct order
                while (len > 0 && slen > 1) {
                    *s++ = buf[--len];
                    slen--;
                }
                f++;
            } else if (*f == '%') {
                *s++ = '%';
                slen--;
                f++;
            } else {
                // Unknown format - skip
                f++;
            }
        } else {
            *s++ = *f++;
            slen--;
        }
    }
    
    *s = '\0';
    va_end(args);
    return s - str;
}

int __strcpy_chk(char *dest, const char *src, size_t destlen) {
    char *d = dest;
    const char *s = src;
    while (*s && destlen > 0) {
        *d++ = *s++;
        destlen--;
    }
    *d = '\0';
    return d - dest;
}

// scanf stub
int __isoc99_sscanf(const char *str, const char *format, ...) {
    return 0;  // Not implemented
}

// More standard library functions
char *getenv(const char *name) {
    // Provide basic environment variables that DOOM needs
    if (strcmp(name, "HOME") == 0) {
        return "/home/user";
    }
    if (strcmp(name, "DOOMWADDIR") == 0) {
        /* Absolute IWADs live at the volume root (putfilesinsata / mcopy). */
        return "/";
    }
    return NULL;
}

void exit(int status) {
    syscall(SYS_EXIT, status, 0, 0, 0, 0);
    while(1);  // Should never reach here
}

long strtol(const char *nptr, char **endptr, int base) {
    // Very basic implementation
    long result = 0;
    int negative = 0;
    
    while (*nptr == ' ' || *nptr == '\t') nptr++;
    
    if (*nptr == '-') {
        negative = 1;
        nptr++;
    } else if (*nptr == '+') {
        nptr++;
    }
    
    while (*nptr >= '0' && *nptr <= '9') {
        result = result * 10 + (*nptr - '0');
        nptr++;
    }
    
    if (endptr) *endptr = (char*)nptr;
    return negative ? -result : result;
}

int atoi(const char *nptr) {
    return (int)strtol(nptr, NULL, 10);
}

int mkdir(const char *pathname, int mode) {
    (void)mode;
    return (int)syscall(SYS_MKDIR, (uint64_t)pathname, 0, 0, 0, 0);
}

int fseek(FILE *stream, long offset, int whence) {
    return -1;  // Not implemented
}

long ftell(FILE *stream) {
    return -1;  // Not implemented
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    return 0;  // Not implemented
}

int getc(FILE *stream) {
    (void)stream;
    return (int)syscall(SYS_GETCHAR, 0, 0, 0, 0, 0);
}

int getchar(void) {
    return getc(stdin);
}

void setbuf(FILE *stream, char *buf) {
    // No-op
}

int feof(FILE *stream) {
    return 1;  // Always at EOF
}

int __isoc99_fscanf(FILE *stream, const char *format, ...) {
    return 0;  // Not implemented
}

// Protected string functions
void *__memcpy_chk(void *dest, const void *src, size_t len, size_t destlen) {
    return memcpy(dest, src, len);
}

char *__strcat_chk(char *dest, const char *src, size_t destlen) {
    char *d = dest;
    while (*d) d++;
    while ((*d++ = *src++));
    return dest;
}

char *__strncpy_chk(char *dest, const char *src, size_t n, size_t destlen) {
    return strncpy(dest, src, n);
}

int __vsnprintf_chk(char *str, size_t maxlen, int flag, size_t slen, 
                    const char *format, va_list ap) {
    // Very basic implementation - just copy format string
    char *s = str;
    const char *f = format;
    while (*f && maxlen > 0) {
        *s++ = *f++;
        maxlen--;
    }
    *s = '\0';
    return s - str;
}

// DOOM config variable
char *sndserver_filename = "sndserver";

/*
 * inDevOS exec builds the x86-64 process initial stack as:
 *   [RSP+0]  argc
 *   [RSP+8]  argv[0]
 *   [RSP+16] argv[1] (NULL)
 * The program entry must NOT be main() — RDI/RSI are not set by the kernel.
 *
 * argc/argv therefore have to be read off the stack before anything touches
 * RSP, which a normal C function cannot guarantee: at -O2 gcc adjusts RSP in
 * the prologue before the inline asm runs, so the load picked up a saved
 * register instead of argc, myargc became garbage and FindResponseFile walked
 * a bogus argv until it took a #GP.  Keep the stack read in a naked stub and
 * hand off to plain C for everything else.
 */
__attribute__((naked, noreturn)) void _start(void) {
    __asm__ volatile(
        "movq (%rsp), %rdi\n\t"     /* argc */
        "leaq 8(%rsp), %rsi\n\t"    /* argv */
        "andq $-16, %rsp\n\t"       /* 16-byte align for the call */
        "call app_main\n\t"         /* never returns */
        "ud2\n\t"
    );
}

void app_main(long argc, char **argv) {
    extern int main(int argc, char **argv);

    static char *fallback_argv[] = { "app", NULL };
    if (argc < 1 || argv == NULL || argv[0] == NULL) {
        argc = 1;
        argv = fallback_argv;
    }

    int ret = main((int)argc, argv);

    syscall(SYS_EXIT, (uint64_t)(unsigned)ret, 0, 0, 0, 0);
    while (1)
        ;
}
