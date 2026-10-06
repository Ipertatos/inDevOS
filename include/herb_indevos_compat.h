/*
 * Freestanding C library shims for porting herb-engine-C-3D to inDevOS user apps.
 * Included when building with -D__INDEVOS_USERAPP__ (see herb_engine_indevos.c).
 */
#ifndef HERB_INDEVOS_COMPAT_H
#define HERB_INDEVOS_COMPAT_H

#include <stddef.h>
#include <stdint.h>
#include "indevos_syscall_numbers.h"

/* No libm when linking with -nostdlib -static; keep tiny math here. */
#ifndef HERB_PI
#define HERB_PI 3.14159265358979323846
#endif

static inline double herb_fabs(double x) {
    return x < 0.0 ? -x : x;
}

static inline double herb_floor(double x) {
    long xi = (long)x;
    if ((double)xi > x) return (double)(xi - 1);
    return (double)xi;
}

static inline double herb_sqrt(double x) {
    if (x <= 0.0) return 0.0;
    double r = x;
    for (int i = 0; i < 20; i++) r = 0.5 * (r + x / r);
    return r;
}

/* Taylor sin on [-pi/2, pi/2] */
static inline double herb_sin_impl(double x) {
    double x2 = x * x;
    double t = x;
    double s = t;
    t *= x2 / 6.0;
    s -= t;
    t *= x2 / 20.0;
    s += t;
    t *= x2 / 42.0;
    s -= t;
    t *= x2 / 72.0;
    s += t;
    return s;
}

static inline double herb_sin(double x) {
    /* reduce to [-pi, pi] */
    const double two_pi = 2.0 * HERB_PI;
    while (x > HERB_PI) x -= two_pi;
    while (x < -HERB_PI) x += two_pi;
    if (x > HERB_PI / 2.0) return herb_sin_impl(HERB_PI - x);
    if (x < -HERB_PI / 2.0) return -herb_sin_impl(HERB_PI + x);
    return herb_sin_impl(x);
}

static inline double herb_cos(double x) {
    return herb_sin(x + HERB_PI / 2.0);
}

#define sin herb_sin
#define cos herb_cos
#define sqrt herb_sqrt
#define floor herb_floor

/* timespec is referenced by herbengineC3D.c (Linux platform uses it for timing). */
struct timespec {
    long tv_sec;
    long tv_nsec;
};

/* ---- memcpy (engine does not use it; qsort helper does) ---- */
static inline void *herb_memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
    return dst;
}

/* ---- Bump allocator with headers for realloc() ---- */
#ifndef HERB_HEAP_BYTES
#define HERB_HEAP_BYTES (48u * 1024u * 1024u)
#endif

static unsigned char herb_heap[HERB_HEAP_BYTES];
static size_t herb_heap_used;

static inline void *malloc(size_t size) {
    if (size == 0) return (void *)0;
    size_t align = 16;
    size_t hdr = sizeof(size_t);
    size_t n = hdr + size;
    n = (n + (align - 1)) & ~(align - (size_t)1);
    if (herb_heap_used + n > HERB_HEAP_BYTES) {
        return (void *)0;
    }
    unsigned char *p = &herb_heap[herb_heap_used];
    herb_heap_used += n;
    *(size_t *)p = size;
    return p + hdr;
}

static inline void free(void *ptr) {
    (void)ptr;
}

static inline void *realloc(void *ptr, size_t new_size) {
    if (!ptr) return malloc(new_size);
    if (new_size == 0) {
        free(ptr);
        return (void *)0;
    }
    size_t old_sz = *((size_t *)((unsigned char *)ptr - sizeof(size_t)));
    void *n = malloc(new_size);
    if (!n) return (void *)0;
    size_t copy = old_sz < new_size ? old_sz : new_size;
    herb_memcpy(n, ptr, copy);
    return n;
}

/* ---- time() for srand(seed) (SYS_TIME = 13) ---- */
static inline uint64_t herb_syscall(uint64_t num, uint64_t arg1, uint64_t arg2,
                                    uint64_t arg3, uint64_t arg4, uint64_t arg5) {
    uint64_t result;
    register uint64_t r10_reg __asm__("r10") = arg4;
    register uint64_t r8_reg __asm__("r8") = arg5;
    __asm__ volatile("int $0x80"
                     : "=a"(result)
                     : "a"(num), "D"(arg1), "S"(arg2), "d"(arg3), "r"(r10_reg), "r"(r8_reg)
                     : "memory", "rcx", "r11");
    return result;
}

typedef long time_t;
static inline time_t indevos_time(time_t *tp) {
    uint64_t t = herb_syscall(SYS_TIME, 0, 0, 0, 0, 0);
    if (tp) *tp = (time_t)t;
    return (time_t)t;
}

/* ---- PRNG (stdlib rand) ---- */
static uint32_t herb_rand_state = 1u;
static inline void srand(unsigned int seed) {
    herb_rand_state = seed ? seed : 1u;
}
static inline int rand(void) {
    herb_rand_state = herb_rand_state * 1103515245u + 12345u;
    return (int)((herb_rand_state >> 16) & 0x7fff);
}

static inline int abs(int x) {
    return x < 0 ? -x : x;
}

/* ---- qsort (Lomuto quicksort) ---- */
static inline void herb_swap(char *a, char *b, size_t size) {
    for (size_t i = 0; i < size; i++) {
        unsigned char t = (unsigned char)a[i];
        a[i] = b[i];
        b[i] = (char)t;
    }
}

static void herb_qsort_rec(char *a, size_t n, size_t w, int (*cmp)(const void *, const void *)) {
    if (n <= 1) return;
    /* pivot = last element */
    char *pivot = a + (n - 1) * w;
    size_t i = 0;
    for (size_t j = 0; j < n - 1; j++) {
        if (cmp(a + j * w, pivot) <= 0) {
            herb_swap(a + i * w, a + j * w, w);
            i++;
        }
    }
    herb_swap(a + i * w, a + (n - 1) * w, w);
    if (i > 0) herb_qsort_rec(a, i, w, cmp);
    if (n > i + 1) herb_qsort_rec(a + (i + 1) * w, n - i - 1, w, cmp);
}

static inline void herb_qsort(void *base, size_t nel, size_t width,
                              int (*cmp)(const void *, const void *)) {
    if (!base || nel < 2 || width == 0) return;
    herb_qsort_rec((char *)base, nel, width, cmp);
}

#define qsort herb_qsort

#endif /* HERB_INDEVOS_COMPAT_H */
