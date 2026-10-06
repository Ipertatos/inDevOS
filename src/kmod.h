/**
 * @file kmod.h
 * @brief Kernel Module (kmod) subsystem
 *
 * Provides the ability to load, unload, and manage kernel modules at runtime.
 * Modules are ELF relocatable object files (.ko) loaded from the filesystem.
 * Each module exports an init and cleanup function for lifecycle management.
 *
 * Module interface:
 *   int  module_init(void)  - Called when module is loaded, return 0 on success
 *   void module_cleanup(void) - Called when module is unloaded
 */

#ifndef KMOD_H
#define KMOD_H

#include "types.h"

/** @brief Maximum number of simultaneously loaded modules */
#define KMOD_MAX_MODULES    32

/** @brief Maximum module name length */
#define KMOD_MAX_NAME       64

/** @brief Maximum number of exported symbols per module */
#define KMOD_MAX_EXPORTS    128

/** @brief Module state */
typedef enum {
    KMOD_STATE_UNLOADED = 0,    /**< Module is not loaded */
    KMOD_STATE_LOADING,         /**< Module is being loaded */
    KMOD_STATE_LIVE,            /**< Module is loaded and running */
    KMOD_STATE_UNLOADING,       /**< Module is being unloaded */
    KMOD_STATE_FAILED           /**< Module load/init failed */
} kmod_state_t;

/** @brief Exported symbol entry */
typedef struct {
    char name[64];              /**< Symbol name */
    uint64_t address;           /**< Symbol address in memory */
} kmod_symbol_t;

/** @brief Module descriptor */
typedef struct kmod {
    char name[KMOD_MAX_NAME];       /**< Module name */
    kmod_state_t state;             /**< Current module state */

    /* Memory allocation */
    void *text_base;                /**< Base of loaded code section */
    uint64_t text_size;             /**< Size of code section */
    void *data_base;                /**< Base of loaded data section */
    uint64_t data_size;             /**< Size of data section */

    /* Entry points */
    int (*init)(void);              /**< Module init function */
    void (*cleanup)(void);          /**< Module cleanup function */

    /* Exported symbols */
    kmod_symbol_t exports[KMOD_MAX_EXPORTS];
    uint32_t num_exports;           /**< Number of exported symbols */

    /* Metadata */
    char description[128];          /**< Module description */
    char version[32];               /**< Module version string */
    char author[64];                /**< Module author */

    /* Dependency tracking */
    uint32_t ref_count;             /**< Reference count */
} kmod_t;

/* ============================================================================
 * Module Subsystem API
 * ============================================================================ */

/**
 * @brief Initialize the kernel module subsystem
 *
 * Must be called once during kernel boot before any module operations.
 */
void kmod_init(void);

/**
 * @brief Load a kernel module from a file
 *
 * Reads the module file from the filesystem, loads it into memory,
 * resolves symbols, and calls the module's init function.
 *
 * @param filename  Path to the module file (e.g. "mydriver.ko")
 * @return 0 on success, -1 on error
 */
int kmod_load(const char *filename);

/**
 * @brief Unload a kernel module by name
 *
 * Calls the module's cleanup function and frees its memory.
 * Fails if the module has a non-zero reference count.
 *
 * @param name  Module name
 * @return 0 on success, -1 on error
 */
int kmod_unload(const char *name);

/**
 * @brief List all loaded modules
 *
 * Prints information about each loaded module to the console.
 */
void kmod_list(void);

/**
 * @brief Find a loaded module by name
 *
 * @param name  Module name to search for
 * @return Pointer to the module descriptor, or NULL if not found
 */
kmod_t* kmod_find(const char *name);

/**
 * @brief Get a module's state
 * @param name Module name
 * @return Module state, or KMOD_STATE_UNLOADED if not found
 */
kmod_state_t kmod_get_state(const char *name);

/* ============================================================================
 * Symbol Resolution API
 * ============================================================================ */

/**
 * @brief Register a kernel symbol that modules can link against
 *
 * Modules can call kernel functions by linking against exported symbols.
 * Core kernel functions should be exported using this mechanism.
 *
 * @param name    Symbol name
 * @param address Symbol address
 * @return 0 on success, -1 if symbol table is full
 */
int kmod_export_symbol(const char *name, uint64_t address);

/**
 * @brief Resolve a symbol by name
 *
 * Searches kernel exports and loaded module exports for the named symbol.
 *
 * @param name Symbol name to resolve
 * @return Symbol address, or 0 if not found
 */
uint64_t kmod_resolve_symbol(const char *name);

/**
 * @brief Get the number of loaded modules
 * @return Count of modules in LIVE state
 */
uint32_t kmod_count(void);

#endif /* KMOD_H */
