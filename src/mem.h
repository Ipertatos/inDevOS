#pragma once

#include "limine.h"
#include "types.h"

#define PAGE_SIZE 4096

extern volatile struct limine_memmap_response *memmap;

extern uint64_t total_memory_bytes;
extern uint64_t max_pages_available;
extern uint64_t num_pages_available;
extern uint64_t vmm_higher_half_offset;

void kmem_init();
void* kalloc(size_t numBytes);
void kfree(void *ptr);

void memdumps(void *location, uint64_t len_bytes);
void memdumpx32(void *location, uint64_t len_bytes);
void memdumpx64(void *location, uint64_t len_bytes);

void *memset(void *s, int c, size_t n);
void *memmove(void *dest, const void *src, size_t n);
int memcmp(const void *s1, const void *s2, size_t n);

void* _get_addr_from_page(uint64_t page);
uint64_t _get_page_from_addr(void* addr);