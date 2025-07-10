#include "debug.h"
#include "mem.h"
#include "utils.h"
#include "atomic.h"

// Simple allocation tracker
#define MAX_ALLOCATIONS 1024
struct allocation_entry {
    void* ptr;
    size_t size;
    const char* file;
    int line;
    bool active;
};

static struct allocation_entry allocations[MAX_ALLOCATIONS];
static spinlock_t alloc_lock = {0};
static uint64_t total_allocations = 0;
static uint64_t total_deallocations = 0;

void debug_memory_check(const char* location) {
    printf("DEBUG: Memory check at %s\n", location);
    
    // Basic heap integrity check
    debug_heap_check();
    
    // Check for obvious stack corruption
    debug_stack_check();
    
    printf("DEBUG: Memory check complete\n");
}

void debug_heap_check(void) {
    printf("DEBUG: Heap integrity check\n");
    // This would check heap metadata if we had a more complex allocator
    printf("DEBUG: Total allocations: %lld, deallocations: %lld\n", 
           total_allocations, total_deallocations);
}

void debug_stack_check(void) {
    uint64_t current_rsp;
    __asm__ volatile("mov %%rsp, %0" : "=r" (current_rsp));
    
    printf("DEBUG: Stack check - RSP: 0x%llx\n", current_rsp);
    
    // Check if we're in a reasonable kernel stack range
    // Limine bootloader sets up stack in higher half memory
    uint64_t kernel_stack_base = 0xffff800000000000ULL;
    uint64_t kernel_stack_top = 0xffff800100000000ULL;
    
    if(current_rsp < kernel_stack_base || current_rsp > kernel_stack_top) {
        printf("WARNING: Stack pointer outside kernel range!\n");
        printf("  Expected: 0x%llx - 0x%llx\n", kernel_stack_base, kernel_stack_top);
        printf("  Actual: 0x%llx\n", current_rsp);
    } else {
        printf("DEBUG: Stack pointer is in valid kernel range\n");
    }
}

void debug_analyze_address(uint64_t addr) {
    printf("DEBUG: Analyzing faulting address 0x%llx\n", addr);
    
    // Check if it's in kernel space
    if(addr < 0x1000) {
        printf("  - NULL pointer dereference (addr < 0x1000)\n");
    } else if(addr < 0x100000) {
        printf("  - Low memory access (possible uninitialized pointer)\n");
    } else if(addr >= 0xFFFF800000000000ULL) {
        printf("  - Kernel space address\n");
    } else {
        printf("  - User space address\n");
    }
    
    // Check if it's aligned
    if(addr % 8 != 0) {
        printf("  - Unaligned access (addr %% 8 = %d)\n", (int)(addr % 8));
    }
    
    // Check if it looks like a corrupted pointer
    if((addr & 0xFF) == 0xCC) {
        printf("  - Looks like poisoned memory (0xCC pattern)\n");
    }
    if((addr & 0xFFFFFFFF) == 0xDEADBEEF) {
        printf("  - Looks like debug pattern (0xDEADBEEF)\n");
    }
}

void debug_track_allocation(void* ptr, size_t size, const char* file, int line) {
    spinlock_lock(&alloc_lock);
    
    // Find empty slot
    for(int i = 0; i < MAX_ALLOCATIONS; i++) {
        if(!allocations[i].active) {
            allocations[i].ptr = ptr;
            allocations[i].size = size;
            allocations[i].file = file;
            allocations[i].line = line;
            allocations[i].active = true;
            break;
        }
    }
    
    total_allocations++;
    spinlock_unlock(&alloc_lock);
}

void debug_track_deallocation(void* ptr, const char* file, int line) {
    spinlock_lock(&alloc_lock);
    
    // Find and mark as inactive
    bool found = false;
    for(int i = 0; i < MAX_ALLOCATIONS; i++) {
        if(allocations[i].active && allocations[i].ptr == ptr) {
            allocations[i].active = false;
            found = true;
            break;
        }
    }
    
    if(!found) {
        printf("WARNING: Double free or invalid free at %s:%d\n", file, line);
    }
    
    total_deallocations++;
    spinlock_unlock(&alloc_lock);
}

void debug_print_allocations(void) {
    spinlock_lock(&alloc_lock);
    
    printf("Active allocations:\n");
    int count = 0;
    for(int i = 0; i < MAX_ALLOCATIONS; i++) {
        if(allocations[i].active) {
            printf("  %d: ptr=%p, size=%d, at %s:%d\n", 
                   count++, allocations[i].ptr, allocations[i].size,
                   allocations[i].file, allocations[i].line);
        }
    }
    printf("Total: %d active allocations\n", count);
    
    spinlock_unlock(&alloc_lock);
}

void debug_memory_poison(void* ptr, size_t size) {
    uint8_t* bytes = (uint8_t*)ptr;
    for(size_t i = 0; i < size; i++) {
        bytes[i] = DEBUG_POISON_BYTE;
    }
}

bool debug_memory_check_poison(void* ptr, size_t size) {
    uint8_t* bytes = (uint8_t*)ptr;
    for(size_t i = 0; i < size; i++) {
        if(bytes[i] != DEBUG_POISON_BYTE) {
            return false;
        }
    }
    return true;
} 