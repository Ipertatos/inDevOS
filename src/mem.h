/**
 * @file mem.h
 * @brief Physical Memory Management Header
 *
 * This header defines the interface for the physical memory manager (PMM)
 * which handles allocation and deallocation of physical memory pages.
 * The PMM uses a bitmap to track free/used pages and provides both
 * regular and high-memory allocation functions.
 *
 * @author inDevOS development team
 * @version 1.0
 * @date 2024
 */

#pragma once

#include "limine.h"
#include "types.h"

/** @brief Standard page size (4KB) */
#define PAGE_SIZE 4096

/** @brief Page table entry flags */
#define VMM_PRESENT     (1ULL << 0)    /**< Page present in memory */
#define VMM_WRITABLE    (1ULL << 1)    /**< Page is writable */
#define VMM_USER        (1ULL << 2)    /**< Page accessible from user mode */
#define VMM_WRITETHRU   (1ULL << 3)    /**< Write-through caching */
#define VMM_NOCACHE     (1ULL << 4)    /**< Disable caching */
#define VMM_ACCESSED    (1ULL << 5)    /**< Page has been accessed */
#define VMM_DIRTY       (1ULL << 6)    /**< Page has been written to */
#define VMM_HUGE        (1ULL << 7)    /**< Huge page (2MB at PD level, 1GB at PDPT) */
#define VMM_GLOBAL      (1ULL << 8)    /**< Global page (not flushed on CR3 change) */
#define VMM_COW         (1ULL << 9)    /**< Copy-on-write (OS-defined, avail bit 9) */
#define VMM_SHARED      (1ULL << 11)   /**< MAP_SHARED anon: fork shares the frame, never COW (avail bit 11) */
#define VMM_USER_RO     (1ULL << 52)   /**< user asked for read-only (mmap/mprotect): never COW-break (avail bit 52) */
#define VMM_DEVICE      (1ULL << 10)   /**< Device memory (fb mmap): never freed, never COW (avail bit 10) */
/** No-execute bit, or 0 when the CPU lacks NX (bit 63 would be reserved) */
extern uint64_t vmm_nx_bit;
#define VMM_NX          vmm_nx_bit

/** @brief Page table entry address mask (bits 12-51) */
#define PTE_ADDR_MASK   0x000FFFFFFFFFF000ULL
#define PTE_FLAGS_MASK  (~PTE_ADDR_MASK)

/** @brief Virtual memory layout */
#define KERNEL_VIRT_BASE    0xFFFFFFFF80000000ULL  /**< Kernel virtual base */
#define USER_VIRT_BASE      0x0000000000000000ULL  /**< User virtual base */
#define USER_STACK_TOP      0x00007FFFFFFFF000ULL  /**< User stack top */
#define USER_HEAP_START     0x0000000010000000ULL  /**< User heap start (256MB) */
#define USER_CODE_START     0x0000000000400000ULL  /**< User code start (4MB) */

/** @brief Page table levels */
#define PML4_SHIFT      39
#define PDP_SHIFT       30
#define PD_SHIFT        21
#define PT_SHIFT        12

#define PML4_INDEX(v)   (((v) >> PML4_SHIFT) & 0x1FF)
#define PDP_INDEX(v)    (((v) >> PDP_SHIFT) & 0x1FF)
#define PD_INDEX(v)     (((v) >> PD_SHIFT) & 0x1FF)
#define PT_INDEX(v)     (((v) >> PT_SHIFT) & 0x1FF)

/** @brief Limine memory map response from bootloader */
extern volatile struct limine_memmap_response *memmap;

/** @brief Total physical memory available in bytes */
extern uint64_t total_memory_bytes;

/** @brief Maximum number of physical pages available */
extern uint64_t max_pages_available;

/** @brief Current number of available physical pages */
extern uint64_t num_pages_available;

/** @brief Offset for higher-half virtual memory mapping */
extern uint64_t vmm_higher_half_offset;

/**
 * @brief Initialize the physical memory manager
 *
 * Sets up the page bitmap, calculates available memory,
 * and prepares the allocation system.
 */
void kmem_init();

/**
 * @brief Allocate physical memory
 *
 * Allocates contiguous physical memory pages and returns
 * a virtual address in the higher-half mapping.
 *
 * @param numBytes Number of bytes to allocate
 * @return Virtual address of allocated memory, or NULL on failure
 */
void* kalloc(size_t numBytes);

/**
 * @brief Allocate high memory (above 1MB)
 *
 * Allocates memory guaranteed to be above the 1MB physical address
 * boundary, useful for DMA buffers and network packets.
 *
 * @param numBytes Number of bytes to allocate
 * @return Virtual address of allocated memory, or NULL on failure
 */
void* kalloc_high(size_t numBytes);

/**
 * @brief Free allocated memory
 *
 * Releases previously allocated memory back to the free pool.
 * The pointer must have been returned by kalloc() or kalloc_high().
 *
 * @param ptr Pointer to memory to free
 */
void kfree(void *ptr);

/**
 * @brief Returns the usable size in bytes of a live kalloc() allocation.
 *
 * @param ptr Pointer previously returned by kalloc()
 * @return Allocation size in bytes, or 0 if not a live allocation
 */
uint64_t kalloc_usable_size(void *ptr);

/**
 * @brief Memory debugging functions
 * @{
 */

/**
 * @brief Dump memory in decimal format
 * @param location Starting address
 * @param len_bytes Number of bytes to dump
 */
void memdumps(void *location, uint64_t len_bytes);

/**
 * @brief Dump memory in 32-bit hexadecimal format
 * @param location Starting address
 * @param len_bytes Number of bytes to dump
 */
void memdumpx32(void *location, uint64_t len_bytes);

/**
 * @brief Dump memory in 64-bit hexadecimal format
 * @param location Starting address
 * @param len_bytes Number of bytes to dump
 */
void memdumpx64(void *location, uint64_t len_bytes);

/** @} */

/**
 * @brief Standard C library memory functions
 * @{
 */

/**
 * @brief Set memory to a specific value
 * @param s Pointer to memory
 * @param c Value to set
 * @param n Number of bytes to set
 * @return Pointer to memory
 */
void *memset(void *s, int c, size_t n);

/**
 * @brief Copy memory area
 * @param dest Destination address
 * @param src Source address
 * @param n Number of bytes to copy
 * @return Pointer to destination
 */
void *memcpy(void *dest, const void *src, size_t n);

/**
 * @brief Copy memory with possible overlap
 * @param dest Destination address
 * @param src Source address
 * @param n Number of bytes to copy
 * @return Pointer to destination
 */
void *memmove(void *dest, const void *src, size_t n);

/**
 * @brief Compare memory regions
 * @param s1 First memory region
 * @param s2 Second memory region
 * @param n Number of bytes to compare
 * @return 0 if equal, negative if s1 < s2, positive if s1 > s2
 */
int memcmp(const void *s1, const void *s2, size_t n);

/** @} */

/**
 * @brief Internal utility functions
 * @{
 */

/**
 * @brief Convert page number to virtual address
 * @param page Page number
 * @return Virtual address
 */
void* _get_addr_from_page(uint64_t page);

/**
 * @brief Convert virtual address to page number
 * @param addr Virtual address
 * @return Page number
 */
uint64_t _get_page_from_addr(void* addr);

/** @} */

/**
 * @brief Address conversion helpers
 * @{
 */

/** @brief Convert physical address to virtual using HHDM */
static inline void* phys_to_virt(uint64_t phys_addr) {
    extern uint64_t vmm_higher_half_offset;
    return (void*)(phys_addr + vmm_higher_half_offset);
}

/** @brief Convert HHDM virtual address to physical */
static inline uint64_t virt_to_phys(void *virt_addr) {
    extern uint64_t vmm_higher_half_offset;
    return (uint64_t)virt_addr - vmm_higher_half_offset;
}

/** @} */

/**
 * @brief Virtual Memory Management Functions
 * @{
 */

/** @brief Initialize virtual memory management (reads bootloader page tables) */
void vmm_init(void);

/** @brief Get the kernel PML4 (top-level page table) */
uint64_t* vmm_get_kernel_pml4(void);

/**
 * @brief Map a virtual address to a physical address in the kernel address space
 * @return 0 on success, -1 on failure
 */
int vmm_map_page(uint64_t virtual_addr, uint64_t physical_addr, uint64_t flags);

/**
 * @brief Map a virtual address in a specific address space
 * @param pml4 Top-level page table for the target address space
 * @return 0 on success, -1 on failure
 */
int vmm_map_page_in(uint64_t *pml4, uint64_t virtual_addr, uint64_t physical_addr, uint64_t flags);

/** @brief Unmap a virtual address from the kernel address space */
int vmm_unmap_page(uint64_t virtual_addr);

/** @brief Unmap a virtual address from a specific address space */
int vmm_unmap_page_in(uint64_t *pml4, uint64_t virtual_addr);

/** @brief Unmap a user page and release (unref/free) its physical frame.
 *  Use for anonymous user memory; vmm_unmap_page_in() alone leaks the frame. */
int vmm_unmap_page_release_in(uint64_t *pml4, uint64_t virtual_addr);

/** @brief Unmap @pages user pages from @virtual_addr, shoot down the TLB on all
 *  CPUs, then release (unref/free) the frames.  Returns PTEs cleared. */
int vmm_unmap_range_release_in(uint64_t *pml4, uint64_t virtual_addr, uint64_t pages);
/* COW/RO-break: release the old frame after the PTE now maps a copy. */
void vmm_cow_release_old(uint64_t phys_addr);

/** @brief Check if virtual address is mapped; returns physical address or 0 */
uint64_t vmm_is_mapped(uint64_t virtual_addr);

/** @brief Check if virtual address is mapped in a specific address space */
uint64_t vmm_is_mapped_in(uint64_t *pml4, uint64_t virtual_addr);

/** @brief Get page table entry pointer for a virtual address */
uint64_t* vmm_get_pte(uint64_t virtual_addr);

/** @brief Get page table entry pointer in a specific address space */
uint64_t* vmm_get_pte_in(uint64_t *pml4, uint64_t virtual_addr);

/**
 * @brief Create a new address space with kernel mappings shared
 * @return New PML4 pointer (virtual), or NULL on failure
 */
uint64_t* vmm_create_address_space(void);

/**
 * @brief Deep-copy user-space mappings from a source address space
 * Kernel mappings are shared (not copied).
 * @param src_pml4 Source PML4 to clone from
 * @return New PML4 pointer (virtual), or NULL on failure
 */
uint64_t* vmm_clone_address_space(uint64_t *src_pml4);

/**
 * @brief Switch to a different address space by loading CR3
 * @param pml4 PML4 to switch to (virtual address)
 */
void vmm_switch_address_space(uint64_t *pml4);

/**
 * @brief Free all user-space page tables in an address space
 * Does NOT free the physical pages themselves (they may be shared).
 * @param pml4 PML4 to destroy (must not be the kernel PML4)
 */
void vmm_destroy_address_space(uint64_t *pml4);

/**
 * @brief Map a contiguous range of pages
 * @return 0 on success, -1 on failure
 */
int vmm_map_range(uint64_t *pml4, uint64_t virt_start, uint64_t phys_start,
                  uint64_t num_pages, uint64_t flags);

/**
 * @brief Allocate a zeroed physical frame for page tables
 * @return Physical address of the frame, or 0 on failure
 */
uint64_t vmm_alloc_frame(void);

/** @brief Free a physical frame */
void vmm_free_frame(uint64_t phys_addr);

/** @} */

/**
 * @brief Page Reference Counting (for Copy-on-Write)
 * @{
 */

/** @brief Per-page reference count array (indexed by physical page number) */
extern uint8_t  *vmm_page_refcounts;

/** @brief Maximum physical page number tracked by refcount array */
extern uint64_t  vmm_max_phys_pages;

/** @brief Initialize the page refcount system (call once after kmem_init) */
void vmm_refcount_init(void);

/** @brief Increment reference count for a physical page */
void vmm_page_ref(uint64_t phys_addr);

/** @brief Decrement reference count; frees the frame when it reaches 0 */
void vmm_page_unref(uint64_t phys_addr);

/** @brief Get current reference count for a physical page */
uint8_t vmm_page_get_ref(uint64_t phys_addr);

/** @} */

/**
 * @brief Memory Pool Management (for performance optimization)
 * @{
 */

/**
 * @brief Initialize memory pool allocator
 * @param pool_size Size of each memory block in the pool
 * @param num_blocks Number of blocks in the pool
 * @return Pool ID on success, -1 on failure
 */
int mempool_create(size_t pool_size, size_t num_blocks);

/**
 * @brief Allocate from memory pool
 * @param pool_id Pool identifier
 * @return Pointer to allocated memory, NULL on failure
 */
void* mempool_alloc(int pool_id);

/**
 * @brief Free memory back to pool
 * @param pool_id Pool identifier
 * @param ptr Pointer to free
 */
void mempool_free(int pool_id, void *ptr);

/**
 * @brief Destroy memory pool
 * @param pool_id Pool identifier
 */
void mempool_destroy(int pool_id);

/** @} */