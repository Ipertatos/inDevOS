#include "mem.h"
#include "utils.h"
#include "limine.h"
#include "macros.h"
uint64_t max_pages_available;

uint64_t total_mem_bytes;

volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0
};

volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0
};

volatile struct limine_memmap_response *memmap;
volatile struct limine_hhdm_response *hhdm;

uint64_t highest_address = 0;
uint64_t lowest_address = 0;

static uint64_t num_pages_in_map = 0;

static uint8_t *bitmap = NULL;

uint64_t vmm_higher_half_offset = 0;

uint64_t allocation_cursor = 0;
uint64_t num_pages_available = 0;

struct PageEntry{
    uint32_t pages_allocated;
} __attribute__((packed));

struct PageEntry* entry_map;
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


void _get_free_pages()
{
    printf("Getting free pages{n}");
    for(uint64_t i = 0; i < memmap->entry_count; i++)
    {
        struct limine_memmap_entry *entry = memmap->entries[i];
        if(entry->type != LIMINE_MEMMAP_USABLE)
            continue;
       
        uint64_t start_bit = (entry->base - lowest_address) / PAGE_SIZE;
        uint64_t end_bit = entry->length / PAGE_SIZE;
        for(uint64_t j = start_bit; j < end_bit; j++)
        {
            if(bitmap[j / 8] & (1 << (j % 8)))
                continue;
            bitmap[j / 8] |= (1 << (j % 8));
            num_pages_available++;
        }
    }
}

void _create_entry_map()
{
uint64_t map_size = ALIGN_UP(sizeof(struct PageEntry) * num_pages_in_map, PAGE_SIZE);
    printf("PageEntry Size: {d} bytes{n}", map_size);

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

    printf("Page Bitmap Size: {d} Kib{n}", bitmap_size / 1024);

    printf("Locating space for bitmap...{n}");
    
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
            bitmap = (uint8_t*)(entry->base + vmm_higher_half_offset);

            // Now set all the bits to 1 in the map to mark everything as taken to start with.
            memset(bitmap, 0xFF, bitmap_size);

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
    printf("-----------HHDM Offset: {x}{n}", vmm_higher_half_offset);

    // Gather some stats about the memory map so we know how big we need to make our
    // page bitmap.
    for (size_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *entry = memmap->entries[i];

        if (entry->type == LIMINE_MEMMAP_USABLE) {
            total_mem_bytes += entry->length;

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
        bitmap[i / 8] |= (1 << (i % 8));
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
    uint64_t pages_to_alloc = DIV_ROUNDUP(numBytes, PAGE_SIZE);
    uint64_t start_alloc_page = 0;
    uint64_t allocated_pages = 0;
    bool looped = false;
    bool allocating = false;
    bool page_state = false;

    // Try and allocate pages starting from the cursor.
    while(true) {
        page_state = bitmap_test(bitmap, allocation_cursor);

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
            return (void*)(lowest_address + (start_alloc_page * PAGE_SIZE) + vmm_higher_half_offset);
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

    printf("PMM Allocation failed.{n}");

    return NULL;
}

/*
    Initialize the physical memory manager.
*/
void kmem_init()
{
    printf("Initialzing PMM...{n}");

    _init_stats();

    printf("Total Memory: {d} Mib{n}", total_mem_bytes / 1024 / 1024);
    printf("Total Map Pages: {d}{n}", num_pages_in_map);
    printf("Total Pages Available: {d}{n}", max_pages_available);
    printf("Lowest Memory Addr: 0x{x}{n}", lowest_address);
    
    _create_page_bitmap();
    _create_entry_map();
    _get_free_pages();

    printf("PMM initialized.{n}");
}

/*
 * Frees the pages allocated from a previous kalloc() call.
*/
void kfree(void *ptr)
{

    uint64_t page_index = ((uint64_t)ptr - lowest_address - vmm_higher_half_offset) / PAGE_SIZE;
    
    if (page_index >= max_pages_available) {
        printf("*FATAL*: Page index lookup resulted in out of bounds value of {d} from address 0x{x}.{n}", page_index, ptr);
        while(1) __asm__("hlt");
    }

    // Turn off all the bits for all of the pages that were allocated.
    for (uint64_t i = page_index; i < page_index + entry_map[page_index].pages_allocated; i++) {
        bitmap[i / 8] &= ~(1 << (i % 8));
    }

    // Update the entry.
    entry_map[page_index].pages_allocated = 0;

}

// Dumps contents of the specified memory location in char format.
void memdumps(void *location, uint64_t len_bytes)
{
    char * chLocation = (char*)location;
    for (uint64_t i = 0; i < len_bytes; i++) {
        printf("%c", *chLocation++);
    }
    
    printf("{n}");
}

void memdumpx32(void *location, uint64_t len_bytes)
{
    uint32_t *data = (uint32_t*)location;
    for (uint64_t i = 0; i < (len_bytes / 4); i++) {
        printf("0x%X ", *data++);
    }
    
    printf("{n}");
}

void memdumpx64(void *location, uint64_t len_bytes)
{
    uint64_t *data = (uint64_t*)location;
    for (uint64_t i = 0; i < (len_bytes / 8); i++) {
        printf("0x%X ", *data++);
    }
    
    printf("{n}");
}