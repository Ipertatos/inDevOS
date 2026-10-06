#ifndef LUA_RUNTIME_H
#define LUA_RUNTIME_H

#include "types.h"

/*
 * Lua runtime embedding shim.
 *
 * This interface lets the OS execute a Lua script that has already been
 * loaded into memory (e.g., by FAT32). The implementation is conditional:
 * - If ENABLE_LUA is defined and third-party Lua sources are integrated,
 *   the implementation will initialize a Lua state and run the script.
 * - Otherwise, it will print an instructional message and return an error.
 */

/* Returns 0 on success, negative value on failure. */
int lua_run_script_from_buffer(const char *name,
                               const void *script_bytes,
                               uint32_t script_size);

#ifdef ENABLE_LUA
/* Lua kernel allocator function for LuaDE */
void *lua_kernel_allocator(void *ud, void *ptr, size_t osize, size_t nsize);
#endif

#endif /* LUA_RUNTIME_H */







