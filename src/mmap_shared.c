/*
 * MAP_SHARED file mappings (Linux ABI).
 *
 * A shared file mapping is a VMM_SHARED anonymous region (fork children
 * share the frames) filled from the file, plus a record here. The record
 * writes the pages back to the file:
 *   - msync() on the range,
 *   - munmap() / MAP_FIXED over the range (then the record is trimmed),
 *   - destruction of the address space (exit, exec),
 *   - before read() of the same file (pages the owner dirtied),
 * and write() to the same file copies the new bytes into the mapping, so
 * simple read()/write() <-> mapping sequences are coherent. Write-back never
 * extends the file (bytes past EOF stay in memory only, like Linux).
 * Pages are reached through the owner's page tables (phys alias), so the
 * flush works from any context while the address space exists.
 */
#include "mem.h"
#include "string.h"
#include "vfs.h"
#include "thread.h"
#include "atomic.h"

#define SHMF_PATHMAX 256

typedef struct shmf_rec {
    struct shmf_rec *next;
    uint64_t *pml4;
    uint64_t va, len;          /* page aligned */
    uint64_t foff;
    bool writable;             /* PROT_WRITE at mmap: else never written back */
    char path[SHMF_PATHMAX];
} shmf_rec_t;

static shmf_rec_t *shmf_list;
volatile int shmf_count;
static volatile int shmf_busy;

static void shmf_lock(void) {
    while (__atomic_exchange_n(&shmf_busy, 1, __ATOMIC_ACQUIRE)) {
        if (global_scheduler.scheduler_running) thread_sleep(1);
        else __asm__ volatile("pause");
    }
}
static void shmf_unlock(void) { __atomic_store_n(&shmf_busy, 0, __ATOMIC_RELEASE); }

static uint8_t *shmf_page(uint64_t *pml4, uint64_t va, bool need_dirty) {
    uint64_t *pte = vmm_get_pte_in(pml4, va & ~0xFFFull);
    if (!pte || !(*pte & VMM_PRESENT)) return NULL;
    if (need_dirty && !(*pte & VMM_DIRTY)) return NULL;
    return (uint8_t *)phys_to_virt(*pte & PTE_ADDR_MASK);
}

/* write record pages [a, b) (record VAs) back; caller holds the lock */
static void shmf_flush(shmf_rec_t *r, uint64_t a, uint64_t b, bool dirty_only) {
    if (a < r->va) a = r->va;
    if (b > r->va + r->len) b = r->va + r->len;
    if (a >= b || !r->writable) return;
    vfs_stat_t st;
    if (vfs_stat(r->path, &st) != 0) return;
    uint64_t fend = st.size;
    int fd = -1;
    for (uint64_t p = a & ~0xFFFull; p < b; p += 0x1000) {
        uint64_t s = p < a ? a : p, e = p + 0x1000 > b ? b : p + 0x1000;
        uint64_t fo = r->foff + (s - r->va);
        if (fo >= fend) break;
        if (fo + (e - s) > fend) e = s + (fend - fo);
        uint8_t *pg = shmf_page(r->pml4, p, dirty_only);
        if (!pg) continue;
        if (fd < 0) {
            fd = vfs_open(r->path, VFS_O_READ | VFS_O_WRITE);
            if (fd < 0) return;
        }
        if (vfs_seek(fd, (int32_t)fo, VFS_SEEK_SET) < 0) break;
        vfs_write(fd, pg + (s & 0xFFF), (uint32_t)(e - s));
    }
    if (fd >= 0) vfs_close(fd);
}

/* mmap(MAP_SHARED, file): fill [va, va+len) from the file through the
 * phys alias (so the user PTEs stay clean) and register the record. */
long shmf_map(uint64_t *pml4, uint64_t va, uint64_t len, int vfs_fd, uint64_t foff, bool writable) {
    shmf_rec_t *r = (shmf_rec_t *)kalloc(sizeof(shmf_rec_t));
    if (!r) return -12;
    memset(r, 0, sizeof(*r));
    if (vfs_fd_get_path(vfs_fd, r->path, sizeof(r->path)) != 0) { kfree(r); return -9; }
    r->pml4 = pml4;
    r->va = va;
    r->len = (len + 0xFFF) & ~0xFFFull;
    r->foff = foff;
    r->writable = writable;
    int fd = vfs_open(r->path, VFS_O_READ);
    if (fd >= 0) {
        if (vfs_seek(fd, (int32_t)foff, VFS_SEEK_SET) >= 0) {
            for (uint64_t p = 0; p < r->len; p += 0x1000) {
                uint8_t *pg = shmf_page(pml4, va + p, false);
                if (!pg) continue;
                int n = vfs_read(fd, pg, 0x1000);
                if (n <= 0) break;
            }
        }
        vfs_close(fd);
    }
    shmf_lock();
    r->next = shmf_list;
    shmf_list = r;
    shmf_count++;
    shmf_unlock();
    return 0;
}

/* msync (unmap=false) or munmap/MAP_FIXED (unmap=true) over [va, va+len) */
void shmf_range(uint64_t *pml4, uint64_t va, uint64_t len, bool unmap) {
    if (!shmf_count || !pml4) return;
    uint64_t a = va & ~0xFFFull, b = (va + len + 0xFFF) & ~0xFFFull;
    shmf_lock();
    shmf_rec_t **pp = &shmf_list;
    while (*pp) {
        shmf_rec_t *r = *pp;
        if (r->pml4 != pml4 || b <= r->va || a >= r->va + r->len) { pp = &r->next; continue; }
        shmf_flush(r, a, b, false);
        if (!unmap) { pp = &r->next; continue; }
        uint64_t rs = r->va, re = r->va + r->len;
        if (a <= rs && b >= re) {                  /* fully covered: drop */
            *pp = r->next;
            kfree(r);
            shmf_count--;
            continue;
        }
        if (a > rs && b < re) {                    /* hole in the middle: split */
            shmf_rec_t *t = (shmf_rec_t *)kalloc(sizeof(shmf_rec_t));
            if (t) {
                memcpy(t, r, sizeof(*t));
                t->va = b;
                t->len = re - b;
                t->foff = r->foff + (b - rs);
                t->next = r->next;
                r->next = t;
                shmf_count++;
            }
            r->len = a - rs;
        } else if (a <= rs) {                      /* head removed */
            r->foff += b - rs;
            r->len = re - b;
            r->va = b;
        } else {                                   /* tail removed */
            r->len = a - rs;
        }
        pp = &r->next;
    }
    shmf_unlock();
}

/* address space going away (exit / exec): final write-back */
void shmf_as_destroy(uint64_t *pml4) {
    if (!shmf_count || !pml4) return;
    shmf_lock();
    shmf_rec_t **pp = &shmf_list;
    while (*pp) {
        shmf_rec_t *r = *pp;
        if (r->pml4 != pml4) { pp = &r->next; continue; }
        shmf_flush(r, r->va, r->va + r->len, false);
        *pp = r->next;
        kfree(r);
        shmf_count--;
    }
    shmf_unlock();
}

/* read() of a file: make dirty mapped pages visible first */
void shmf_before_read(int vfs_fd) {
    if (!shmf_count) return;
    char path[SHMF_PATHMAX];
    if (vfs_fd_get_path(vfs_fd, path, sizeof(path)) != 0) return;
    shmf_lock();
    for (shmf_rec_t *r = shmf_list; r; r = r->next)
        if (!strcmp(r->path, path)) shmf_flush(r, r->va, r->va + r->len, true);
    shmf_unlock();
}

/* write() of [pos, pos+n) to a file: mirror into its shared mappings */
void shmf_after_write(int vfs_fd, uint64_t pos, const void *kbuf, uint32_t n) {
    if (!shmf_count || !n) return;
    char path[SHMF_PATHMAX];
    if (vfs_fd_get_path(vfs_fd, path, sizeof(path)) != 0) return;
    shmf_lock();
    for (shmf_rec_t *r = shmf_list; r; r = r->next) {
        if (strcmp(r->path, path)) continue;
        uint64_t fs = r->foff, fe = r->foff + r->len;
        uint64_t s = pos > fs ? pos : fs, e = pos + n < fe ? pos + n : fe;
        while (s < e) {
            uint64_t va = r->va + (s - fs);
            uint64_t chunk = 0x1000 - (va & 0xFFF);
            if (chunk > e - s) chunk = e - s;
            uint8_t *pg = shmf_page(r->pml4, va, false);
            if (pg) memcpy(pg + (va & 0xFFF), (const uint8_t *)kbuf + (s - pos), (size_t)chunk);
            s += chunk;
        }
    }
    shmf_unlock();
}
