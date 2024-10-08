/*
    BloreOS - Operating System
    Copyright (C) 2023 Martin Blore

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/
/*
    This is the physical memory manager, dealing with physical available memory only.
*/
#include "stdint.h"
#include "stdarg.h"
#include "stddef.h"
#include "mem.h"
#include "limine.h"
#include "bitmap.h"
#include "math.h"
#include "atomic.h"
spinlock_t lock = {0};

/* The maximum number of pages of available RAM across the entire memory map */
uint64_t max_pages_available;

uint64_t total_memory_bytes;

volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0};

volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0
};

volatile struct limine_memmap_response *memmap;
volatile struct limine_hhdm_response *hhdm;

/* Highest memory location found from all memory map entries */
uint64_t highest_address = 0;

/* Lowest memory location found from all memory map entries */
uint64_t lowest_address = 0;

/*
    Number of pages found between the lowest and highest memory locations
    reported in the memory map entries.
*/
static uint64_t num_pages_in_map = 0;

/*
    The page bitmap tracks all pages in the entire memory map from lowest and highest points
    in the memory map. A bit value of 0 marks a free page.
*/
static uint8_t *page_bitmap = NULL;

/* The location in virtual memory to the higher-half */
uint64_t vmm_higher_half_offset = 0;

/*
    The cursor tracks the last page index where we last allocated memory from.
    To save finding pages, we allocate forwards from this and loop back around
    if we go past the last page.
*/
uint64_t allocation_cursor = 0;

uint64_t num_pages_available = 0;

// The PageEntry helps us determine how pages our allocations reserve when they do a kalloc.
// This helps know how many pages to free when we come to free that memory.
// At 4 bytes per entry, the cost of this metadata is approx. 2MB for a 2GB system.
struct PageEntry {
    uint32_t pages_allocated;  // Number of pages allocated starting at this entry.
} __attribute__((packed));

// Contains a PageEntry item per page in memory.
struct PageEntry* entry_map;

// GCC and Clang reserve the right to generate calls to the following
// 4 functions even if they are not directly called.
// Implement them as the C specification mandates.
// DO NOT remove or rename these functions, or stuff will eventually break!
void *memcpy(void *dest, const void *src, size_t n)
{
    uint8_t *pdest = (uint8_t *)dest;
    const uint8_t *psrc = (const uint8_t *)src;

    for (size_t i = 0; i < n; i++)
    {
        pdest[i] = psrc[i];
    }

    return dest;
}

void *memset(void *s, int c, size_t n)
{
    uint8_t *p = (uint8_t *)s;

    for (size_t i = 0; i < n; i++)
    {
        p[i] = (uint8_t)c;
    }

    return s;
}

void *memmove(void *dest, const void *src, size_t n)
{
    uint8_t *pdest = (uint8_t *)dest;
    const uint8_t *psrc = (const uint8_t *)src;

    if (src > dest)
    {
        for (size_t i = 0; i < n; i++)
        {
            pdest[i] = psrc[i];
        }
    }
    else if (src < dest)
    {
        for (size_t i = n; i > 0; i--)
        {
            pdest[i - 1] = psrc[i - 1];
        }
    }

    return dest;
}

int memcmp(const void *s1, const void *s2, size_t n)
{
    const uint8_t *p1 = (const uint8_t *)s1;
    const uint8_t *p2 = (const uint8_t *)s2;

    for (size_t i = 0; i < n; i++)
    {
        if (p1[i] != p2[i])
        {
            return p1[i] < p2[i] ? -1 : 1;
        }
    }

    return 0;
}

/*
 * Prints the state of the bitmap at the specified indices.
*/
void _print_bitmap(uint64_t start, uint64_t len)
{
    printf("Bitmap (%d-%d): ", start, start+len);
    for (uint64_t i = start; i < start+len; i++) {
        printf(bitmap_test(page_bitmap, i) ? "1" : "0");
    }
    printf("\n");
}

/*
 * Walks the memory map entries, setting the state of the bitmap with what it finds as usable memory. 
*/
void _get_free_pages()
{
    printf("Searching for free memory pages...\n");

    // So we have a memory map allocated and its all full. Let's walk through the memory map entries again
    // and start making pages available.
    for (size_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];

        if (entry->type != LIMINE_MEMMAP_USABLE) {
            continue;
        }

        // Find the page this entry refers to in our bitmap.
        uint64_t start_bit = (entry->base - lowest_address) / PAGE_SIZE;
        uint64_t pages_free = entry->length / PAGE_SIZE;

        // Marks all the pages as free for the size of this memory map entry.
        for (uint64_t i = start_bit; i < start_bit + pages_free; i++) {
            bitmap_off(page_bitmap, i);
            num_pages_available++;
        }
    }
}

/*
 * Finds memory to store the entry map, allocates it, and store a pointer to it.
*/
void _create_entry_map()
{
    uint64_t map_size = ALIGN_UP(sizeof(struct PageEntry) * num_pages_in_map, PAGE_SIZE);
    printf("PageEntry Size: %lu bytes\n", map_size);

    for (size_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];

        if (entry->type != LIMINE_MEMMAP_USABLE) {
            continue;
        }

        if (entry->length >= map_size) {
            // We've got a spot, lets point there.
            entry_map = (struct PageEntry*)(entry->base + vmm_higher_half_offset);

            memset(entry_map, 0, map_size);

            // Change the values in the limine map as this part of mem is now permanently allocated to our kernel.
            entry->length -= map_size;
            entry->base += map_size;
            max_pages_available -= (map_size / PAGE_SIZE);
            break;
        }
    }
}

void _create_page_bitmap()
{
    // Convert page numbers (bits) to bytes (8 bits).
    // The block size for the bitmap aligns to the page size even though we might not need that much.
    uint64_t bitmap_size = ALIGN_UP(num_pages_in_map / 8, PAGE_SIZE);

    printf("Page Bitmap Size: %lu Kib\n", bitmap_size / 1024);

    printf("Locating space for bitmap...\n");
    
    // Now we know how big our page map needs to be, let's find a spare place in memory
    // to keep it.
    for (size_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];

        if (entry->type != LIMINE_MEMMAP_USABLE) {
            continue;
        }

        // Is this entry big enough to contain our bitmap?
        if (entry->length >= bitmap_size) {
            // We've got a spot, lets point there.
            // We have to use the HHDM offset to correctly point to this physical place in virtual memory.
            page_bitmap = (uint8_t*)(entry->base + vmm_higher_half_offset);

            // Now set all the bits to 1 in the map to mark everything as taken to start with.
            memset(page_bitmap, 0xFF, bitmap_size);

            // Change the values in the limine map as this part of mem is now permanently allocated to our kernel.
            entry->length -= bitmap_size;
            entry->base += bitmap_size;
            max_pages_available -= (bitmap_size / PAGE_SIZE);
            break;
        }
    }
}

void _init_stats()
{
    memmap = memmap_request.response;
    hhdm = hhdm_request.response;
    vmm_higher_half_offset = hhdm->offset;

    // Gather some stats about the memory map so we know how big we need to make our
    // page bitmap.
    for (size_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];

        if (entry->type == LIMINE_MEMMAP_USABLE) {
            total_memory_bytes += entry->length;

            // In this entry, how many pages are there?
            max_pages_available += entry->length / PAGE_SIZE;

            highest_address = MAX(highest_address, entry->base + entry->length);

            if (lowest_address == 0) {
                lowest_address = entry->base;
            } else {
                lowest_address = MIN(lowest_address, entry->base);
            }
        }
    }

    uint64_t map_size_bytes = highest_address - lowest_address;
    num_pages_in_map = map_size_bytes / PAGE_SIZE;
}

inline void* _get_addr_from_page(uint64_t page)
{
    return (void*)(lowest_address + (page * PAGE_SIZE) + vmm_higher_half_offset);
}

inline uint64_t _get_page_from_addr(void* addr)
{
    return ((uint64_t)addr - lowest_address - vmm_higher_half_offset) / PAGE_SIZE;
}

void _reserve_pages(uint64_t startPage, uint64_t pages)
{
    for (uint64_t i = startPage; i < startPage + pages; i++) {
        bitmap_on(page_bitmap, i);
    }
}

/*
    Allocates a requested amount of contiguous pages of physical memory.
    
    If the amount of pages could not be allocated contiguously, the call will fail
    returning NULL. But this does not mean that we are out of memory. It may be that
    the remaining pages of memory are fragmented in blocks that only allow for small
    amounts of contiguous page allocation. You would have to look in to the memory map
    entries to determine this.
*/
void* kalloc(size_t numBytes)
{
    spinlock_lock(&lock);

    uint64_t pages_to_alloc = DIV_ROUNDUP(numBytes, PAGE_SIZE);
    uint64_t start_alloc_page = 0;
    uint64_t allocated_pages = 0;
    bool looped = false;
    bool allocating = false;
    bool page_state = false;

    // Try and allocate pages starting from the cursor.
    while(true) {
        page_state = bitmap_test(page_bitmap, allocation_cursor);

        if (!allocating) {
            if (!page_state) {
                // Found a memory slot, lets start trying to allocate from here.
                start_alloc_page = allocation_cursor;
                allocating = true;
                allocated_pages = 1;
            }
        } else {
            if (page_state) {
                // This page is taken, back to seeking.
                allocating = false;
            } else {
                // Free page.
                allocated_pages++;
            }
        }

        if (allocated_pages == pages_to_alloc) {
            // We have enough memory now.
            // Advance the cursor for the next alloc start.
            allocation_cursor++;
            if (allocation_cursor == max_pages_available) {
                allocation_cursor = 0;
            }

            // Reserve it in the map and return the location.
            _reserve_pages(start_alloc_page, allocated_pages);
            
            // Update the entry map to track how many pages we allocated from here.
            entry_map[start_alloc_page].pages_allocated = allocated_pages;

            num_pages_available -= allocated_pages;
            spinlock_unlock(&lock);
            return _get_addr_from_page(start_alloc_page);
        }

        // Forward the cursor and bounds check.
        allocation_cursor++;

        if (allocation_cursor == max_pages_available) {
            allocation_cursor = 0;

            if (looped) {
                // If we've looped around already, we failed to find
                // a suitable hole in the memory map.
                break;
            }
            
            // Start looking from the start and back to seeking
            // as we break the contiguous allocation.
            looped = true;
            allocating = false;
        }
    }

    // Failed to allocate getting this far.
    spinlock_unlock(&lock);

    printf("PMM Allocation failed.\n");

    return NULL;
}

/*
    Initialize the physical memory manager.
*/
void kmem_init()
{
    printf("Initialzing PMM...\n");

    _init_stats();

    printf("Total Memory: %lu Mib\n", total_memory_bytes / 1024 / 1024);
    printf("Total Map Pages: %lu\n", num_pages_in_map);
    printf("Total Pages Available: %lu\n", max_pages_available);
    printf("Lowest Memory Addr: 0x%X\n", lowest_address);
    
    _create_page_bitmap();
    _create_entry_map();
    _get_free_pages();

    printf("PMM initialized.\n");
}

/*
 * Frees the pages allocated from a previous kalloc() call.
*/
void kfree(void *ptr)
{
    spinlock_lock(&lock);

    uint64_t page_index = _get_page_from_addr(ptr);
    
    if (page_index >= max_pages_available) {
        printf("*FATAL*: Page index lookup resulted in out of bounds value of %lu from address 0x%X.\n", page_index, ptr);
        while (1)
        {
            __asm__("hlt");
        }
        
    }

    // Turn off all the bits for all of the pages that were allocated.
    for (uint64_t i = page_index; i < page_index + entry_map[page_index].pages_allocated; i++) {
        bitmap_off(page_bitmap, i);
    }

    // Update the entry.
    entry_map[page_index].pages_allocated = 0;

    spinlock_unlock(&lock);
}

// Dumps contents of the specified memory location in char format.
void memdumps(void *location, uint64_t len_bytes)
{
    char * chLocation = (char*)location;
    for (uint64_t i = 0; i < len_bytes; i++) {
        printf("%c", *chLocation++);
    }
    
    printf("\n");
}

void memdumpx32(void *location, uint64_t len_bytes)
{
    uint32_t *data = (uint32_t*)location;
    for (uint64_t i = 0; i < (len_bytes / 4); i++) {
        printf("0x%X ", *data++);
    }
    
    printf("\n");
}

void memdumpx64(void *location, uint64_t len_bytes)
{
    uint64_t *data = (uint64_t*)location;
    for (uint64_t i = 0; i < (len_bytes / 8); i++) {
        printf("0x%X ", *data++);
    }
    
    printf("\n");
}