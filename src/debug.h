#pragma once
#include "types.h"
#include "utils.h"

// Function entry debugging
#ifdef DEBUG
    #define DEBUG_ENTER() printf("DEBUG: Entering %s()\n", __func__)
    #define DEBUG_ENTER_ARGS(fmt, ...) printf("DEBUG: Entering %s(" fmt ")\n", __func__, __VA_ARGS__)
    #define DEBUG_EXIT() printf("DEBUG: Exiting %s()\n", __func__)
    #define DEBUG_MSG(fmt, ...) printf("DEBUG: %s: " fmt "\n", __func__, __VA_ARGS__)
    #define DEBUG_POINT(msg) printf("DEBUG: %s: %s\n", __func__, msg)
#else
    #define DEBUG_ENTER() do {} while(0)
    #define DEBUG_ENTER_ARGS(fmt, ...) do {} while(0)
    #define DEBUG_EXIT() do {} while(0)
    #define DEBUG_MSG(fmt, ...) do {} while(0)
    #define DEBUG_POINT(msg) do {} while(0)
#endif

// Memory debugging functions
void debug_memory_check(const char* location);
void debug_heap_check(void);
void debug_stack_check(void);
void debug_print_memory_map(void);

// Memory guard functions
void debug_memory_poison(void* ptr, size_t size);
bool debug_memory_check_poison(void* ptr, size_t size);

// Stack canary functions
void debug_stack_canary_init(void);
bool debug_stack_canary_check(void);

// Memory tracking
void debug_track_allocation(void* ptr, size_t size, const char* file, int line);
void debug_track_deallocation(void* ptr, const char* file, int line);
void debug_print_allocations(void);

// Page fault analysis
void debug_analyze_address(uint64_t addr);

// Magic patterns for memory debugging
#define DEBUG_MAGIC_ALLOC   0xDEADBEEF
#define DEBUG_MAGIC_FREE    0xFEEDFACE
#define DEBUG_POISON_BYTE   0xCC
#define DEBUG_CANARY_VALUE  0x1337BEEF

// Enhanced allocation tracking macros
#ifdef DEBUG
    #define DEBUG_MALLOC(size) \
        ({ void* ptr = malloc(size); \
           debug_track_allocation(ptr, size, __FILE__, __LINE__); \
           ptr; })
    #define DEBUG_FREE(ptr) \
        ({ debug_track_deallocation(ptr, __FILE__, __LINE__); \
           free(ptr); })
#else
    #define DEBUG_MALLOC(size) malloc(size)
    #define DEBUG_FREE(ptr) free(ptr)
#endif 