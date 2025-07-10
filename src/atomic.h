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
            __asm__ __volatile__("nop");
        }
    }
}

static inline void spinlock_unlock(spinlock_t *lock) {
    __sync_lock_release(&lock->lock);
}

// Atomic operations
static inline void atomic_set(atomic_t *atom, uint64_t value) {
    __sync_lock_test_and_set(&atom->value, value);
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