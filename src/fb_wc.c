/*
 * fb_wc.c - write-combining for the linear framebuffer.
 *
 * Limine programs the PAT as WB,WT,UC-,UC,WP,WC,... and is supposed to map
 * the framebuffer WC in the HHDM, but older loaders / some firmwares leave it
 * WB (which the firmware MTRRs then demote to UC on the MMIO BAR).  UC
 * framebuffer stores are an order of magnitude slower than WC on real
 * hardware.  This file:
 *   - makes sure PAT entry 5 is WC on every CPU (BSP + APs),
 *   - rewrites the kernel's HHDM leaf entries covering the framebuffer to
 *     select PAT entry 5 (PAT=1, PCD=0, PWT=1),
 *   - provides fb_wc_pte_flags() so user mappings of the framebuffer
 *     (fbdev mmap) can use the same memory type,
 *   - logs MTRR state for the framebuffer range (PAT WC overrides any MTRR
 *     type, so no MTRR change is needed),
 *   - times a full-frame RAM->FB copy before/after (TSC cycles).
 * If the CPU has no PAT nothing is changed.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "limine.h"
#include "mem.h"
#include "msr.h"
#include "serial.h"
#include "fb_wc.h"

extern struct limine_framebuffer *fb;

#define MSR_IA32_PAT        0x277
#define MSR_MTRRCAP         0xFE
#define MSR_MTRR_DEF_TYPE   0x2FF
#define MSR_MTRR_PHYSBASE0  0x200

#define PAT_WB  0x06ULL
#define PAT_WT  0x04ULL
#define PAT_UCM 0x07ULL
#define PAT_UC  0x00ULL
#define PAT_WP  0x05ULL
#define PAT_WC  0x01ULL

/* Limine layout, entries 6/7 made explicit (UC-, UC). */
#define PAT_LAYOUT (PAT_WB | (PAT_WT << 8) | (PAT_UCM << 16) | (PAT_UC << 24) | \
                    (PAT_WP << 32) | (PAT_WC << 40) | (PAT_UCM << 48) | (PAT_UC << 56))

static bool g_pat_ok = false;     /* BSP PAT has WC at index 5 */
static bool g_fb_wc  = false;     /* kernel fb mapping now WC */

static inline void cpuid_raw(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}
static inline uint64_t rdmsr64(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static inline void wrmsr64(uint32_t msr, uint64_t v) {
    __asm__ volatile("wrmsr" :: "a"((uint32_t)v), "d"((uint32_t)(v >> 32)), "c"(msr));
}
static inline uint64_t rdtsc64(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
static inline void flush_tlb_local(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
}

static bool cpu_has_pat(void) {
    uint32_t a, b, c, d;
    cpuid_raw(1, &a, &b, &c, &d);
    return (d >> 16) & 1;
}
static bool cpu_has_mtrr(void) {
    uint32_t a, b, c, d;
    cpuid_raw(1, &a, &b, &c, &d);
    return (d >> 12) & 1;
}

bool fb_wc_pat_ready(void) { return g_pat_ok; }

/* Run on every CPU (BSP from fb_wc_init, APs from ap_entry). */
void fb_wc_pat_init_cpu(void) {
    if (!cpu_has_pat()) return;
    uint64_t pat = rdmsr64(MSR_IA32_PAT);
    if (((pat >> 40) & 7) == PAT_WC && (pat & 7) == PAT_WB) return;
    /* Entry 0 (WB) and the low entries keep their meaning so existing
     * mappings are unaffected; only 4..7 change.  SDM 11.12.4: flush caches
     * and TLBs around a PAT change. */
    uint64_t want = (pat & 0x00000000FFFFFFFFULL) | (PAT_LAYOUT & 0xFFFFFFFF00000000ULL);
    __asm__ volatile("wbinvd" ::: "memory");
    wrmsr64(MSR_IA32_PAT, want);
    __asm__ volatile("wbinvd" ::: "memory");
    flush_tlb_local();
}

/* PTE bits selecting PAT index 5 (WC) for a 4 KiB leaf. */
uint64_t fb_wc_pte_flags(void) {
    if (!g_pat_ok) return VMM_NOCACHE | VMM_WRITETHRU;   /* UC fallback */
    return (1ULL << 7) | VMM_WRITETHRU;                   /* PAT=1,PCD=0,PWT=1 */
}

/* Which PAT index does a leaf select? level: 1=4K, 2=2M, 3=1G */
static int leaf_pat_index(uint64_t e, int level) {
    int pat = (level == 1) ? (int)((e >> 7) & 1) : (int)((e >> 12) & 1);
    return pat * 4 + (int)((e >> 4) & 1) * 2 + (int)((e >> 3) & 1);
}
static uint64_t leaf_set_wc(uint64_t e, int level) {
    e &= ~(VMM_NOCACHE | VMM_WRITETHRU);
    if (level == 1) e |= (1ULL << 7);
    else            e |= (1ULL << 12);
    return e | VMM_WRITETHRU;
}

static const char *pat_name(uint64_t t) {
    switch (t & 7) { case 0: return "UC"; case 1: return "WC"; case 4: return "WT";
                     case 5: return "WP"; case 6: return "WB"; case 7: return "UC-"; }
    return "?";
}

static void log_mtrrs(uint64_t phys, uint64_t len) {
    if (!cpu_has_mtrr()) { serial_printf("[fbwc] no MTRRs\n"); return; }
    uint64_t cap = rdmsr64(MSR_MTRRCAP), def = rdmsr64(MSR_MTRR_DEF_TYPE);
    serial_printf("[fbwc] MTRR cap=%llx def=%llx (default %s, %s)\n",
                  (unsigned long long)cap, (unsigned long long)def, pat_name(def & 0xFF),
                  (def & (1 << 11)) ? "enabled" : "DISABLED");
    int n = (int)(cap & 0xFF);
    for (int i = 0; i < n && i < 16; i++) {
        uint64_t base = rdmsr64(MSR_MTRR_PHYSBASE0 + 2 * i);
        uint64_t mask = rdmsr64(MSR_MTRR_PHYSBASE0 + 2 * i + 1);
        if (!(mask & (1 << 11))) continue;
        uint64_t m = mask & 0x000FFFFFFFFFF000ULL;
        bool hit = ((phys & m) == (base & m & 0x000FFFFFFFFFF000ULL)) ||
                   (((phys + len - 1) & m) == (base & m & 0x000FFFFFFFFFF000ULL));
        serial_printf("[fbwc] MTRR%d base=%llx mask=%llx type=%s%s\n", i,
                      (unsigned long long)(base & ~0xFFFULL), (unsigned long long)m,
                      pat_name(base & 0xFF), hit ? "  <-- covers fb" : "");
    }
}

/* Time copying `rows` rows of a RAM buffer to the framebuffer. */
static uint64_t bench_copy(const uint32_t *src, int rows) {
    uint8_t *dst = (uint8_t *)fb->address;
    size_t bytes = (size_t)fb->width * 4u;
    uint64_t t0 = rdtsc64();
    for (int r = 0; r < rows; r++)
        memcpy(dst + (size_t)r * fb->pitch, src + (size_t)r * fb->width, bytes);
    __asm__ volatile("sfence" ::: "memory");
    return rdtsc64() - t0;
}

/* Walk kernel tables for one virtual address; returns leaf pointer + level. */
static uint64_t *walk_leaf(uint64_t *pml4, uint64_t va, int *level) {
    uint64_t *t = pml4;
    for (int lvl = 4; lvl >= 1; lvl--) {
        int idx = (int)((va >> (12 + 9 * (lvl - 1))) & 0x1FF);
        uint64_t *e = &t[idx];
        if (!(*e & VMM_PRESENT)) return NULL;
        if (lvl == 1 || ((lvl == 2 || lvl == 3) && (*e & VMM_HUGE))) { *level = lvl; return e; }
        t = (uint64_t *)phys_to_virt(*e & PTE_ADDR_MASK);
    }
    return NULL;
}

void fb_wc_init(void) {
    if (!fb || !fb->address) return;
    uint64_t va = (uint64_t)fb->address;
    uint64_t len = (uint64_t)fb->pitch * fb->height;
    uint64_t phys = va - vmm_higher_half_offset;
    serial_printf("[fbwc] fb va=%llx phys=%llx len=%llu (%llux%llu pitch %llu)\n",
                  (unsigned long long)va, (unsigned long long)phys, (unsigned long long)len,
                  (unsigned long long)fb->width, (unsigned long long)fb->height,
                  (unsigned long long)fb->pitch);
    if (!cpu_has_pat()) {
        serial_printf("[fbwc] CPU has no PAT - framebuffer left as mapped by loader\n");
        return;
    }
    serial_printf("[fbwc] PAT before=%llx\n", (unsigned long long)rdmsr64(MSR_IA32_PAT));
    fb_wc_pat_init_cpu();
    uint64_t pat = rdmsr64(MSR_IA32_PAT);
    g_pat_ok = ((pat >> 40) & 7) == PAT_WC;
    serial_printf("[fbwc] PAT now=%llx (idx5=%s)\n", (unsigned long long)pat, pat_name(pat >> 40));
    if (!g_pat_ok) return;
    log_mtrrs(phys, len);

    uint64_t *pml4 = vmm_get_kernel_pml4();
    int bench_rows = (int)(fb->height < 256 ? fb->height : 256);
    uint32_t *src = (uint32_t *)kalloc((size_t)fb->width * 4u * (size_t)bench_rows);
    if (src)
        for (int r = 0; r < bench_rows; r++)   /* snapshot: bench rewrites same pixels */
            memcpy(src + (size_t)r * fb->width, (uint8_t *)fb->address + (size_t)r * fb->pitch,
                   (size_t)fb->width * 4u);
    uint64_t before = src ? bench_copy(src, bench_rows) : 0;

    int changed = 0, already = 0, skipped = 0, first_idx = -1;
    for (uint64_t a = va & ~0xFFFULL; a < va + len; ) {
        int lvl = 0;
        uint64_t *e = walk_leaf(pml4, a, &lvl);
        uint64_t span = (lvl == 3) ? (1ULL << 30) : (lvl == 2) ? (1ULL << 21) : 4096;
        if (!e) { skipped++; a += 4096; continue; }
        uint64_t start = a & ~(span - 1);
        int idx = leaf_pat_index(*e, lvl);
        if (first_idx < 0) first_idx = idx;
        if (idx == 5) {
            already++;
        } else if (lvl == 1 || (lvl == 2 && start >= (va & ~0xFFFULL)) || (start >= va && start + span <= va + len)) {
            /* 2 MiB leaves that overhang the fb stay inside the (>= 16 MiB,
             * naturally aligned) VRAM BAR on real GPUs; a 1 GiB leaf could
             * cover unrelated MMIO, so only take it when fully inside. */
            *e = leaf_set_wc(*e, lvl);
            changed++;
        } else {
            skipped++;
        }
        a = start + span;
    }
    flush_tlb_local();
    __asm__ volatile("wbinvd" ::: "memory");
    g_fb_wc = (skipped == 0);
    uint64_t after = src ? bench_copy(src, bench_rows) : 0;
    if (src) kfree(src);
    serial_printf("[fbwc] fb leaves: loader PAT idx=%d (%s), already WC=%d, changed=%d, skipped=%d\n",
                  first_idx, first_idx >= 0 ? pat_name(pat >> (8 * first_idx)) : "-",
                  already, changed, skipped);
    serial_printf("[fbwc] bench %d rows x %llu px: before=%llu cyc after=%llu cyc\n",
                  bench_rows, (unsigned long long)fb->width,
                  (unsigned long long)before, (unsigned long long)after);
}

bool fb_wc_active(void) { return g_fb_wc; }
