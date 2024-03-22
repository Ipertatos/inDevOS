#pragma once

#include "types.h"
#include "registers.h"
int check_apic();
void cpu_set_apic_base(uint32_t apic);
uint32_t cpu_get_apic_base();
void enable_apic();
uint32_t cpuReadIoApic(void *ioapicaddr, uint32_t reg);
void cpuWriteIoApic(void *ioapicaddr, uint32_t reg, uint32_t value);