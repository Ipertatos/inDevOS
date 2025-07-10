#pragma once
#include "types.h"
typedef struct {
    uint64_t addr;
    const char* name;
} symbol_t;
extern const symbol_t kernel_symbols[];
extern const int kernel_symbols_count;
const char* lookup_symbol(uint64_t addr); 