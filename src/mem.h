#pragma once

#include "limine.h"
#include "types.h"

#define PAGE_SIZE 4096

extern volatile struct limine_memmap_response *memmap;

extern uint64_t total_mem_bytes;
extern uint64_t max_pages_available;
extern uint64_t num_pages_available;
extern uint64_t vmm_higher_half_offset;

static inline bool bitmap_test(const void *bitmap, size_t bit) {
    const uint8_t *bitmap_u8 = (const uint8_t*)bitmap;

    size_t byte_index = bit / 8;
    uint8_t bit_mask = 1 << (bit % 8);

    return (bitmap_u8[byte_index] & bit_mask) != 0;
}

void kmem_init();
void* kalloc(uint64_t size);
void kfree(void* ptr);
void *malloc(size_t size);

void memdumps(void *location, uint64_t size);
void memdumpx32(void *location, uint64_t size);
void memdumpx64(void *location, uint64_t size);

void *memset(void *s, int c, uint64_t n);
void *memmove(void *dest, const void *src, uint64_t n);
int memcmp(const void *dest, const void *src, uint64_t n);

void* _get_addr_from_page(uint64_t page);
uint64_t _get_page_from_addr(void* addr);
