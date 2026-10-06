#ifndef LUADE_H
#define LUADE_H

#include <stdbool.h>

/* LuaDE - Lua Desktop Environment Core Header */

// Initialize the LuaDE system
bool luade_init(void);

// Run a Lua script from string in LuaDE context  
bool luade_run_script(const char *script);

// Load and run a Lua file from filesystem in LuaDE context
bool luade_run_file(const char *filename);

// Main LuaDE event loop (blocks until exit)
void luade_main_loop(void);

// Shutdown LuaDE system
void luade_shutdown(void);

// Print last executed Lua source and line (for diagnostics during faults)
void luade_print_last_lua_location(void);

#endif // LUADE_H
