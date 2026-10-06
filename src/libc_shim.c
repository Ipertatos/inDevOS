#include "term.h"
#include "mem.h"
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <math.h>   /* HUGE_VAL and the maths prototypes (src/libc/math.h) */

extern void *memcpy(void *dst, const void *src, size_t n);

/* Minimal libc/math shims to satisfy Lua core in freestanding kernel. */

/* string/memory */
int strcmp(const char *a, const char *b) {
    while (*a && (*a == *b)) { a++; b++; }
    return (unsigned char)(*a) - (unsigned char)(*b);
}

int strncmp(const char *a, const char *b, size_t n) {
    while (n && *a && (*a == *b)) { a++; b++; n--; }
    if (n == 0) return 0;
    return (unsigned char)(*a) - (unsigned char)(*b);
}

char *strchr(const char *s, int c) {
    char ch = (char)c;
    while (*s) { if (*s == ch) return (char *)s; s++; }
    return (ch == 0) ? (char *)s : NULL;
}

char *strstr(const char *haystack, const char *needle) {
    if (!*needle) return (char *)haystack;
    for (const char *p = haystack; *p; p++) {
        const char *h = p, *n = needle;
        while (*h && *n && *h == *n) { h++; n++; }
        if (!*n) return (char *)p;
    }
    return NULL;
}

char *strpbrk(const char *s, const char *accept) {
    for (; *s; s++) {
        for (const char *a = accept; *a; a++) if (*s == *a) return (char *)s;
    }
    return NULL;
}

char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++)) {}
    return dst;
}

int strcoll(const char *s1, const char *s2) { return strcmp(s1, s2); }

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = (const unsigned char *)s;
    for (size_t i = 0; i < n; i++) if (p[i] == (unsigned char)c) return (void *)(p + i);
    return NULL;
}

/* ctype/locale */
struct lconv { char *decimal_point; };
static struct lconv g_lconv = { "." };
struct lconv *localeconv(void) { return &g_lconv; }

/* stdlib */
int abs(int x) { return x < 0 ? -x : x; }

void abort(void) {
    printf("abort() called - halting.\n");
    for(;;) {}
}

/* setjmp/longjmp are implemented in asmutils.asm (real register save and
 * restore). The old C stubs here always returned 0 and swallowed longjmp,
 * which silently broke Lua's error handling (lua_pcall protection). */

/* time */
typedef long time_t;
time_t time(time_t *tloc) { time_t now = 0; if (tloc) *tloc = now; return now; }

/* numeric parsing */
double strtod(const char *nptr, char **endptr) {
    const char *p = nptr;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f' || *p == '\v') p++;
    int neg = 0; if (*p == '+') p++; else if (*p == '-') { neg = 1; p++; }
    double intpart = 0.0;
    int any = 0;
    while (*p >= '0' && *p <= '9') { intpart = intpart*10.0 + (*p - '0'); p++; any = 1; }
    double frac = 0.0, base = 1.0;
    if (*p == '.') { p++; while (*p >= '0' && *p <= '9') { base *= 10.0; frac += (*p - '0')/base; p++; any = 1; } }
    double val = intpart + frac;
    if ((*p == 'e' || *p=='E')) {
        p++;
        int eneg = 0; if (*p == '+') p++; else if (*p == '-') { eneg = 1; p++; }
        int exp = 0; while (*p >= '0' && *p <= '9') { exp = exp*10 + (*p - '0'); p++; }
        while (exp-- > 0) { val = eneg ? (val/10.0) : (val*10.0); }
    }
    if (!any) { if (endptr) *endptr = (char *)nptr; return 0.0; }
    if (endptr) *endptr = (char *)p;
    return neg ? -val : val;
}

/* math */
double ldexp(double x, int exp) { if (exp >= 0) { while (exp--) x *= 2.0; } else { while (exp++) x /= 2.0; } return x; }
double frexp(double x, int *exp) {
    if (x == 0.0) { *exp = 0; return 0.0; }
    int e = 0; double v = x;
    while (v >= 1.0) { v /= 2.0; e++; }
    while (v < 0.5) { v *= 2.0; e--; }
    *exp = e; return v;
}
double floor(double x) { long long i = (long long)x; if (x < 0 && (double)i != x) i -= 1; return (double)i; }
double fmod(double a, double b) { if (b == 0.0) return 0.0; long long q = (long long)(a / b); return a - (double)q * b; }
double pow(double a, double b) {
    /* integer exponent fast path */
    long long ib = (long long)b;
    if ((double)ib == b) {
        double res = 1.0; long long e = ib; double base = a;
        if (e < 0) { e = -e; base = 1.0/base; }
        while (e) { if (e & 1) res *= base; base *= base; e >>= 1; }
        return res;
    }
    /* fallback: very rough exp via repeated multiplication */
    if (b == 0.0) return 1.0;
    if (a <= 0.0) return 0.0;
    double res = 1.0; int n = (int)b;
    if (n < 0) {
        n = -n;
        for (int i = 0; i < n; i++) res *= a;
        return 1.0/res;
    }
    for (int i = 0; i < n; i++) res *= a;
    return res;
}

/* minimal stdio shims to satisfy lauxlib macros without pulling real stdio */
typedef struct FILE FILE;
int printf(const char *fmt, ...);
int fprintf(FILE *stream, const char *fmt, ...);
int fflush(FILE *stream);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);
int vprintf(const char *fmt, va_list ap);
int putchar(int c);

/* provided by console implementation */
int vsnprintf(char *str, size_t size, const char *fmt, va_list ap);

/* basic console-backed stdio shims */
int vprintf(const char *fmt, va_list ap) {
    char tmp[512];
    int len = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    if (len > 0) term_write(tmp, (size_t)len);
    return len;
}

int putchar(int c) {
    term_putchar((char)c);
    return (unsigned char)c;
}

/* route lua's lauxlib printing to kernel console */
int fprintf(FILE *stream, const char *fmt, ...) { (void)stream; va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); return 0; }
int fflush(FILE *stream) { (void)stream; return 0; }
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) { (void)stream; const char *c = (const char *)ptr; size_t n = size * nmemb; for (size_t i=0;i<n;i++) putchar(c[i]); return nmemb; }


/* ---------------- Additional shims for Lua ---------------- */

/* errno */
static int g_errno;
int * __errno_location(void) { return &g_errno; }
char *strerror(int errnum) { (void)errnum; return "error"; }

/* stdio FILE stubs — minimal host glue for Lua/lauxlib; not a full libc. */
struct FILE { int dummy; };
static struct FILE g_stdin_s, g_stderr_s;
FILE *stdin = &g_stdin_s;
FILE *stderr = &g_stderr_s;

/* 64-bit fopen/freopen variants referenced by Lua */
FILE *fopen64(const char *pathname, const char *mode) { (void)pathname; (void)mode; return NULL; }
FILE *freopen64(const char *pathname, const char *mode, FILE *stream) { (void)pathname; (void)mode; (void)stream; return NULL; }

/* Plain (non-64) spellings: Lua's lauxlib uses fopen()/freopen() when it is not
 * compiled with _FILE_OFFSET_BITS=64.  Both are the same "no filesystem for
 * Lua's stdio" stub as the 64-bit variants above. */
FILE *fopen(const char *pathname, const char *mode) { (void)pathname; (void)mode; return NULL; }
FILE *freopen(const char *pathname, const char *mode, FILE *stream) { (void)pathname; (void)mode; (void)stream; return NULL; }
int fclose(FILE *stream) { (void)stream; return 0; }
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream) { (void)ptr; (void)size; (void)nmemb; (void)stream; return 0; }
int getc(FILE *stream) { (void)stream; return -1; }
int feof(FILE *stream) { (void)stream; return 1; }
int ferror(FILE *stream) { (void)stream; return 1; }

/* ctype helpers */
int tolower(int c) { if (c >= 'A' && c <= 'Z') return c - 'A' + 'a'; return c; }
int toupper(int c) { if (c >= 'a' && c <= 'z') return c - 'a' + 'A'; return c; }

/* glibc-specific ctype table shims used by host ctype.h macros in vendored
 * code (Lua). Tables are built lazily with the proper glibc _ISxxx class bits
 * so pattern classes like %a/%d actually work. */
#define ISH_UPPER  0x0100
#define ISH_LOWER  0x0200
#define ISH_ALPHA  0x0400
#define ISH_DIGIT  0x0800
#define ISH_XDIGIT 0x1000
#define ISH_SPACE  0x2000
#define ISH_PRINT  0x4000
#define ISH_GRAPH  0x8000
#define ISH_BLANK  0x0001
#define ISH_CNTRL  0x0002
#define ISH_PUNCT  0x0004
#define ISH_ALNUM  0x0008

static unsigned short ctype_b_table[384];
static int ctype_tolower_table[384];
static int ctype_toupper_table[384];
static int ctype_tables_ready;

static void ctype_tables_init(void) {
    for (int i = -128; i < 256; i++) {
        int c = i & 0xff;
        unsigned short f = 0;
        if (c < 32 || c == 127) f |= ISH_CNTRL;
        if (c >= 'A' && c <= 'Z') f |= ISH_UPPER | ISH_ALPHA | ISH_ALNUM;
        if (c >= 'a' && c <= 'z') f |= ISH_LOWER | ISH_ALPHA | ISH_ALNUM;
        if (c >= '0' && c <= '9') f |= ISH_DIGIT | ISH_ALNUM | ISH_XDIGIT;
        if ((c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')) f |= ISH_XDIGIT;
        if (c == ' ' || (c >= '\t' && c <= '\r')) f |= ISH_SPACE;
        if (c == ' ' || c == '\t') f |= ISH_BLANK;
        if (c >= 33 && c <= 126 && !(f & ISH_ALNUM)) f |= ISH_PUNCT;
        if (c >= 33 && c <= 126) f |= ISH_GRAPH;
        if (c >= 32 && c <= 126) f |= ISH_PRINT;
        ctype_b_table[128 + i] = f;
        ctype_tolower_table[128 + i] = (c >= 'A' && c <= 'Z') ? c + 32 : i;
        ctype_toupper_table[128 + i] = (c >= 'a' && c <= 'z') ? c - 32 : i;
    }
    ctype_tables_ready = 1;
}

const unsigned short int **__ctype_b_loc(void) {
    static const unsigned short *p;
    if (!ctype_tables_ready) ctype_tables_init();
    p = ctype_b_table + 128;
    return &p;
}

const int **__ctype_tolower_loc(void) {
    static const int *p;
    if (!ctype_tables_ready) ctype_tables_init();
    p = (const int *)ctype_tolower_table + 128;
    return &p;
}

const int **__ctype_toupper_loc(void) {
    static const int *p;
    if (!ctype_tables_ready) ctype_tables_init();
    p = (const int *)ctype_toupper_table + 128;
    return &p;
}

/* plain ctype functions for code compiled with __NO_CTYPE or extern refs */
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isalpha(int c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
int isalnum(int c) { return isdigit(c) || isalpha(c); }
int isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
int iscntrl(int c) { return (c >= 0 && c < 32) || c == 127; }
int ispunct(int c) { return c >= 33 && c <= 126 && !isalnum(c); }
int isxdigit(int c) { return isdigit(c) || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'); }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int isgraph(int c) { return c >= 33 && c <= 126; }
int isprint(int c) { return c >= 32 && c <= 126; }

long strtol(const char *nptr, char **endptr, int base) {
    const char *p = nptr;
    while (*p == ' ' || (*p >= '\t' && *p <= '\r')) p++;
    int neg = 0;
    if (*p == '+') p++;
    else if (*p == '-') { neg = 1; p++; }
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2; base = 16;
    } else if (base == 0) {
        base = (*p == '0') ? 8 : 10;
    }
    long val = 0;
    int any = 0;
    for (;;) {
        int d;
        if (*p >= '0' && *p <= '9') d = *p - '0';
        else if (*p >= 'a' && *p <= 'z') d = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'Z') d = *p - 'A' + 10;
        else break;
        if (d >= base) break;
        val = val * base + d;
        any = 1;
        p++;
    }
    if (endptr) *endptr = (char *)(any ? p : nptr);
    return neg ? -val : val;
}

/* time/clock */
typedef long clock_t;
clock_t clock(void) { return 0; }

/* memory allocation shims used by lauxlib (free/realloc) */
void free(void *ptr) { if (ptr) kfree(ptr); }
void *malloc(size_t size) { return size ? kalloc(size) : NULL; }
void *calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void *p = total ? kalloc(total) : NULL;
    if (p) {
        /* zero memory */
        uint8_t *b = (uint8_t *)p;
        for (size_t i = 0; i < total; i++) b[i] = 0;
    }
    return p;
}
void *realloc(void *ptr, size_t size) {
    if (!ptr) return size ? kalloc(size) : NULL;
    if (!size) { kfree(ptr); return NULL; }

    uint64_t old_size = kalloc_usable_size(ptr);
    if (old_size >= size) {
        /* Allocation is page-granular; the existing block already fits. */
        return ptr;
    }

    void *n = kalloc(size);
    if (!n) return NULL;
    memcpy(n, ptr, old_size < size ? old_size : size);
    kfree(ptr);
    return n;
}

/* math: safe implementations without builtins to avoid kernel pagefaults */
double fabs(double x) { 
    return x < 0.0 ? -x : x;
}

/* Taylor series implementations for trig functions */
double sin(double x) {
    // Normalize to [-π, π]
    while (x > 3.14159265359) x -= 6.28318530718;
    while (x < -3.14159265359) x += 6.28318530718;
    
    // Taylor series: sin(x) = x - x³/3! + x⁵/5! - x⁷/7! + ...
    double result = x;
    double term = x;
    for (int i = 1; i < 10; i++) {
        term *= -x * x / ((2*i) * (2*i + 1));
        result += term;
    }
    return result;
}

double cos(double x) {
    // Normalize to [-π, π]
    while (x > 3.14159265359) x -= 6.28318530718;
    while (x < -3.14159265359) x += 6.28318530718;
    
    // Taylor series: cos(x) = 1 - x²/2! + x⁴/4! - x⁶/6! + ...
    double result = 1.0;
    double term = 1.0;
    for (int i = 1; i < 10; i++) {
        term *= -x * x / ((2*i-1) * (2*i));
        result += term;
    }
    return result;
}

double tan(double x) {
    double c = cos(x);
    if (c == 0.0) return 0.0; // avoid division by zero
    return sin(x) / c;
}

double asin(double x) {
    if (x < -1.0 || x > 1.0) return 0.0;
    // Simple approximation: asin(x) ≈ x + x³/6 + 3x⁵/40 + ...
    double x2 = x * x;
    return x * (1.0 + x2 * (1.0/6.0 + x2 * (3.0/40.0)));
}

double acos(double x) {
    if (x < -1.0 || x > 1.0) return 0.0;
    return 1.5707963268 - asin(x); // π/2 - asin(x)
}

double sqrt(double x) {
    if (x < 0.0) return 0.0;
    if (x == 0.0) return 0.0;
    if (x == 1.0) return 1.0;
    
    // Newton's method - safe implementation
    double guess = x / 2.0;
    for (int i = 0; i < 10; i++) {
        if (guess == 0.0) break; // avoid division by zero
        double new_guess = (guess + x / guess) / 2.0;
        if (fabs(new_guess - guess) < 1e-10) break; // converged
        guess = new_guess;
    }
    return guess;
}

double atan2(double y, double x) {
    if (x == 0.0 && y == 0.0) return 0.0;
    if (x > 0.0) return asin(y / sqrt(x*x + y*y));
    if (x < 0.0 && y >= 0.0) return 3.14159265359 + asin(y / sqrt(x*x + y*y));
    if (x < 0.0 && y < 0.0) return -3.14159265359 + asin(y / sqrt(x*x + y*y));
    if (y > 0.0) return 1.5707963268; // π/2
    return -1.5707963268; // -π/2
}

double ceil(double x) { 
    long long i = (long long)x; 
    return (x > (double)i) ? (double)(i + 1) : (double)i; 
}

double log(double x) {
    if (x <= 0.0) return 0.0;
    if (x == 1.0) return 0.0;
    
    // Simple approximation for all values to avoid recursion
    // ln(x) ≈ (x-1) - (x-1)²/2 + (x-1)³/3 for x near 1
    if (x > 0.5 && x < 2.0) {
        double t = x - 1.0;
        return t - t*t/2.0 + t*t*t/3.0;
    }
    
    // For other values, use simpler approximation
    return (x - 1.0) / x; // Very rough approximation
}

double log2(double x) {
    return log(x) / 0.69314718056; // log(x) / ln(2)
}

double log10(double x) {
    return log(x) / 2.30258509299; // log(x) / ln(10)
}

double exp(double x) {
    if (x == 0.0) return 1.0;
    if (x < 0.0) return 1.0 / exp(-x);
    
    // Taylor series: e^x = 1 + x + x²/2! + x³/3! + ...
    double result = 1.0;
    double term = 1.0;
    for (int i = 1; i < 20; i++) {
        term *= x / i;
        result += term;
        if (term < 1e-10) break; // convergence
    }
    return result;
}

/* Hyperbolic functions, required by Lua's math library (math.sinh/cosh/tanh).
 * Defined after exp()/fabs() so no forward declarations are needed. */
double sinh(double x) {
    double ax = fabs(x);
    if (ax > 710.0) return (x < 0.0) ? -HUGE_VAL : HUGE_VAL;
    double e = exp(ax);
    double r = (e - 1.0 / e) / 2.0;
    return (x < 0.0) ? -r : r;
}

double cosh(double x) {
    double ax = fabs(x);
    if (ax > 710.0) return HUGE_VAL;
    double e = exp(ax);
    return (e + 1.0 / e) / 2.0;
}

double tanh(double x) {
    double ax = fabs(x);
    if (ax > 20.0) return (x < 0.0) ? -1.0 : 1.0;  /* saturates well before exp() overflows */
    double e = exp(2.0 * ax);
    double r = (e - 1.0) / (e + 1.0);
    return (x < 0.0) ? -r : r;
}

