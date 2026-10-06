#include "luade.h"
#include "luade_graphics.h"
#include "luade_input.h"
#include "lua_runtime.h"
#include "mem.h"
#include "atomic.h"  // for irq_save/irq_restore
#include "term.h"
#include "fat32.h"
#include "serial.h"
#include "hpet.h"
#include "debug_config.h"
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include "string.h"

/* LuaDE - Lua Desktop Environment Core */

static lua_State *luade_L = NULL;
static bool luade_running = false;

// Track last executed Lua location for diagnostics
static int luade_last_line = 0;
static char luade_last_src[128];
static char luade_last_func[64];
static char luade_current_src[128];

#define LUADE_TRACE_DEPTH 16
typedef struct {
    int line;
    char src[64];
    char func[64];
    char ev; // 'L' line, 'C' call, 'R' return
} LuadeTraceEntry;
static LuadeTraceEntry luade_trace[LUADE_TRACE_DEPTH];
static int luade_trace_head = 0; // next write position

static inline void luade_trace_push(char ev, const char *src, int line, const char *func) {
    LuadeTraceEntry *e = &luade_trace[luade_trace_head];
    e->ev = ev;
    e->line = line;
    size_t nsrc = src ? strlen(src) : 0;
    if (nsrc >= sizeof(e->src)) nsrc = sizeof(e->src) - 1;
    if (nsrc && src) memcpy(e->src, src, nsrc);
    e->src[nsrc] = '\0';
    size_t nfn = func ? strlen(func) : 0;
    if (nfn >= sizeof(e->func)) nfn = sizeof(e->func) - 1;
    if (nfn && func) memcpy(e->func, func, nfn);
    e->func[nfn] = '\0';
    luade_trace_head = (luade_trace_head + 1) % LUADE_TRACE_DEPTH;
}

static void luade_line_hook(lua_State *L, lua_Debug *ar) {
    if (!ar) return;
    char ev = '?';
    const char *src = "?";
    const char *fname = "?";
    int line = 0;

    if (ar->event == LUA_HOOKLINE) {
        // Do NOT call lua_getinfo here; rely on cached source from CALL events
        line = ar->currentline;
        ev = 'L';
        luade_last_line = line;
        // Use current cached source if available; else leave last source
        if (luade_current_src[0] != '\0') {
            size_t n = strlen(luade_current_src);
            if (n >= sizeof(luade_last_src)) n = sizeof(luade_last_src) - 1;
            memcpy(luade_last_src, luade_current_src, n);
            luade_last_src[n] = '\0';
            src = luade_last_src;
        } else {
            src = luade_last_src[0] ? luade_last_src : "?";
        }
    } else if (ar->event == LUA_HOOKCALL) {
        // Minimal: only fetch source info once per call; no function names
        if (!lua_getinfo(L, "S", ar)) return;
        src = ar->short_src;  /* array member, never NULL */
        size_t n = strlen(src);
        if (n >= sizeof(luade_current_src)) n = sizeof(luade_current_src) - 1;
        memcpy(luade_current_src, src, n);
        luade_current_src[n] = '\0';
        ev = 'C';
        // Do not update last_func; keep it blank to avoid confusion
    } else {
        // Ignore other events
        return;
    }

    luade_trace_push(ev, src, line, "");
}

/* One-shot reader for lua_load in C */
typedef struct { const char *p; size_t n; int done; } LuadeReaderBuf;
static const char *luade_reader_once(lua_State *L, void *ud, size_t *sz) {
    (void)L;
    LuadeReaderBuf *r = (LuadeReaderBuf *)ud;
    if (r->done) { *sz = 0; return NULL; }
    r->done = 1; *sz = r->n; return r->p;
}

// Lua function: luade.log(message) — no console I/O unless DEBUG_LUA (see debug_config.h)
static int l_luade_log(lua_State *L) {
    int n = lua_gettop(L);
    for (int i = 1; i <= n; i++) {
        size_t len = 0;
        const char *s = lua_tolstring(L, i, &len);
#if DEBUG_LUA
        if (s)
            printf("[LuaDE] %s\n", s);
#else
        (void)s;
#endif
    }
    return 0;
}

// Lua function: luade.sleep(ms) — HPET-based wait when available
static int l_luade_sleep(lua_State *L) {
    int ms = luaL_checkinteger(L, 1);
    if (ms <= 0)
        return 0;
    if (hpet_main_cnt && hpet_get_frequency() > 0) {
        hpet_sleep((uint64_t)ms);
    } else {
        for (volatile int i = 0; i < ms * 1000; i++) { }
    }
    return 0;
}

// Lua function: luade.exit()
static int l_luade_exit(lua_State *L) {
    (void)L;
    luade_running = false;
    printf("[LuaDE] Desktop environment shutting down\n");
    return 0;
}

// Initialize LuaDE system
bool luade_init(void) {
    DEBUG_PRINT(LUA, "[LuaDE] Initializing Lua Desktop Environment\n");

    luade_L = lua_newstate(lua_kernel_allocator, NULL);
    if (!luade_L) {
        printf("[LuaDE] Failed to create Lua state\n");
        return false;
    }
    
    // Load basic Lua libraries (math library now has proper implementations)
    luaL_requiref(luade_L, LUA_STRLIBNAME, luaopen_string, 1);
    lua_pop(luade_L, 1);
    luaL_requiref(luade_L, LUA_TABLIBNAME, luaopen_table, 1);
    lua_pop(luade_L, 1);
    luaL_requiref(luade_L, LUA_MATHLIBNAME, luaopen_math, 1);
    lua_pop(luade_L, 1);
    
    // Register LuaDE APIs
    DEBUG_PRINT(LUA, "[LuaDE] About to register graphics APIs\n");
    luade_register_graphics(luade_L);
    DEBUG_PRINT(LUA, "[LuaDE] Graphics APIs registered\n");
    DEBUG_PRINT(LUA, "[LuaDE] About to register input APIs\n");
    luade_register_input(luade_L);
    DEBUG_PRINT(LUA, "[LuaDE] Input APIs registered\n");
    
    // Create luade table with core functions
    lua_newtable(luade_L);
    lua_pushcfunction(luade_L, l_luade_log);
    lua_setfield(luade_L, -2, "log");
    lua_pushcfunction(luade_L, l_luade_sleep);
    lua_setfield(luade_L, -2, "sleep");
    lua_pushcfunction(luade_L, l_luade_exit);
    lua_setfield(luade_L, -2, "exit");
    lua_setglobal(luade_L, "luade");
    
    // Override print to use our logging
    lua_pushcfunction(luade_L, l_luade_log);
    lua_setglobal(luade_L, "print");

    // Install hook to track Lua source/line and calls/returns
    luade_last_line = 0;
    luade_last_src[0] = '\0';
    luade_last_func[0] = '\0';
    for (int i = 0; i < LUADE_TRACE_DEPTH; ++i) {
        luade_trace[i].ev = 0;
        luade_trace[i].line = 0;
        luade_trace[i].src[0] = '\0';
        luade_trace[i].func[0] = '\0';
    }
    luade_trace_head = 0;
    lua_sethook(luade_L, luade_line_hook, LUA_MASKLINE | LUA_MASKCALL, 0);
    
    luade_running = true;
    DEBUG_PRINT(LUA, "[LuaDE] Initialization complete\n");
    return true;
}

// Run Lua script from string in LuaDE context (unprotected)
bool luade_run_script(const char *script) {
    if (!luade_L) {
        printf("[LuaDE] Error: Not initialized\n");
        return false;
    }

    LuadeReaderBuf R = { script, strlen(script), 0 };
    int load_res = lua_load(luade_L, luade_reader_once, &R, "(script)", NULL);
    if (load_res != LUA_OK) {
        const char *err = lua_tostring(luade_L, -1);
        printf("[LuaDE] Load error: %s\n", err ? err : "unknown");
        lua_pop(luade_L, 1);
        return false;
    }

    // Call without protection; keep interrupts enabled so input IRQs keep flowing
    lua_call(luade_L, 0, LUA_MULTRET);
    return true;
}

// Load and run Lua file from filesystem (unprotected)
bool luade_run_file(const char *filename) {
    if (!luade_L) {
        printf("[LuaDE] Error: Not initialized\n");
        return false;
    }
    
    // Memory allocator is now interrupt-safe, no need to disable interrupts
    
    // Try to open the file
    fat32_file_t *file = fat32_open(filename, "r");
    if (!file) {
        printf("[LuaDE] Error: Could not open file '%s'\n", filename);
        // No longer need interrupt management
        return false;
    }
    
    // Get file size
    uint32_t file_size = file->file_size;
    if (file_size == 0) {
        printf("[LuaDE] Error: File '%s' is empty\n", filename);
        fat32_close(file);
        return false;
    }
    
    // Allocate buffer for file content
    char *buffer = kalloc(file_size + 1);
    if (!buffer) {
        printf("[LuaDE] Error: Could not allocate memory for file '%s'\n", filename);
        fat32_close(file);
        // No longer need interrupt management
        return false;
    }
    
    // Read file content
    uint32_t bytes_read = fat32_read(file, buffer, file_size);
    if (bytes_read != file_size) {
        printf("[LuaDE] Error: Could not read file '%s' completely\n", filename);
        kfree(buffer);
        fat32_close(file);
        return false;
    }
    
    // Null-terminate the buffer
    buffer[file_size] = '\0';
    
    // Close file
    fat32_close(file);
    
    // Load via lua_load with one-shot reader
    LuadeReaderBuf R = { buffer, (size_t)file_size, 0 };
    int load_res = lua_load(luade_L, luade_reader_once, &R, filename, NULL);
    // buffer no longer needed after load
    kfree(buffer);

    if (load_res != LUA_OK) {
        const char *err = lua_tostring(luade_L, -1);
        printf("[LuaDE] Load error in '%s': %s\n", filename, err ? err : "unknown");
        lua_pop(luade_L, 1);
        return false;
    }

    // Call without protection; keep interrupts enabled so input IRQs keep flowing
    lua_call(luade_L, 0, LUA_MULTRET);
    return true;
}

// Main LuaDE event loop
void luade_main_loop(void) {
    if (!luade_L) {
        printf("[LuaDE] Error: Not initialized\n");
        return;
    }
    
    printf("[LuaDE] Starting main event loop\n");
    
    // Try safer scripts first to isolate crashes
    if (luade_run_file("sdeskt.lua") || luade_run_file("safe_desktop.lua") || luade_run_file("desktop.lua")) {
        printf("[LuaDE] Desktop environment loaded\n");
    } else {
        printf("[LuaDE] Could not load external desktop.lua, using fallback\n");
        
        // Fallback: minimal desktop environment
        const char *fallback_desktop = 
            "local width, height = gfx.get_screen_size()\n"
            "local running = true\n"
            "local frame_count = 0\n"
            "gfx.clear_screen(gfx.rgb(20, 50, 100))\n"
            "gfx.fill_rect(10, 10, 200, 50, gfx.WHITE)\n"
            "gfx.fill_rect(12, 12, 196, 46, gfx.rgb(50, 50, 50))\n"
            "while running do\n"
            "    local event = input.poll_event()\n"
            "    if event and event.type == 'key_press' and event.key == 27 then\n"
            "        running = false\n"
            "    end\n"
            "    if frame_count % 60 == 0 then\n"
            "        gfx.fill_rect(220, 20, 100, 20, gfx.rgb(frame_count % 255, 100, 200))\n"
            "    end\n"
            "    frame_count = frame_count + 1\n"
            "    luade.sleep(50)\n"
            "end\n";
            
        if (!luade_run_script(fallback_desktop)) {
            printf("[LuaDE] Failed to start fallback desktop\n");
            return;
        }
    }
    
    printf("[LuaDE] Desktop environment exited normally\n");
}

// Shutdown LuaDE
void luade_shutdown(void) {
    // Cleanup input system first to stop interrupt handlers
    luade_input_cleanup();
    
    if (luade_L) {
        lua_close(luade_L);
        luade_L = NULL;
    }
    luade_running = false;
    printf("[LuaDE] Shutdown complete\n");
}

// Print last executed Lua location
void luade_print_last_lua_location(void) {
    if (luade_last_src[0] == '\0' || luade_last_line <= 0) {
        serial_puts("   No recorded Lua line (idle or not running)\n");
    } else {
        serial_printf("   Last Lua location: %s:%d\n", luade_last_src, luade_last_line);
    }
    if (luade_last_func[0] != '\0') {
        serial_printf("   Current Lua function: %s\n", luade_last_func);
    }
    serial_puts("   Recent Lua trace (newest first):\n");
    int printed = 0;
    for (int i = 0; i < LUADE_TRACE_DEPTH; ++i) {
        int idx = (luade_trace_head - 1 - i);
        if (idx < 0) idx += LUADE_TRACE_DEPTH;
        LuadeTraceEntry *e = &luade_trace[idx];
        if (!e->ev && !e->src[0] && !e->func[0] && e->line == 0) break; // stop at empty entries
        const char *evname = (e->ev == 'L' ? "line" : (e->ev == 'C' ? "call" : (e->ev == 'R' ? "ret" : "?")));
        if (e->line > 0) {
            serial_printf("      - %s: %s:%d (%s)\n", evname, e->src[0] ? e->src : "?", e->line, e->func[0] ? e->func : "?");
        } else {
            serial_printf("      - %s: %s (%s)\n", evname, e->src[0] ? e->src : "?", e->func[0] ? e->func : "?");
        }
        if (++printed >= 10) break;
    }
}
