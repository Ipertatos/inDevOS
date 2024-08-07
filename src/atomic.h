#pragma once

#include "types.h"

typedef struct {
    uint8_t lock;
} spinlock_t;

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