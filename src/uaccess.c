/**
 * @file uaccess.c
 * @brief User-space memory access validation
 *
 * Implements safe copy routines between kernel and user memory.
 * These functions validate that all user-space pointers are within
 * the canonical user address range and backed by user-accessible PTEs.
 * Not-yet-faulted heap/stack pages are demand-mapped and COW pages are
 * broken before the kernel writes to them, and the actual accesses run
 * inside a stac/clac window when SMAP is on.
 */

#include "uaccess.h"
#include "mem.h"
#include "syscall.h"
#include "serial.h"
#include "string.h"

static process_t *uaccess_proc(void) {
    process_t *proc = get_current_process();
    if (proc && proc->pml4 && proc->is_usermode)
        return proc;
    return NULL;
}

static uint64_t *uaccess_resolve_pml4(process_t *proc) {
    return proc ? proc->pml4 : vmm_get_kernel_pml4();
}

static bool user_page_ok(process_t *proc, uint64_t va, bool write) {
    uint64_t *pml4 = uaccess_resolve_pml4(proc);
    if (!pml4)
        return false;
    va &= ~(uint64_t)(PAGE_SIZE - 1);

    for (int attempt = 0; attempt < 2; attempt++) {
        uint64_t entry = 0;
        if (vmm_is_mapped_in(pml4, va)) {
            uint64_t *pte = vmm_get_pte_in(pml4, va);
            if (pte)
                entry = *pte;
        }
        bool present = (entry & VMM_PRESENT) && (entry & VMM_USER);
        if (present && (!write || (entry & VMM_WRITABLE)))
            return true;
        if (attempt > 0 || !user_fault_fixup(proc, va, present, write))
            return false;
    }
    return false;
}

static bool user_range_ok(uint64_t addr, size_t len, bool write) {
    if (!is_user_range(addr, len))
        return false;

    process_t *proc = uaccess_proc();
    uint64_t end = addr + len;
    uint64_t page = addr & ~(uint64_t)(PAGE_SIZE - 1);

    while (page < end) {
        if (!user_page_ok(proc, page, write))
            return false;
        page += PAGE_SIZE;
    }
    return true;
}

bool access_ok_user_range(uint64_t addr, size_t len) {
    return user_range_ok(addr, len, false);
}

int copy_from_user(void *kern_dst, const void *user_src, size_t len) {
    if (len == 0) return 0;
    if (!kern_dst || !user_src) return -1;

    uint64_t src = (uint64_t)user_src;
    if (!user_range_ok(src, len, false)) {
        serial_printf("[uaccess] copy_from_user: bad range 0x%llx len=%llu\n",
                      (unsigned long long)src, (unsigned long long)len);
        return -1;
    }

    user_access_begin();
    memcpy(kern_dst, user_src, len);
    user_access_end();
    return 0;
}

int copy_to_user(void *user_dst, const void *kern_src, size_t len) {
    if (len == 0) return 0;
    if (!user_dst || !kern_src) return -1;

    uint64_t dst = (uint64_t)user_dst;
    if (!user_range_ok(dst, len, true)) {
        serial_printf("[uaccess] copy_to_user: bad range 0x%llx len=%llu\n",
                      (unsigned long long)dst, (unsigned long long)len);
        return -1;
    }

    user_access_begin();
    memcpy(user_dst, kern_src, len);
    user_access_end();
    return 0;
}

char *strncpy_from_user(char *kern_dst, const char *user_src, size_t max) {
    if (max == 0) return kern_dst;
    if (!kern_dst || !user_src) return NULL;

    uint64_t src = (uint64_t)user_src;
    if (!is_user_address(src)) {
        serial_printf("[uaccess] strncpy_from_user: bad addr 0x%llx\n",
                      (unsigned long long)src);
        return NULL;
    }

    process_t *proc = uaccess_proc();
    uint64_t checked_page = ~(uint64_t)0;
    size_t i;
    for (i = 0; i < max - 1; i++) {
        uint64_t cur = src + i;
        uint64_t page = cur & ~(uint64_t)(PAGE_SIZE - 1);
        if (page != checked_page) {
            if (!is_user_address(cur) || !user_page_ok(proc, cur, false)) {
                kern_dst[i] = '\0';
                return NULL;
            }
            checked_page = page;
        }
        user_access_begin();
        char c = ((const char *)user_src)[i];
        user_access_end();
        kern_dst[i] = c;
        if (c == '\0') return kern_dst;
    }
    kern_dst[i] = '\0';
    return kern_dst;
}
