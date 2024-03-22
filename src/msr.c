#include "msr.h"

const uint32_t CPUID_FLAG_MSR = 1 << 5;

bool cpu_has_msr() {
    static uint32_t a, d;
    cpuid(1, &a, &d);
    return d & CPUID_FLAG_MSR;
}

void cpu_get_msr(uint32_t msr, uint32_t* lo, uint32_t* hi) {
    __asm__ volatile("rdmsr"
                     : "=a"(*lo), "=d"(*hi)
                     : "c"(msr));
}

void cpu_set_msr(uint32_t msr, uint32_t lo, uint32_t hi) {
    __asm__ volatile("wrmsr"
                     :
                     : "a"(lo), "d"(hi), "c"(msr));
}