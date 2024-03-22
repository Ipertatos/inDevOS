#include "cpuid.h"

enum cpuid_requests {
    CPUID_GETVENDORSTRING,
    CPUID_GETFEATURES,
    CPUID_GETTLB,
    CPUID_GETSERIAL,

    CPUID_INTELEXTENDED = 0x80000000,
    CPUID_INTELFEATURES,
    CPUID_INTELBRANDSTRING,
    CPUID_INTELBRANDSTRINGMORE,
    CPUID_INTELBRANDSTRINGEND
};

void cpuid(uint32_t code, uint32_t* a, uint32_t* d) {
    __asm__ volatile("cpuid"
                     : "=a"(*a), "=d"(*d)
                     : "a"(code)
                     : "ecx", "ebx");
}

int cpuid_string(uint32_t code, uint32_t where[4]) {
    __asm__ volatile("cpuid"
                     : "=a"(where[0]), "=b"(where[1]), "=c"(where[2]), "=d"(where[3])
                     : "a"(code));
    return (int)where[0];
}