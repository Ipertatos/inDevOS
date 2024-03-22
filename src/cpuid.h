#pragma once
#include "types.h"

enum cpuid_requests;
void cpuid(uint32_t code, uint32_t* a, uint32_t* d);
int cpuid_string(uint32_t code, uint32_t where[4]);
