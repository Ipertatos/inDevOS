import sys

print('#include "kernel_symtab.h"')
print('const symbol_t kernel_symbols[] = {')

seen_names = set()

for line in sys.stdin:
    line = line.strip()
    if not line:
        continue
    
    parts = line.split()
    if len(parts) >= 3:
        addr = parts[0]
        symbol_type = parts[1]
        name = parts[2]
        
        # Only include text symbols (functions); skip duplicate names
        if symbol_type in ['t', 'T'] and name not in seen_names:
            seen_names.add(name)
            print(f'    {{0x{addr}, "{name}"}},')

print('};')
print()

# Count the symbols by reading stdin again, but we can't do that
# Instead, let's use a simpler approach - count during generation
print('const int kernel_symbols_count = sizeof(kernel_symbols) / sizeof(kernel_symbols[0]);')
print()

# Include the lookup_symbol function implementation
print('const char* lookup_symbol(uint64_t addr) {')
print('    const char* name = "unknown";')
print('    for (int i = 0; i < kernel_symbols_count; ++i) {')
print('        if (addr < kernel_symbols[i].addr)')
print('            break;')
print('        name = kernel_symbols[i].name;')
print('    }')
print('    return name;')
print('}')
