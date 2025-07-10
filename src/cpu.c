#include "cpu.h"
#include "limine.h"
#include "apic.h"
#include "mem.h"
#include "utils.h"
#include "atomic.h"
#include <string.h>
#define cpuid(in, a, b, c, d) __asm__("cpuid": "=a" (a), "=b" (b), "=c" (c), "=d" (d) : "a" (in));

volatile struct limine_smp_request smp_request = {
    .id = LIMINE_SMP_REQUEST,
    .revision = 0,
    .flags = 0
};
uint32_t bsp_lapic_id = 0;
uint64_t cpu_count;

spinlock_t cpu_lock = {0};

volatile uint64_t _cpus_awake = 1;

void _cpu_awake(struct limine_smp_info *smp_info)
{
    spinlock_lock(&cpu_lock);
    _cpus_awake++;
    printf("cpu: cpu %llu is awake\n", (unsigned long long)smp_info->processor_id);
    spinlock_unlock(&cpu_lock);
    __asm__("hlt");
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
    char cpu_name[49];
    uint32_t regs[4];

    __asm__ __volatile__(
        "cpuid"
        : "=a" (regs[0]), "=b" (regs[1]), "=c" (regs[2]), "=d" (regs[3])
        : "a" (0x80000002)
    );
    memcpy(cpu_name, regs, sizeof(regs));

    __asm__ __volatile__(
        "cpuid"
        : "=a" (regs[0]), "=b" (regs[1]), "=c" (regs[2]), "=d" (regs[3])
        : "a" (0x80000003)
    );
    memcpy(cpu_name + 16, regs, sizeof(regs));

    __asm__ __volatile__(
        "cpuid"
        : "=a" (regs[0]), "=b" (regs[1]), "=c" (regs[2]), "=d" (regs[3])
        : "a" (0x80000004)
    );
    memcpy(cpu_name + 32, regs, sizeof(regs));

    cpu_name[49] = '\0';
    char* temp = kalloc(sizeof(char) * 49);
    
    // Check if allocation was successful
    if (temp == NULL) {
        printf("cpu: Failed to allocate memory for CPU name\n");
        return NULL;
    }
    
    //write cpu_name to temp
    for (int i = 0; i < 49; i++) {
        temp[i] = cpu_name[i];
    }
    return temp;
}