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
#include "thread.h"

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

/* Kernel bounce buffers a thread may hand to sys_read/sys_write
 * (uaccess_kbuf_begin).  Owner thread + exact range, so other threads and
 * other kernel addresses are still refused. */
#define UKB_SLOTS 16
static struct { void *volatile thr; uint64_t lo, hi; } ukb[UKB_SLOTS];

int uaccess_kbuf_begin(const void *buf, size_t len) {
    void *self = (void *)scheduler_get_current_thread();
    if (!self || !buf || !len) return -1;
    for (int i = 0; i < UKB_SLOTS; i++) {
        void *expect = NULL;
        if (__atomic_compare_exchange_n(&ukb[i].thr, &expect, (void *)1, false,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            ukb[i].lo = (uint64_t)buf;
            ukb[i].hi = (uint64_t)buf + len;
            __atomic_store_n(&ukb[i].thr, self, __ATOMIC_RELEASE);
            return i;
        }
    }
    return -1;
}

void uaccess_kbuf_end(int slot) {
    if (slot < 0 || slot >= UKB_SLOTS) return;
    ukb[slot].lo = ukb[slot].hi = 0;
    __atomic_store_n(&ukb[slot].thr, NULL, __ATOMIC_RELEASE);
}

static bool kbuf_range_ok(uint64_t addr, size_t len) {
    void *self = (void *)scheduler_get_current_thread();
    if (!self || addr + len < addr) return false;
    for (int i = 0; i < UKB_SLOTS; i++)
        if (__atomic_load_n(&ukb[i].thr, __ATOMIC_ACQUIRE) == self &&
            addr >= ukb[i].lo && addr + len <= ukb[i].hi)
            return true;
    return false;
}

static bool user_range_ok(uint64_t addr, size_t len, bool write) {
    if (len && kbuf_range_ok(addr, len))
        return true;
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

int user_atomic_op_u32(uint64_t uaddr, int op, uint32_t oparg, uint32_t *old) {
    if ((uaddr & 3) || !old) return -1;
    if (op < 0 || op > 4) return -2;
    if (!user_range_ok(uaddr, 4, true))
        return -1;
    volatile uint32_t *w = (volatile uint32_t *)uaddr;
    user_access_begin();
    uint32_t o = __atomic_load_n(w, __ATOMIC_RELAXED), n;
    do {
        switch (op) {
        case 0:  n = oparg; break;
        case 1:  n = o + oparg; break;
        case 2:  n = o | oparg; break;
        case 3:  n = o & ~oparg; break;
        default: n = o ^ oparg; break;
        }
    } while (!__atomic_compare_exchange_n(w, &o, n, false, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED));
    user_access_end();
    *old = o;
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
