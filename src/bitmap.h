#pragma once

#include "types.h"

static inline bool bitmap_test(const void *bitmap, size_t bit) {
    const uint8_t *bitmap_u8 = (const uint8_t*)bitmap;

    size_t byte_index = bit / 8;
    uint8_t bit_mask = 1 << (bit % 8);

    return (bitmap_u8[byte_index] & bit_mask) != 0;
}

static inline void bitmap_on(void *bitmap, size_t bit) {
    uint8_t *bitmap_u8 = (uint8_t*)bitmap;
    bitmap_u8[bit / 8] |= (1 << (bit % 8));
}

static inline void bitmap_off(void *bitmap, size_t bit) {
    uint8_t *bitmap_u8 = (uint8_t*)bitmap;
    bitmap_u8[bit / 8] &= ~(1 << (bit % 8));
}