#pragma once 
#include "types.h"

extern uint32_t bsp_lapic_id;
extern uint64_t cpu_count;

extern void cpu_init();

char* cpu_name();