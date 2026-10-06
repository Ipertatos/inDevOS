#include "fpu.h"
#include "term.h"

static inline void write_cr0(unsigned long val) {
    __asm__ volatile ("mov %0, %%cr0" :: "r"(val) : "memory");
}
static inline unsigned long read_cr0(void) {
    unsigned long val; __asm__ volatile ("mov %%cr0, %0" : "=r"(val)); return val;
}
static inline void write_cr4(unsigned long val) {
    __asm__ volatile ("mov %0, %%cr4" :: "r"(val) : "memory");
}
static inline unsigned long read_cr4(void) {
    unsigned long val; __asm__ volatile ("mov %%cr4, %0" : "=r"(val)); return val;
}

void fpu_sse_init(void) {
    /* Enable x87 and SSE */
    unsigned long cr0 = read_cr0();
    cr0 &= ~(1UL << 2);   /* CR0.EM = 0 (enable FPU) */
    cr0 |=  (1UL << 1);   /* CR0.MP = 1 (monitor coprocessor) */
    write_cr0(cr0);

    unsigned long cr4 = read_cr4();
    cr4 |= (1UL << 9);    /* CR4.OSFXSR (FXSAVE/FXRSTOR) */
    cr4 |= (1UL << 10);   /* CR4.OSXMMEXCPT (unmasked SSE exceptions) */
    write_cr4(cr4);

    /* Initialize FPU */
    __asm__ volatile ("fninit");
    /* Optionally set MXCSR default */
    unsigned int mxcsr = 0x1f80; /* default */
    __asm__ volatile ("ldmxcsr %0" :: "m"(mxcsr));

    printf("FPU/SSE initialized\n");

    /* Capture the clean default FPU state for use by new threads */
    fpu_save_default_state();
}

// Default (clean) FPU/SSE state captured at boot -- used to initialize new threads
static uint8_t default_fxsave_area[512] __attribute__((aligned(16)));
static bool default_fpu_state_saved = false;

void fpu_save_default_state(void) {
    __asm__ volatile("fxsave (%0)" : : "r"(default_fxsave_area) : "memory");
    default_fpu_state_saved = true;
}

void fpu_get_default_state(uint8_t *dest) {
    if (default_fpu_state_saved && dest) {
        for (int i = 0; i < 512; i++) {
            dest[i] = default_fxsave_area[i];
        }
    } else if (dest) {
        // Zero-fill as a fallback
        for (int i = 0; i < 512; i++) {
            dest[i] = 0;
        }
    }
}

static inline unsigned long read_cr0(void);
static inline unsigned long read_cr4(void);

void fpu_self_test(void) {
    printf("FPU self-test begin\n");
    unsigned long cr0 = read_cr0();
    unsigned long cr4 = read_cr4();
    printf("CR0=0x%lx CR4=0x%lx\n", cr0, cr4);

    /* Check EM=0 and MP=1 */
    printf("  CR0.EM=%lu (expect 0)  CR0.MP=%lu (expect 1)\n",
           (cr0 >> 2) & 1UL, (cr0 >> 1) & 1UL);
    /* Check OSFXSR and OSXMMEXCPT */
    printf("  CR4.OSFXSR=%lu (expect 1)  CR4.OSXMMEXCPT=%lu (expect 1)\n",
           (cr4 >> 9) & 1UL, (cr4 >> 10) & 1UL);

    /* x87 test: compute sqrt(81.0) via fld/fsqrt and store as int with fistp */
    double x = 81.0;
    int y_i = 0;
    __asm__ volatile (
        "fldl %1\n\t"
        "fsqrt\n\t"
        "fistpl %0\n\t"
        : "=m"(y_i)
        : "m"(x)
        : "memory"
    );
    printf("  x87 fsqrt(81.0) = %d (expect 9)\n", y_i);

    /* x87 test: (1.5 + 2.5) * 3.0 = 12.0, store as int */
    double a = 1.5, b = 2.5, c = 3.0;
    int r_i = 0;
    __asm__ volatile (
        "fldl %1\n\t"   /* st0 = a */
        "faddl %2\n\t"  /* st0 = a+b */
        "fmull %3\n\t"  /* st0 = (a+b)*c */
        "fistpl %0\n\t"  /* r_i = round(st0) */
        : "=m"(r_i)
        : "m"(a), "m"(b), "m"(c)
        : "memory"
    );
    printf("  x87 (1.5+2.5)*3.0 = %d (expect 12)\n", r_i);

    printf("FPU self-test end\n");
}





