#pragma once

#include "types.h"

typedef struct {
    uint8_t lock;
} spinlock_t;

typedef struct {
    volatile uint64_t value;
} atomic_t;

static inline void spinlock_lock(spinlock_t *lock) __attribute__((always_inline));
static inline void spinlock_unlock(spinlock_t *lock) __attribute__((always_inline));

static inline void spinlock_lock(spinlock_t *lock) {
    while (__sync_lock_test_and_set(&lock->lock, 1)) {
        while (lock->lock) {
            __asm__ __volatile__("pause");
        }
    }
}

static inline void spinlock_unlock(spinlock_t *lock) {
    __sync_lock_release(&lock->lock);
}

/*
 * Per-CPU preemption disable.  While this CPU's counter is non-zero the
 * local timer/IPI will not switch threads.  A global counter used to
 * freeze every core whenever the BSP held a klock (VFS, FAT, net).
 */
#define PREEMPT_MAX_CPUS 16
extern volatile uint32_t preempt_count_cpu[PREEMPT_MAX_CPUS];
uint32_t smp_cpu_index_fast(void);

static inline void preempt_disable(void) {
    uint32_t c = smp_cpu_index_fast();
    if (c >= PREEMPT_MAX_CPUS) c = 0;
    __atomic_add_fetch(&preempt_count_cpu[c], 1, __ATOMIC_ACQUIRE);
}

static inline void preempt_enable(void) {
    uint32_t c = smp_cpu_index_fast();
    if (c >= PREEMPT_MAX_CPUS) c = 0;
    __atomic_sub_fetch(&preempt_count_cpu[c], 1, __ATOMIC_RELEASE);
}

/*
 * Sleep-free subsystem lock for thread context (not IRQ handlers).
 * Disabling preemption first means the holder can never be switched out
 * while a Ring-3 thread spins in a syscall (those are not preemptible),
 * which would otherwise deadlock the BSP.
 */
static inline void klock(spinlock_t *lock) {
    preempt_disable();
    spinlock_lock(lock);
}

static inline void kunlock(spinlock_t *lock) {
    spinlock_unlock(lock);
    preempt_enable();
}

// Atomic operations
static inline void atomic_set(atomic_t *atom, uint64_t value) {
    __atomic_store_n(&atom->value, value, __ATOMIC_SEQ_CST);
}

static inline uint64_t atomic_get(atomic_t *atom) {
    return __sync_fetch_and_add(&atom->value, 0);
}

static inline uint64_t atomic_inc(atomic_t *atom) {
    return __sync_fetch_and_add(&atom->value, 1);
}

static inline uint64_t atomic_dec(atomic_t *atom) {
    return __sync_fetch_and_sub(&atom->value, 1);
}

static inline uint64_t atomic_add(atomic_t *atom, uint64_t value) {
    return __sync_fetch_and_add(&atom->value, value);
}

static inline uint64_t atomic_sub(atomic_t *atom, uint64_t value) {
    return __sync_fetch_and_sub(&atom->value, value);
}

// Interrupt control functions
static inline void irq_disable(void) {
    __asm__ __volatile__("cli");
}

static inline void irq_enable(void) {
    __asm__ __volatile__("sti");
}

static inline uint64_t irq_save(void) {
    uint64_t flags;
    __asm__ __volatile__("pushf; pop %0" : "=r" (flags));
    __asm__ __volatile__("cli");
    return flags;
}

static inline void irq_restore(uint64_t flags) {
    __asm__ __volatile__("push %0; popf" : : "r" (flags));
}