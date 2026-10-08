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
#include "serial.h"
#include "smp.h"
#include "debug.h"
#include "term.h"
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

/* Serializes the page bitmap / entry_map / cursor across CPUs.
 * Always taken with local IRQs disabled (irq_save) to stay ISR-safe. */
static spinlock_t pmm_lock = {0};

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

/**
 * @brief Memory copy using x86 string instructions
 *
 * `rep movsq` + `rep movsb` handles any alignment (x86 permits unaligned
 * qword access) and is the fastest generic copy on ERMS/FSRM CPUs; QEMU TCG
 * also executes string ops without leaving the translation block.
 * Requires DF=0, which the ISR/syscall entry paths guarantee.
 */
void *memcpy(void *dest, const void *src, size_t n)
{
    void *d = dest;
    if (n >= 32) {
        size_t qwords = n >> 3;
        __asm__ volatile("rep movsq"
                         : "+D"(d), "+S"(src), "+c"(qwords)
                         :
                         : "memory");
        n &= 7;
    }
    __asm__ volatile("rep movsb"
                     : "+D"(d), "+S"(src), "+c"(n)
                     :
                     : "memory");
    return dest;
}

/**
 * @brief Memory fill using x86 string instructions (see memcpy).
 */
void *memset(void *s, int c, size_t n)
{
    void *d = s;
    if (n >= 32) {
        uint64_t pattern = (uint8_t)c;
        pattern *= 0x0101010101010101ULL;
        size_t qwords = n >> 3;
        __asm__ volatile("rep stosq"
                         : "+D"(d), "+c"(qwords)
                         : "a"(pattern)
                         : "memory");
        n &= 7;
    }
    __asm__ volatile("rep stosb"
                     : "+D"(d), "+c"(n)
                     : "a"(c)
                     : "memory");
    return s;
}

/**
 * @brief Memory move with overlap handling
 *
 * Forward copies go through memcpy.  Backward (dest overlaps the tail of
 * src) copies qwords from the end, then the leftover head bytes.
 */
void *memmove(void *dest, const void *src, size_t n)
{
    uint8_t *pdest = (uint8_t *)dest;
    const uint8_t *psrc = (const uint8_t *)src;

    if (pdest == psrc || n == 0)
        return dest;

    if (psrc > pdest || psrc + n <= pdest)
        return memcpy(dest, src, n);

    pdest += n;
    psrc += n;
    while (n >= 8) {
        pdest -= 8;
        psrc -= 8;
        uint64_t v;
        __builtin_memcpy(&v, psrc, 8);
        __builtin_memcpy(pdest, &v, 8);
        n -= 8;
    }
    while (n--)
        *--pdest = *--psrc;

    return dest;
}

/**
 * @brief Optimized memory comparison
 */
int memcmp(const void *s1, const void *s2, size_t n)
{
    const uint8_t *p1 = (const uint8_t *)s1;
    const uint8_t *p2 = (const uint8_t *)s2;

    // For aligned pointers and large sizes, compare 64 bits at a time
    if (n >= 8 && ((uintptr_t)p1 | (uintptr_t)p2) % 8 == 0) {
        const uint64_t *q1 = (const uint64_t *)p1;
        const uint64_t *q2 = (const uint64_t *)p2;
        
        while (n >= 8) {
            if (*q1 != *q2) {
                // Found difference, compare bytes
                p1 = (const uint8_t *)q1;
                p2 = (const uint8_t *)q2;
                for (int i = 0; i < 8; i++) {
                    if (p1[i] != p2[i]) {
                        return p1[i] < p2[i] ? -1 : 1;
                    }
                }
            }
            q1++;
            q2++;
            n -= 8;
        }
        
        p1 = (const uint8_t *)q1;
        p2 = (const uint8_t *)q2;
    }

    // Compare remaining bytes
    while (n--) {
        if (*p1 != *p2) {
            return *p1 < *p2 ? -1 : 1;
        }
        p1++;
        p2++;
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
    
    // Debug HHDM setup
    printf("HHDM Debug: hhdm_request.response = %p\n", hhdm);
    if (hhdm) {
        printf("HHDM Debug: hhdm->offset = 0x%llx\n", hhdm->offset);
        vmm_higher_half_offset = hhdm->offset;
        
        // Serial debug with working format
        serial_printf("HHDM: response=%p, offset=0x%llx\n", 
                     hhdm, hhdm->offset);
    } else {
        printf("HHDM Debug: hhdm is NULL - using fallback offset\n");
        vmm_higher_half_offset = 0xFFFF800000000000ULL;  // Standard higher half offset
        serial_printf("HHDM: NULL response, using fallback 0xFFFF800000000000\n");
    }

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
    Internal allocation function that handles the common allocation logic.
    
    @param numBytes Number of bytes to allocate
    @param start_page Minimum page number to start allocation from (0 for any)
    @param use_global_cursor If true, use and update the global allocation cursor
    @param caller_name Name of calling function for debug messages
    
    Returns pointer to allocated memory or NULL on failure.
*/
static void* _kalloc_internal(size_t numBytes, uint64_t start_page, 
                               bool use_global_cursor, const char *caller_name)
{
    // Security: Prevent integer overflow in allocation size calculation
    if (numBytes == 0) {
        return NULL;  // Cannot allocate zero bytes
    }

    // Security: Prevent allocation of extremely large sizes that could cause overflow
    if (numBytes > (SIZE_MAX / 2)) {
        printf("%s: Allocation size too large: %zu bytes\n", caller_name, numBytes);
        return NULL;
    }

    /* IRQ-disable serializes against ISRs on this CPU; the spinlock
     * serializes against other CPUs (APs). Both are required for SMP. */
    uint64_t flags = irq_save();
    spinlock_lock(&pmm_lock);

    uint64_t pages_to_alloc = DIV_ROUNDUP(numBytes, PAGE_SIZE);

    /* max_pages_available is only filled in once the PMM has walked the memory
     * map, so zero means kalloc() ran before kmem_init().  Say so explicitly --
     * the "max allowed: 0" form of the size check below made that failure look
     * like an oversized allocation. */
    if (max_pages_available == 0) {
        printf("%s: kalloc(%zu bytes) before PMM initialisation; cannot allocate\n",
               caller_name, numBytes);
        spinlock_unlock(&pmm_lock);
        irq_restore(flags);
        return NULL;
    }

    // Security: Prevent allocation of too many pages (potential DoS)
    if (pages_to_alloc > max_pages_available / 4) {
        printf("%s: Allocation too large (%lu pages, max allowed: %lu)\n",
               caller_name, pages_to_alloc, max_pages_available / 4);
        spinlock_unlock(&pmm_lock);
        irq_restore(flags);
        return NULL;
    }

    uint64_t start_alloc_page = 0;
    uint64_t allocated_pages = 0;
    bool looped = false;
    bool allocating = false;
    bool page_state = false;

    // Set up cursor - either use global or local starting from start_page
    uint64_t cursor = use_global_cursor ? allocation_cursor : start_page;
    uint64_t reset_value = start_page;  // Value to reset to when wrapping

    // Try and allocate pages starting from the cursor
    while(true) {
        page_state = bitmap_test(page_bitmap, cursor);

        if (!allocating) {
            if (!page_state) {
                // Found a memory slot, start trying to allocate from here
                start_alloc_page = cursor;
                allocating = true;
                allocated_pages = 1;
            }
        } else {
            if (page_state) {
                // This page is taken, back to seeking
                allocating = false;
            } else {
                // Free page
                allocated_pages++;
            }
        }

        if (allocated_pages == pages_to_alloc) {
            // We have enough memory now
            
            // Advance the global cursor for next alloc (only if using global cursor)
            if (use_global_cursor) {
                allocation_cursor = cursor + 1;
                if (allocation_cursor >= num_pages_in_map) {
                    allocation_cursor = 0;
                }
            }

            // Reserve it in the map
            _reserve_pages(start_alloc_page, allocated_pages);
            
            // Update the entry map to track allocation size
            entry_map[start_alloc_page].pages_allocated = allocated_pages;

            num_pages_available -= allocated_pages;
            spinlock_unlock(&pmm_lock);
            irq_restore(flags);
            return _get_addr_from_page(start_alloc_page);
        }

        // Forward the cursor and bounds check
        cursor++;

        if (cursor >= num_pages_in_map) {
            if (looped) {
                // Already looped around - failed to find suitable memory
                break;
            }
            
            // Reset cursor and start over
            cursor = reset_value;
            looped = true;
            allocating = false;
        }
    }

    // Failed to allocate
    spinlock_unlock(&pmm_lock);
    irq_restore(flags);

    printf("%s: Allocation failed for %zu bytes\n", caller_name, numBytes);
    return NULL;
}

/*
    Allocates a requested amount of contiguous pages of physical memory.
    
    If the amount of pages could not be allocated contiguously, the call will fail
    returning NULL. But this does not mean that we are out of memory. It may be that
    the remaining pages of memory are fragmented in blocks that only allow for small
    amounts of contiguous page allocation. You would have to look in to the memory map
    entries to determine this.
*/
/* ---------------------------------------------------------------------------
 * Slab layer for small kalloc() requests.
 *
 * Requests up to SLAB_MAX_SIZE bytes are served from power-of-two size
 * classes carved out of whole pages.  Objects are naturally aligned to their
 * class size and never straddle a page, so small DMA structures (AHCI command
 * lists, virtio rings, ...) keep the alignment and physical contiguity they
 * got from whole-page allocations.
 *
 * A slab page is tagged in entry_map: bit 31 set, class index in bits 24-27,
 * live object count in bits 0-15.  An empty page is returned to the PMM once
 * its class already holds a page's worth of spare objects.
 * ------------------------------------------------------------------------- */
#define SLAB_MIN_SHIFT   5                       /* 32 bytes */
#define SLAB_MAX_SHIFT   11                      /* 2048 bytes */
#define SLAB_MAX_SIZE    (1u << SLAB_MAX_SHIFT)
#define SLAB_CLASSES     (SLAB_MAX_SHIFT - SLAB_MIN_SHIFT + 1)
#define SLAB_TAG         0x80000000u
#define SLAB_CLASS(e)    (((e) >> 24) & 0xF)
#define SLAB_INUSE(e)    ((e) & 0xFFFF)

typedef struct slab_free_obj {
    struct slab_free_obj *next;
} slab_free_obj_t;

static slab_free_obj_t *slab_free_list[SLAB_CLASSES];
static uint32_t         slab_free_count[SLAB_CLASSES];
static spinlock_t       slab_lock = {0};

static inline int slab_class_for(size_t n)
{
    int shift = SLAB_MIN_SHIFT;
    while ((1ul << shift) < n)
        shift++;
    return shift - SLAB_MIN_SHIFT;
}

static inline void *slab_page_base(void *obj)
{
    return (void *)((uintptr_t)obj & ~(uintptr_t)(PAGE_SIZE - 1));
}

/* Unlink every free object that lives in `page` (called with slab_lock). */
static void slab_unlink_page(int cls, void *page)
{
    slab_free_obj_t **pp = &slab_free_list[cls];
    while (*pp) {
        if (slab_page_base(*pp) == page) {
            *pp = (*pp)->next;
            slab_free_count[cls]--;
        } else {
            pp = &(*pp)->next;
        }
    }
}

static void *slab_alloc(int cls)
{
    uint32_t size = 1u << (cls + SLAB_MIN_SHIFT);

    uint64_t flags = irq_save();
    spinlock_lock(&slab_lock);
    if (!slab_free_list[cls]) {
        spinlock_unlock(&slab_lock);
        irq_restore(flags);

        uint8_t *page = _kalloc_internal(PAGE_SIZE, 0, true, "kalloc");
        if (!page)
            return NULL;

        flags = irq_save();
        spinlock_lock(&slab_lock);
        entry_map[_get_page_from_addr(page)].pages_allocated =
            SLAB_TAG | ((uint32_t)cls << 24);
        for (uint32_t off = 0; off < PAGE_SIZE; off += size) {
            slab_free_obj_t *o = (slab_free_obj_t *)(page + off);
            o->next = slab_free_list[cls];
            slab_free_list[cls] = o;
            slab_free_count[cls]++;
        }
    }

    slab_free_obj_t *obj = slab_free_list[cls];
    slab_free_list[cls] = obj->next;
    slab_free_count[cls]--;
    entry_map[_get_page_from_addr(obj)].pages_allocated++;
    spinlock_unlock(&slab_lock);
    irq_restore(flags);
    return obj;
}

static void slab_free(void *ptr, uint64_t page_index)
{
    uint64_t flags = irq_save();
    spinlock_lock(&slab_lock);

    uint32_t e = entry_map[page_index].pages_allocated;
    int cls = SLAB_CLASS(e);
    uint32_t size = 1u << (cls + SLAB_MIN_SHIFT);
    void *page = slab_page_base(ptr);

    if (!(e & SLAB_TAG) || SLAB_INUSE(e) == 0 ||
        ((uintptr_t)ptr - (uintptr_t)page) % size != 0) {
        spinlock_unlock(&slab_lock);
        irq_restore(flags);
        printf("kfree: bad slab free %p\n", ptr);
        return;
    }

    slab_free_obj_t *o = (slab_free_obj_t *)ptr;
    o->next = slab_free_list[cls];
    slab_free_list[cls] = o;
    slab_free_count[cls]++;
    entry_map[page_index].pages_allocated = --e;

    uint32_t per_page = PAGE_SIZE / size;
    bool release = SLAB_INUSE(e) == 0 && slab_free_count[cls] >= 2 * per_page;
    if (release) {
        slab_unlink_page(cls, page);
        entry_map[page_index].pages_allocated = 1;
    }
    spinlock_unlock(&slab_lock);
    irq_restore(flags);

    if (release)
        kfree(page);
}

void* kalloc(size_t numBytes)
{
    if (numBytes > 0 && numBytes <= SLAB_MAX_SIZE && max_pages_available != 0)
        return slab_alloc(slab_class_for(numBytes));
    return _kalloc_internal(numBytes, 0, true, "kalloc");
}

/*
    Allocates memory above 1MB for network buffers that require higher addresses.
    This is specifically for RTL8139 and other network cards that cannot access
    memory below 1MB.
*/
void* kalloc_high(size_t numBytes)
{
    // Calculate the page number that corresponds to 1MB
    uint64_t one_mb_page = (0x100000 - lowest_address) / PAGE_SIZE;
    
    void* result = _kalloc_internal(numBytes, one_mb_page, false, "kalloc_high");
    
    if (result) {
        printf("kalloc_high: Allocated %zu bytes at 0x%lx (phys: 0x%lx)\n", 
               numBytes, (uint64_t)result, (uint64_t)result - vmm_higher_half_offset);
    }
    
    return result;
}

/*
    Initialize the physical memory manager.
*/
void kmem_init()
{
    // NOTE: Removed DEBUG macros as they call printf() which is unsafe during early init
    printf("Initialzing PMM...\n");

    // Use serial debug instead of printf-based DEBUG macros during early init
    DEBUG_SERIAL("Initializing memory statistics\n");
    _init_stats();

    printf("Total Memory: %lu Mib\n", total_memory_bytes / 1024 / 1024);
    printf("Total Map Pages: %lu\n", num_pages_in_map);
    printf("Total Pages Available: %lu\n", max_pages_available);
    printf("Lowest Memory Addr: 0x%llx\n", lowest_address);
    
    DEBUG_SERIAL("Creating page bitmap\n");
    _create_page_bitmap();
    DEBUG_SERIAL("Creating entry map\n");
    _create_entry_map();
    DEBUG_SERIAL("Getting free pages\n");
    _get_free_pages();

    printf("PMM initialized.\n");
}

/*
 * Frees the pages allocated from a previous kalloc() call.
*/
void kfree(void *ptr)
{
    // Security: Validate pointer is not NULL
    if (!ptr) {
        printf("kfree: Attempted to free NULL pointer\n");
        return;
    }

    uint64_t page_index = _get_page_from_addr(ptr);
    if (page_index < num_pages_in_map &&
        (entry_map[page_index].pages_allocated & SLAB_TAG)) {
        slab_free(ptr, page_index);
        return;
    }

    uint64_t flags = irq_save();
    spinlock_lock(&pmm_lock);

    // Security: Validate page index bounds
    if (page_index >= num_pages_in_map) {
        printf("kfree: Invalid page index %lu from address %p (max: %lu)\n",
               page_index, ptr, num_pages_in_map);
        spinlock_unlock(&pmm_lock);
        irq_restore(flags);
        return;
    }

    // Security: Check if the page was actually allocated
    if (entry_map[page_index].pages_allocated == 0) {
        printf("kfree: Attempted to free unallocated or already freed memory at %p\n", ptr);
        spinlock_unlock(&pmm_lock);
        irq_restore(flags);
        return;
    }

    // Security: Validate that all pages in the allocation are within bounds
    uint64_t pages_to_free = entry_map[page_index].pages_allocated;
    if (page_index + pages_to_free > num_pages_in_map) {
        printf("kfree: Allocation extends beyond memory bounds at %p\n", ptr);
        spinlock_unlock(&pmm_lock);
        irq_restore(flags);
        return;
    }

    // Turn off all the bits for all of the pages that were allocated.
    for (uint64_t i = page_index; i < page_index + pages_to_free; i++) {
        bitmap_off(page_bitmap, i);
    }

    // Update the entry and available page count
    entry_map[page_index].pages_allocated = 0;
    num_pages_available += pages_to_free;

    DEBUG_SERIAL("kfree: Freed %lu pages at address %p\n", pages_to_free, ptr);
    spinlock_unlock(&pmm_lock);
    irq_restore(flags);
}

/*
 * Returns the usable size (in bytes) of an allocation made with kalloc(),
 * or 0 if the pointer does not refer to a live allocation. Needed by
 * realloc() in libc_shim.c so it can copy the old contents.
 */
uint64_t kalloc_usable_size(void *ptr)
{
    if (!ptr) return 0;

    uint64_t flags = irq_save();
    spinlock_lock(&pmm_lock);

    uint64_t page_index = _get_page_from_addr(ptr);
    uint64_t size = 0;
    if (page_index < num_pages_in_map) {
        uint32_t e = entry_map[page_index].pages_allocated;
        if (e & SLAB_TAG)
            size = 1u << (SLAB_CLASS(e) + SLAB_MIN_SHIFT);
        else
            size = (uint64_t)e * PAGE_SIZE;
    }

    spinlock_unlock(&pmm_lock);
    irq_restore(flags);
    return size;
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
        printf("0x%08x ", *data++);
    }
    
    printf("\n");
}

void memdumpx64(void *location, uint64_t len_bytes)
{
    uint64_t *data = (uint64_t*)location;
    for (uint64_t i = 0; i < (len_bytes / 8); i++) {
        printf("0x%016llx ", *data++);
    }

    printf("\n");
}

// ============================================================================
// Virtual Memory Manager (VMM) - x86-64 4-level paging
// ============================================================================

// Global page table root (PML4) - set by Limine bootloader
static uint64_t *kernel_pml4 = NULL;

uint64_t* vmm_get_kernel_pml4(void) {
    return kernel_pml4;
}

// Allocate a zeroed physical frame (page table or user page)
uint64_t vmm_alloc_frame(void) {
    void *page = kalloc(PAGE_SIZE);
    if (!page) return 0;
    memset(page, 0, PAGE_SIZE);
    uint64_t phys = virt_to_phys(page);
    /* A freshly allocated frame has no PTE owners yet: reset its COW
     * refcount.  Release paths park a freed frame at 255 ("already freed",
     * so a stale second release is a no-op), but nothing cleared that when
     * the PMM handed the frame out again -- the next owner then saw a
     * saturated count and the frame was never freed (leaked on every
     * teardown, and fork's vmm_page_ref() made it immortal). */
    if (vmm_page_refcounts) {
        uint64_t pfn = phys / PAGE_SIZE;
        if (pfn < vmm_max_phys_pages)
            __atomic_store_n(&vmm_page_refcounts[pfn], 0, __ATOMIC_RELEASE);
    }
    return phys;
}

void vmm_free_frame(uint64_t phys_addr) {
    if (phys_addr == 0) return;
    kfree(phys_to_virt(phys_addr));
}

/**
 * Walk one level of the page table hierarchy.
 * If 'create' is true, allocates missing intermediate tables.
 * 'parent_flags' are OR'd into newly created entries (e.g. VMM_USER).
 */
static uint64_t* vmm_get_next_level(uint64_t *table, uint64_t index,
                                     bool create, uint64_t parent_flags) {
    if (table[index] & VMM_PRESENT) {
        return (uint64_t*)phys_to_virt(table[index] & PTE_ADDR_MASK);
    }
    if (!create) return NULL;

    uint64_t new_phys = vmm_alloc_frame();
    if (!new_phys) return NULL;

    // Intermediate entries are always present + writable; add user bit if requested
    table[index] = new_phys | VMM_PRESENT | VMM_WRITABLE | parent_flags;
    return (uint64_t*)phys_to_virt(new_phys);
}

/**
 * @brief Initialize VMM by reading the bootloader's page tables from CR3.
 */
void vmm_init(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));

    kernel_pml4 = (uint64_t*)phys_to_virt(cr3 & PTE_ADDR_MASK);

    printf("VMM: Kernel PML4 at phys 0x%llx virt %p\n",
           (unsigned long long)(cr3 & PTE_ADDR_MASK), kernel_pml4);
    printf("VMM: HHDM offset 0x%llx\n", (unsigned long long)vmm_higher_half_offset);
    printf("VMM: Virtual memory management initialized\n");
}

// --------------- Map / Unmap in a specific address space -------------------

int vmm_map_page_in(uint64_t *pml4, uint64_t virtual_addr,
                    uint64_t physical_addr, uint64_t flags) {
    if (!pml4) return -1;

    // Propagate user flag to intermediate table entries
    uint64_t pf = (flags & VMM_USER) ? VMM_USER : 0;

    uint64_t *pdpt = vmm_get_next_level(pml4, PML4_INDEX(virtual_addr), true, pf);
    if (!pdpt) return -1;

    uint64_t *pd = vmm_get_next_level(pdpt, PDP_INDEX(virtual_addr), true, pf);
    if (!pd) return -1;

    uint64_t *pt = vmm_get_next_level(pd, PD_INDEX(virtual_addr), true, pf);
    if (!pt) return -1;

    pt[PT_INDEX(virtual_addr)] = (physical_addr & PTE_ADDR_MASK) | flags | VMM_PRESENT;

    // Flush TLB entry
    __asm__ volatile("invlpg (%0)" : : "r"(virtual_addr) : "memory");
    return 0;
}

int vmm_unmap_page_in(uint64_t *pml4, uint64_t virtual_addr) {
    if (!pml4) return -1;

    uint64_t *pdpt = vmm_get_next_level(pml4, PML4_INDEX(virtual_addr), false, 0);
    if (!pdpt) return -1;
    uint64_t *pd = vmm_get_next_level(pdpt, PDP_INDEX(virtual_addr), false, 0);
    if (!pd) return -1;
    uint64_t *pt = vmm_get_next_level(pd, PD_INDEX(virtual_addr), false, 0);
    if (!pt) return -1;

    pt[PT_INDEX(virtual_addr)] = 0;
    __asm__ volatile("invlpg (%0)" : : "r"(virtual_addr) : "memory");
    return 0;
}

/**
 * Unmap a user page AND release its physical frame (refcount-aware).
 * Use this for anonymous user memory (mmap/brk/stack) — plain
 * vmm_unmap_page_in() only clears the PTE and leaks the frame.
 * Mirrors the leaf behaviour of vmm_free_table().
 */
int vmm_unmap_page_release_in(uint64_t *pml4, uint64_t virtual_addr) {
    if (!pml4) return -1;

    uint64_t *pte = vmm_get_pte_in(pml4, virtual_addr);
    if (!pte || !(*pte & VMM_PRESENT)) return -1;

    uint64_t phys = *pte & PTE_ADDR_MASK;
    bool user_page = (*pte & VMM_USER) != 0;

    *pte = 0;
    __asm__ volatile("invlpg (%0)" : : "r"(virtual_addr) : "memory");

    if (user_page && phys)
        vmm_page_unref(phys);
    return 0;
}

uint64_t vmm_is_mapped_in(uint64_t *pml4, uint64_t virtual_addr) {
    if (!pml4) return 0;

    uint64_t *pdpt = vmm_get_next_level(pml4, PML4_INDEX(virtual_addr), false, 0);
    if (!pdpt) return 0;
    uint64_t *pd = vmm_get_next_level(pdpt, PDP_INDEX(virtual_addr), false, 0);
    if (!pd) return 0;
    uint64_t *pt = vmm_get_next_level(pd, PD_INDEX(virtual_addr), false, 0);
    if (!pt) return 0;

    uint64_t entry = pt[PT_INDEX(virtual_addr)];
    return (entry & VMM_PRESENT) ? (entry & PTE_ADDR_MASK) : 0;
}

uint64_t* vmm_get_pte_in(uint64_t *pml4, uint64_t virtual_addr) {
    if (!pml4) return NULL;

    uint64_t *pdpt = vmm_get_next_level(pml4, PML4_INDEX(virtual_addr), false, 0);
    if (!pdpt) return NULL;
    uint64_t *pd = vmm_get_next_level(pdpt, PDP_INDEX(virtual_addr), false, 0);
    if (!pd) return NULL;
    uint64_t *pt = vmm_get_next_level(pd, PD_INDEX(virtual_addr), false, 0);
    if (!pt) return NULL;

    return &pt[PT_INDEX(virtual_addr)];
}

// --------------- Kernel address-space wrappers ----------------------------

int vmm_map_page(uint64_t virtual_addr, uint64_t physical_addr, uint64_t flags) {
    return vmm_map_page_in(kernel_pml4, virtual_addr, physical_addr, flags);
}

int vmm_unmap_page(uint64_t virtual_addr) {
    return vmm_unmap_page_in(kernel_pml4, virtual_addr);
}

uint64_t vmm_is_mapped(uint64_t virtual_addr) {
    return vmm_is_mapped_in(kernel_pml4, virtual_addr);
}

uint64_t* vmm_get_pte(uint64_t virtual_addr) {
    return vmm_get_pte_in(kernel_pml4, virtual_addr);
}

// --------------- Map a contiguous range -----------------------------------

int vmm_map_range(uint64_t *pml4, uint64_t virt_start, uint64_t phys_start,
                  uint64_t num_pages, uint64_t flags) {
    for (uint64_t i = 0; i < num_pages; i++) {
        int r = vmm_map_page_in(pml4,
                                virt_start + i * PAGE_SIZE,
                                phys_start + i * PAGE_SIZE,
                                flags);
        if (r != 0) return r;
    }
    return 0;
}

// --------------- Address space lifecycle ----------------------------------

uint64_t* vmm_create_address_space(void) {
    uint64_t new_phys = vmm_alloc_frame();
    if (!new_phys) return NULL;

    uint64_t *new_pml4 = (uint64_t*)phys_to_virt(new_phys);

    // Clear user-space entries (PML4 indices 0-255)
    for (int i = 0; i < 256; i++) {
        new_pml4[i] = 0;
    }
    // Share kernel-space entries (PML4 indices 256-511)
    for (int i = 256; i < 512; i++) {
        new_pml4[i] = kernel_pml4[i];
    }
    return new_pml4;
}

/**
 * Recursively deep-copy a page table level.
 * At level 1 (PT), copies the entries (sharing the physical pages).
 * At higher levels, recursively clones child tables.
 * At PML4 level (4), only clones user-space (indices 0-255).
 */
/**
 * Recursively deep-copy a page table level with COW support.
 *
 * At level 1 (PT / leaf entries):
 *   - For writable user pages, mark BOTH source and destination
 *     as read-only + VMM_COW and increment the physical page's
 *     reference count.  The page fault handler will copy-on-write
 *     when either process writes.
 *   - Non-writable or non-user pages are simply duplicated.
 *
 * At higher levels: recursively clones child tables.
 * At PML4 level (4): only clones user-space (indices 0-255).
 */
static uint64_t* vmm_clone_table(uint64_t *src, int level) {
    uint64_t new_phys = vmm_alloc_frame();
    if (!new_phys) return NULL;

    uint64_t *dst = (uint64_t*)phys_to_virt(new_phys);
    int limit = (level == 4) ? 256 : 512;

    for (int i = 0; i < limit; i++) {
        if (!(src[i] & VMM_PRESENT)) {
            dst[i] = 0;
            continue;
        }
        if (level == 1) {
            /* Leaf level -- COW for writable user pages */
            uint64_t pte = src[i];
            uint64_t phys = pte & PTE_ADDR_MASK;

            if ((pte & VMM_USER) && (pte & VMM_WRITABLE)) {
                /* Mark both entries read-only + COW */
                pte = (pte & ~VMM_WRITABLE) | VMM_COW;
                src[i] = pte;      /* parent loses write permission too */
                dst[i] = pte;
                /* Pages are not refcounted until the first COW share.
                 * After the split two PTEs own the frame, so the count
                 * must be 2 — a single bump left it at 1 and the child's
                 * first write took the "sole owner" path, mutating the
                 * parent's stack in place (RIP 0 after vfork resume). */
                if (vmm_page_get_ref(phys) == 0) {
                    vmm_page_ref(phys); /* account for src mapping */
                    vmm_page_ref(phys); /* account for dst mapping */
                } else {
                    vmm_page_ref(phys); /* additional dst mapping */
                }
            } else {
                /* Shared leaf (RX text, already-COW, etc.): both PTEs own the
                 * frame. Match writable-COW accounting so one destroy cannot
                 * free the other's pages. */
                dst[i] = pte;
                if (vmm_page_get_ref(phys) == 0) {
                    vmm_page_ref(phys); /* src */
                    vmm_page_ref(phys); /* dst */
                } else {
                    vmm_page_ref(phys); /* additional dst */
                }
            }
        } else {
            // Intermediate level: recursively clone the child table
            uint64_t *child = vmm_clone_table(
                (uint64_t*)phys_to_virt(src[i] & PTE_ADDR_MASK),
                level - 1);
            if (!child) {
                dst[i] = 0;
                continue;
            }
            dst[i] = virt_to_phys(child) | (src[i] & PTE_FLAGS_MASK);
        }
    }

    // At PML4 level, share kernel mappings
    if (level == 4) {
        for (int i = 256; i < 512; i++) {
            dst[i] = kernel_pml4[i];
        }
    }
    return dst;
}

uint64_t* vmm_clone_address_space(uint64_t *src_pml4) {
    if (!src_pml4) return NULL;
    uint64_t *new_pml4 = vmm_clone_table(src_pml4, 4);

    /* Flush TLB on every CPU and WAIT for ACKs: write-protecting parent PTEs
     * in place is useless if any CPU keeps a stale writable TLB entry — that
     * CPU would dirty the shared frame and the sibling would see the writes
     * (pipe_sigpipe close(read) race, yeshead RIP=0x20). */
    if (new_pml4) {
        extern void smp_tlb_shootdown(void);
        smp_tlb_shootdown();
    }
    return new_pml4;
}

/* Reload CR3 (a full TLB flush) with the given top-level table.
 *
 * NOTE: this briefly skipped the write when `pml4` was already active, to avoid
 * a pointless flush on every user-memory copy.  That is NOT safe here: a
 * process's PML4 is freed at exit and the allocator can hand the same address to
 * the next process, so a stale "already active" made a needed switch a no-op -
 * teardown then walked garbage page tables (kernel page fault in vmm_free_table
 * on the second program launch).  Keep the write unconditional. */
void vmm_switch_address_space(uint64_t *pml4) {
    if (!pml4) return;
    uint64_t phys = virt_to_phys(pml4);
    __asm__ volatile("mov %0, %%cr3" : : "r"(phys) : "memory");
}

/**
 * Recursively free user-space page table structures.
 * Also releases mapped user physical pages at leaf entries.
 */
static void vmm_leaf_release(uint64_t phys_addr);

static void vmm_free_table(uint64_t *table, int level) {
    int limit = (level == 4) ? 256 : 512;

    for (int i = 0; i < limit; i++) {
        if (!(table[i] & VMM_PRESENT)) continue;
        if (level > 1) {
            vmm_free_table(
                (uint64_t*)phys_to_virt(table[i] & PTE_ADDR_MASK),
                level - 1);
        } else {
            vmm_leaf_release(table[i] & PTE_ADDR_MASK);
        }
    }
    kfree(table);
}

void vmm_destroy_address_space(uint64_t *pml4) {
    if (!pml4 || pml4 == kernel_pml4) return;
    vmm_free_table(pml4, 4);
}

// --------------- Page Reference Counting (COW) ----------------------------

uint8_t  *vmm_page_refcounts  = NULL;
uint64_t  vmm_max_phys_pages  = 0;

void vmm_refcount_init(void) {
    /* Use the already-known total_memory_bytes from the PMM */
    vmm_max_phys_pages = total_memory_bytes / PAGE_SIZE;
    size_t alloc_bytes = vmm_max_phys_pages * sizeof(uint8_t);

    vmm_page_refcounts = (uint8_t *)kalloc(alloc_bytes);
    if (vmm_page_refcounts) {
        memset(vmm_page_refcounts, 0, alloc_bytes);
        serial_printf("[vmm] refcount array: %llu pages, %llu KB\n",
                      (unsigned long long)vmm_max_phys_pages,
                      (unsigned long long)(alloc_bytes / 1024));
    } else {
        serial_puts("[vmm] WARNING: failed to allocate page refcount array\n");
    }
}

/* Refcount ops are atomic: COW faults and address-space teardown can run
 * concurrently on different CPUs. */
void vmm_page_ref(uint64_t phys_addr) {
    if (!vmm_page_refcounts) return;
    uint64_t pfn = phys_addr / PAGE_SIZE;
    if (pfn >= vmm_max_phys_pages) return;
    uint8_t old;
    do {
        old = vmm_page_refcounts[pfn];
        if (old >= 255) return; /* saturated: page becomes immortal (leak, not UAF) */
    } while (!__sync_bool_compare_and_swap(&vmm_page_refcounts[pfn], old, old + 1));
}

void vmm_page_unref(uint64_t phys_addr) {
    if (!vmm_page_refcounts) return;
    uint64_t pfn = phys_addr / PAGE_SIZE;
    if (pfn >= vmm_max_phys_pages) return;
    uint8_t old;
    do {
        old = vmm_page_refcounts[pfn];
        if (old >= 255) return; /* saturated pages are never freed */
        /* old==0: already freed or never tracked — do NOT free again.
         * (Previously old<=1 freed on 0, which double-freed after COW
         * teardown / wait reap + parent exit → kfree spam.) */
        if (old == 0)
            return;
        if (old == 1) {
            /* 1→255 (poison), not 1→0: a later leaf_release must not
             * treat the frame as an untracked ref==0 and free again. */
            if (__sync_bool_compare_and_swap(&vmm_page_refcounts[pfn], 1, 255)) {
                vmm_free_frame(phys_addr);
                return;
            }
            continue;
        }
    } while (!__sync_bool_compare_and_swap(&vmm_page_refcounts[pfn], old, old - 1));
}

/* Release a leaf frame from an address space being destroyed.
 * Refcounted pages: unref (free on last). Untracked (ref 0): free once
 * and leave ref at 0. */
static void vmm_leaf_release(uint64_t phys_addr) {
    if (!phys_addr) return;
    if (!vmm_page_refcounts) {
        vmm_free_frame(phys_addr);
        return;
    }
    uint64_t pfn = phys_addr / PAGE_SIZE;
    if (pfn >= vmm_max_phys_pages) {
        vmm_free_frame(phys_addr);
        return;
    }
    uint8_t r = vmm_page_refcounts[pfn];
    if (r >= 255)
        return; /* already freed / saturated */
    if (r == 0) {
        /* Untracked sole mapping: claim with poison, free once. */
        if (__sync_bool_compare_and_swap(&vmm_page_refcounts[pfn], 0, 255))
            vmm_free_frame(phys_addr);
        return;
    }
    vmm_page_unref(phys_addr);
}

uint8_t vmm_page_get_ref(uint64_t phys_addr) {
    if (!vmm_page_refcounts) return 0;
    uint64_t pfn = phys_addr / PAGE_SIZE;
    if (pfn >= vmm_max_phys_pages) return 0;
    return vmm_page_refcounts[pfn];
}

// Memory Pool Management for Performance Optimization

#define MAX_MEMORY_POOLS 16

typedef struct {
    size_t block_size;      // Size of each block
    size_t num_blocks;      // Total number of blocks
    size_t free_blocks;     // Number of free blocks
    void **free_list;       // Array of free block pointers
    void *pool_memory;      // Pointer to allocated pool memory
    bool initialized;       // Whether this pool is active
} memory_pool_t;

static memory_pool_t memory_pools[MAX_MEMORY_POOLS];
static int next_pool_id = 0;

/**
 * @brief Create a memory pool for fast allocation
 */
int mempool_create(size_t block_size, size_t num_blocks) {
    if (next_pool_id >= MAX_MEMORY_POOLS) {
        printf("mempool: Maximum number of pools reached\n");
        return -1;
    }

    if (block_size == 0 || num_blocks == 0) {
        printf("mempool: Invalid pool parameters\n");
        return -1;
    }

    memory_pool_t *pool = &memory_pools[next_pool_id];
    size_t total_size = block_size * num_blocks;

    // Allocate memory for the pool
    pool->pool_memory = kalloc(total_size + num_blocks * sizeof(void*));
    if (!pool->pool_memory) {
        printf("mempool: Failed to allocate pool memory\n");
        return -1;
    }

    // Allocate free list array
    pool->free_list = (void**)((char*)pool->pool_memory + total_size);
    pool->block_size = block_size;
    pool->num_blocks = num_blocks;
    pool->free_blocks = num_blocks;
    pool->initialized = true;

    // Initialize free list
    char *block_ptr = (char*)pool->pool_memory;
    for (size_t i = 0; i < num_blocks; i++) {
        pool->free_list[i] = (void*)block_ptr;
        block_ptr += block_size;
    }

    printf("mempool: Created pool %d with %zu blocks of %zu bytes\n",
           next_pool_id, num_blocks, block_size);

    return next_pool_id++;
}

/**
 * @brief Allocate from memory pool (fast path)
 */
void* mempool_alloc(int pool_id) {
    if (pool_id < 0 || pool_id >= MAX_MEMORY_POOLS ||
        !memory_pools[pool_id].initialized) {
        printf("mempool: Invalid pool ID %d\n", pool_id);
        return NULL;
    }

    memory_pool_t *pool = &memory_pools[pool_id];

    if (pool->free_blocks == 0) {
        printf("mempool: Pool %d is full\n", pool_id);
        return NULL;
    }

    // Get a free block from the end of the free list
    void *block = pool->free_list[--pool->free_blocks];
    return block;
}

/**
 * @brief Free memory back to pool (fast path)
 */
void mempool_free(int pool_id, void *ptr) {
    if (pool_id < 0 || pool_id >= MAX_MEMORY_POOLS ||
        !memory_pools[pool_id].initialized) {
        printf("mempool: Invalid pool ID %d\n", pool_id);
        return;
    }

    if (!ptr) {
        printf("mempool: Cannot free NULL pointer\n");
        return;
    }

    memory_pool_t *pool = &memory_pools[pool_id];

    // Validate that pointer belongs to this pool
    char *pool_start = (char*)pool->pool_memory;
    char *pool_end = pool_start + (pool->block_size * pool->num_blocks);
    char *ptr_char = (char*)ptr;

    if (ptr_char < pool_start || ptr_char >= pool_end) {
        printf("mempool: Pointer %p does not belong to pool %d\n", ptr, pool_id);
        return;
    }

    // Check alignment
    if ((ptr_char - pool_start) % pool->block_size != 0) {
        printf("mempool: Pointer %p is not properly aligned for pool %d\n", ptr, pool_id);
        return;
    }

    if (pool->free_blocks >= pool->num_blocks) {
        printf("mempool: Pool %d free list is full\n", pool_id);
        return;
    }

    // Add back to free list
    pool->free_list[pool->free_blocks++] = ptr;
}

/**
 * @brief Destroy memory pool
 */
void mempool_destroy(int pool_id) {
    if (pool_id < 0 || pool_id >= MAX_MEMORY_POOLS ||
        !memory_pools[pool_id].initialized) {
        printf("mempool: Invalid pool ID %d\n", pool_id);
        return;
    }

    memory_pool_t *pool = &memory_pools[pool_id];

    if (pool->pool_memory) {
        kfree(pool->pool_memory);
    }

    memset(pool, 0, sizeof(memory_pool_t));
    printf("mempool: Destroyed pool %d\n", pool_id);
}