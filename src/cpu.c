#include "cpu.h"
#include "limine.h"
#include "apic.h"
#include "utils.h"

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