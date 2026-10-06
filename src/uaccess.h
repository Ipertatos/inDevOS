/**
 * @file uaccess.h
 * @brief User-space memory access validation
 *
 * Provides safe copy routines between kernel and user memory.
 * All syscalls that dereference user pointers MUST use these
 * functions to prevent kernel data leaks and arbitrary writes.
 */

#ifndef UACCESS_H
#define UACCESS_H

#include "types.h"
#include "cpu.h"
#include <stddef.h>

/** @brief Upper bound of valid user-space addresses */
#define USER_ADDR_MAX   0x00007FFFFFFFFFFFULL

struct process;

/**
 * @brief Map a demand-paged heap/stack page or break COW for @p proc.
 * @return true if the page at @p addr is now accessible (idt.c)
 */
bool user_fault_fixup(struct process *proc, uint64_t addr,
                      bool page_present, bool was_write);

/**
 * @brief Open/close a window in which the kernel may touch user pages
 *        (stac/clac under SMAP).  Keep the window as small as possible.
 */
static inline void user_access_begin(void) {
    if (cpu_smap_enabled)
        __asm__ volatile("stac" ::: "memory", "cc");
}

static inline void user_access_end(void) {
    if (cpu_smap_enabled)
        __asm__ volatile("clac" ::: "memory", "cc");
}

/**
 * @brief Check if an address is in user-space range
 * @param addr Address to check
 * @return true if addr is a valid user-space address
 */
static inline bool is_user_address(uint64_t addr) {
    return addr <= USER_ADDR_MAX;
}

/**
 * @brief Check if an entire range [addr, addr+len) is in user-space
 * @return true if the full range is valid user-space
 */
static inline bool is_user_range(uint64_t addr, size_t len) {
    if (len == 0) return true;
    /* Check for overflow and that the whole range stays below the limit */
    return addr <= USER_ADDR_MAX && (addr + len - 1) <= USER_ADDR_MAX;
}

/**
 * @brief Check canonical user range and that every page in the range is
 *        present with VMM_USER in the current process (or kernel) page tables.
 */
bool access_ok_user_range(uint64_t addr, size_t len);

/**
 * @brief Copy data from user-space to kernel-space
 *
 * Validates that the source range lies entirely in user memory,
 * then copies @p len bytes.
 *
 * @param kern_dst Kernel destination buffer
 * @param user_src User-space source pointer
 * @param len      Number of bytes to copy
 * @return 0 on success, -1 if the user pointer is invalid
 */
int copy_from_user(void *kern_dst, const void *user_src, size_t len);

/**
 * @brief Copy data from kernel-space to user-space
 *
 * Validates that the destination range lies entirely in user memory,
 * then copies @p len bytes.
 *
 * @param user_dst User-space destination pointer
 * @param kern_src Kernel source buffer
 * @param len      Number of bytes to copy
 * @return 0 on success, -1 if the user pointer is invalid
 */
int copy_to_user(void *user_dst, const void *kern_src, size_t len);

/**
 * @brief Safely copy a NUL-terminated string from user-space
 *
 * Copies up to @p max-1 bytes from @p user_src into @p kern_dst,
 * always NUL-terminating the result.
 *
 * @param kern_dst Kernel destination buffer (at least @p max bytes)
 * @param user_src User-space source string
 * @param max      Maximum number of bytes (including NUL) to write
 * @return kern_dst on success, NULL if the pointer is invalid
 */
char *strncpy_from_user(char *kern_dst, const char *user_src, size_t max);

#endif /* UACCESS_H */
