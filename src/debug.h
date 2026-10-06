#pragma once
#include "types.h"
#include "utils.h"

/* ================================================================
 * Unified kernel logging (klog)
 *
 * One API for all kernel log output instead of mixed printf /
 * serial_printf calls. Every message always goes to the serial port
 * (when enabled); messages at or below the console level also go to
 * the terminal once it is ready (klog_console_ready()).
 * ================================================================ */
#define KLOG_ERR    0   /* unrecoverable / data-loss class problems  */
#define KLOG_WARN   1   /* unexpected but survivable                 */
#define KLOG_INFO   2   /* normal boot / state-change messages       */
#define KLOG_DEBUG  3   /* verbose; compiled in, filtered at runtime */

void klog(int level, const char *fmt, ...);
void klog_set_console_level(int level);
void klog_console_ready(void);

#define klog_err(fmt, ...)   klog(KLOG_ERR,   fmt, ##__VA_ARGS__)
#define klog_warn(fmt, ...)  klog(KLOG_WARN,  fmt, ##__VA_ARGS__)
#define klog_info(fmt, ...)  klog(KLOG_INFO,  fmt, ##__VA_ARGS__)
#define klog_debug(fmt, ...) klog(KLOG_DEBUG, fmt, ##__VA_ARGS__)

/* Hot-path tracing: program loads and filesystem operations.
 *
 * These messages are genuinely useful when something goes wrong, but there is
 * one per open/create/delete plus a dozen per program launch, and each console
 * write costs 15-40 ms under emulation - they dominated program start-up time
 * and flooded the screen while programs were printing.  Off by default;
 * `trace on` in the shell turns them on (serial + console). */
extern bool ktrace_enabled;
void ktrace_set(bool on);

#define ktrace(fmt, ...) \
    do { if (ktrace_enabled) klog(KLOG_DEBUG, fmt, ##__VA_ARGS__); } while (0)

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