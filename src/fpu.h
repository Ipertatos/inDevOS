#ifndef FPU_H
#define FPU_H

#include "types.h"

/* Enable x87 FPU and SSE in CR0/CR4 and initialize the FP unit. */
void fpu_sse_init(void);

/* Run a self-test of the x87 FPU and SSE state and print diagnostics. */
void fpu_self_test(void);

/* Capture the default (clean) FPU/SSE state into a 512-byte buffer.
 * Call once after fpu_sse_init() to create a template for new threads. */
void fpu_save_default_state(void);

/* Copy the clean default FPU state into 'dest' (must be 16-byte aligned, 512 bytes). */
void fpu_get_default_state(uint8_t *dest);

/* Save current FPU/SSE state into 'area' (must be 16-byte aligned, 512 bytes). */
static inline void fpu_save(uint8_t *area) {
    __asm__ volatile("fxsave (%0)" : : "r"(area) : "memory");
}

/* Restore FPU/SSE state from 'area' (must be 16-byte aligned, 512 bytes). */
static inline void fpu_restore(uint8_t *area) {
    __asm__ volatile("fxrstor (%0)" : : "r"(area) : "memory");
}

#endif /* FPU_H */






