#pragma once

#include "types.h"
#include "cpuid.h"

bool cpu_has_msr();
void cpu_get_msr(uint32_t msr, uint32_t *lo, uint32_t *hi);
void cpu_set_msr(uint32_t msr, uint32_t lo, uint32_t hi);