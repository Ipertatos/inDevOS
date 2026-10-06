#include "cpu.h"
#include "limine.h"
#include "apic.h"
#include "mem.h"
#include "utils.h"
#include "atomic.h"
#include "string.h"
#define cpuid(in, a, b, c, d) __asm__("cpuid": "=a" (a), "=b" (b), "=c" (c), "=d" (d) : "a" (in));

volatile struct limine_smp_request smp_request = {
    .id = LIMINE_SMP_REQUEST,
    .revision = 0,
    .flags = 0
};
uint32_t bsp_lapic_id = 0;
uint64_t cpu_count;

spinlock_t cpu_lock = {0};

bool cpu_smap_enabled = false;
bool cpu_smep_enabled = false;
uint64_t vmm_nx_bit = 0;

#define MSR_EFER        0xC0000080
#define EFER_NXE        (1u << 11)
#define CR4_SMEP        (1ULL << 20)
#define CR4_SMAP        (1ULL << 21)

void cpu_enable_protections(void)
{
    uint32_t a, b, c, d;

    cpuid(0x80000000, a, b, c, d);
    if (a >= 0x80000001) {
        cpuid(0x80000001, a, b, c, d);
        if (d & (1u << 20)) {
            uint32_t lo, hi;
            __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(MSR_EFER));
            lo |= EFER_NXE;
            __asm__ volatile("wrmsr" :: "a"(lo), "d"(hi), "c"(MSR_EFER));
            vmm_nx_bit = 1ULL << 63;
        }
    }

    cpuid(0, a, b, c, d);
    if (a < 7)
        return;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7), "c"(0));

    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    if (b & (1u << 7)) {
        cr4 |= CR4_SMEP;
        cpu_smep_enabled = true;
    }
    if (b & (1u << 20)) {
        __asm__ volatile("clac" ::: "memory");
        cr4 |= CR4_SMAP;
        cpu_smap_enabled = true;
    }
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");
}

volatile uint64_t _cpus_awake = 1;

/* AP redirect mechanism: after initial wake, APs spin-wait here until
 * the SMP subsystem sets _ap_redirect and _ap_release.  Then each AP
 * jumps to the real ap_entry() in smp.c for full initialization. */
void (* volatile _ap_redirect)(struct limine_smp_info *) = NULL;
volatile int _ap_release = 0;

void _cpu_awake(struct limine_smp_info *smp_info)
{
    spinlock_lock(&cpu_lock);
    _cpus_awake++;
    printf("cpu: cpu %llu is awake\n", (unsigned long long)smp_info->processor_id);
    spinlock_unlock(&cpu_lock);

    /* Spin-wait until smp_start_aps() signals us.
     * We use 'pause' to reduce bus traffic while spinning. */
    while (!__atomic_load_n(&_ap_release, __ATOMIC_ACQUIRE)) {
        __asm__ volatile("pause");
    }

    /* Jump to the real AP initialization in smp.c */
    void (*fn)(struct limine_smp_info *) =
        (void (*)(struct limine_smp_info *))__atomic_load_n(
            (void * volatile *)&_ap_redirect, __ATOMIC_ACQUIRE);
    if (fn) {
        fn(smp_info);
    }

    /* Should never reach here */
    while (1) __asm__ volatile("cli; hlt");
}

void cpu_init()
{
    if (smp_request.response == NULL) {
        // SMP not provided by bootloader – assume single-CPU BSP only.
        bsp_lapic_id = 0;
        cpu_count = 1;
        printf("cpu: SMP response missing – falling back to 1 CPU\n");
        return;
    }

    bsp_lapic_id = smp_request.response->bsp_lapic_id;
    cpu_count = smp_request.response->cpu_count;
    printf("cpu: bsp lapic id: %llu\n", (unsigned long long)bsp_lapic_id);
    printf("cpu: cpu count: %llu\n", (unsigned long long)cpu_count);
    for (uint64_t i = 0; i < cpu_count; i++) {
        if (i == bsp_lapic_id) {
            continue;
        }
        smp_request.response->cpus[i]->goto_address = _cpu_awake;
    }
}

char* cpu_name() {
    // Use a static buffer to avoid leaking memory on every call
    static char name_buf[49];
    static bool name_cached = false;

    if (name_cached) return name_buf;

    uint32_t regs[4];

    __asm__ __volatile__(
        "cpuid"
        : "=a" (regs[0]), "=b" (regs[1]), "=c" (regs[2]), "=d" (regs[3])
        : "a" (0x80000002)
    );
    memcpy(name_buf, regs, sizeof(regs));

    __asm__ __volatile__(
        "cpuid"
        : "=a" (regs[0]), "=b" (regs[1]), "=c" (regs[2]), "=d" (regs[3])
        : "a" (0x80000003)
    );
    memcpy(name_buf + 16, regs, sizeof(regs));

    __asm__ __volatile__(
        "cpuid"
        : "=a" (regs[0]), "=b" (regs[1]), "=c" (regs[2]), "=d" (regs[3])
        : "a" (0x80000004)
    );
    memcpy(name_buf + 32, regs, sizeof(regs));

    name_buf[48] = '\0';
    name_cached = true;
    return name_buf;
}