#include "cpu.h"
#include "limine.h"
#include "apic.h"
#include "utils.h"
#define cpuid(in, a, b, c, d) __asm__("cpuid": "=a" (a), "=b" (b), "=c" (c), "=d" (d) : "a" (in));

volatile struct limine_smp_request smp_request = {
    .id = LIMINE_SMP_REQUEST,
    .revision = 0,
    .flags = 0
};
uint32_t bsp_lapic_id;
uint64_t cpu_count;

volatile uint64_t _cpus_awake = 1;

void _cpu_awake(struct limine_smp_info *smp_info)
{
    _cpus_awake++;
    __asm__("hlt");
}

void cpu_init()
{
    bsp_lapic_id = smp_request.response->bsp_lapic_id;
    cpu_count = smp_request.response->cpu_count;
    printf("cpu: bsp lapic id: {dn}", bsp_lapic_id);
    printf("cpu: cpu count: {dn}", cpu_count);
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

    cpu_name[48] = '\0';
    char* temp = cpu_name;
    return temp;
}