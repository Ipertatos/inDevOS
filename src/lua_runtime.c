#include "lua_runtime.h"
#include "term.h"
#include "utils.h"

/*
 * Minimal shim. Real Lua integration is optional to keep default build small.
 * To enable real Lua, vendor Lua 5.4 (e.g., in third_party/lua-5.4/), add the
 * sources to the build, and compile with -DENABLE_LUA. See docs/LUA.md.
 */
 

#ifndef ENABLE_LUA

int lua_run_script_from_buffer(const char *name,
                               const void *script_bytes,
                               uint32_t script_size)
{
    (void)script_bytes;
    (void)script_size;
    printf("Lua is not enabled. Cannot run '%s'.\n", name);
    printf("See docs/LUA.md for setup instructions.\n");
    return -1;
}

#else

/*
 * Example skeleton for when ENABLE_LUA is defined. This assumes you have
 * integrated the Lua sources and headers into the build (e.g., lua.h, lualib.h,
 * lauxlib.h are available on include path and liblua is compiled in-tree).
 */

#include "mem.h"
#include "atomic.h"
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include "string.h"

/* We intentionally open only core (no stdlibs) to avoid libc deps. */

/* INTERRUPT-SAFE: custom allocator that uses kernel allocators. */
void *lua_kernel_allocator(void *ud, void *ptr, size_t osize, size_t nsize)
{
    (void)ud; (void)osize;
    
    // CRITICAL: Disable interrupts to prevent corruption from mouse/keyboard handlers
    uint64_t flags = irq_save();
    
    void *result = NULL;
    
    if (nsize == 0) {
        if (ptr) kfree(ptr);
        result = NULL;
    } else if (!ptr) {
        result = kalloc(nsize);
    } else {
        /* No real realloc; allocate new, copy, free old. */
        void *nptr = kalloc(nsize);
        if (nptr) {
            size_t copy_sz = osize < nsize ? osize : nsize;
            memmove(nptr, ptr, copy_sz);
            kfree(ptr);
            result = nptr;
        } else {
            result = NULL;
        }
    }
    
    // CRITICAL: Restore interrupt state
    irq_restore(flags);
    
    return result;
}

/* Convert any value to string using tostring() if available, else lua_tolstring. */
static const char *to_string(lua_State *L, int idx, size_t *out_len) {
    const char *s = lua_tolstring(L, idx, out_len);
    if (s) return s;
    /* try global tostring */
    lua_getglobal(L, "tostring");          /* ... tostring */
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return "<non-string>"; }
    lua_pushvalue(L, idx < 0 ? idx - 1 : idx); /* ... tostring value */
    if (lua_pcall(L, 1, 1, 0) != LUA_OK) { lua_pop(L, 1); return "<error>"; }
    s = lua_tolstring(L, -1, out_len);
    if (!s) s = "<non-string>";
    lua_pop(L, 1);
    return s;
}

static int l_print(lua_State *L) {
    int n = lua_gettop(L);
    for (int i = 1; i <= n; i++) {
        size_t len = 0;
        const char *s = to_string(L, i, &len);
        if (s) printf("%s", s);
        if (i < n) printf(" ");
    }
    printf("\n");
    return 0;
}

/* Minimal base: type */
static int l_type(lua_State *L) {
    const char *tn = lua_typename(L, lua_type(L, 1));
    lua_pushstring(L, tn ? tn : "nil");
    return 1;
}

/* Minimal ipairs implementation */
static int l_ipairs_iter(lua_State *L) {
    lua_Integer i = lua_tointeger(L, 2);
    lua_Integer next = i + 1;
    lua_pushinteger(L, next);           /* push next index */
    lua_geti(L, 1, next);               /* push value = t[next] */
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1); /* pop nil value */
        return 0;      /* terminate */
    }
    return 2; /* return index, value */
}

static int l_ipairs(lua_State *L) {
    /* returns iterator, table, 0 */
    lua_pushcfunction(L, l_ipairs_iter);
    lua_pushvalue(L, 1);
    lua_pushinteger(L, 0);
    return 3;
}

/* Unprotected pcall replacement to avoid setjmp/longjmp dependency.
 * Returns: true, <all results> on success; false, "pcall disabled" on misuse.
 */
static int l_pcall_unprotected(lua_State *L) {
    int nargs = lua_gettop(L);
    if (nargs < 1 || !lua_isfunction(L, 1)) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "pcall expects function");
        return 2;
    }
    int arg_count = nargs - 1;
    /* Move function to bottom if not already */
    /* Call without protection; assume function won't error in our usage */
    lua_call(L, arg_count, LUA_MULTRET);
    int nres = lua_gettop(L);
    lua_pushboolean(L, 1);
    lua_insert(L, 1);
    return nres + 1;
}

/* Disable coroutines */
static int l_coroutine_disabled(lua_State *Lx) {
    (void)Lx;
    lua_pushnil(Lx);
    lua_pushstring(Lx, "coroutines disabled");
    return 2;
}

/* lua_load reader */
typedef struct { const char *p; size_t n; int done; } LuaReaderBuf;
static const char *lua_reader_once(lua_State *Lr, void *ud, size_t *sz) {
    (void)Lr;
    LuaReaderBuf *r = (LuaReaderBuf*)ud;
    if (r->done) { *sz = 0; return NULL; }
    r->done = 1; *sz = r->n; return r->p;
}

/* Ctrl+C hook: fires every N VM instructions and raises an error if
 * the terminate flag has been set by the keyboard ISR. */
extern bool get_terminate_flag(void);
static void lua_ctrlc_hook(lua_State *L2, lua_Debug *ar) {
    (void)ar;
    if (get_terminate_flag()) {
        luaL_error(L2, "interrupted (^C)");
    }
}

int lua_run_script_from_buffer(const char *name,
                               const void *script_bytes,
                               uint32_t script_size)
{
    extern void set_program_running(bool);
    extern void set_terminate_flag(bool);

    set_program_running(true);
    set_terminate_flag(false);

    lua_State *L = lua_newstate(lua_kernel_allocator, NULL);
    if (!L) {
        printf("lua: failed to create state for '%s'\n", name);
        set_program_running(false);
        return -1;
    }

    /* Open only the libraries we can support in a freestanding kernel. */

    /* String, table libs from upstream; keep base/os/io closed. */
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1); /* _G.string */
    lua_pop(L, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1); /* _G.table */
    lua_pop(L, 1);
    /* Math lib relies on libm; we provide libc shims sufficient for tests. */
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1); /* _G.math */
    lua_pop(L, 1);

    /* Register minimal print */
    lua_pushcfunction(L, l_print);
    lua_setglobal(L, "print");
    /* Define _G to reference the global table */
    lua_pushglobaltable(L);
    lua_setglobal(L, "_G");
    /* Provide minimal base functions: type, ipairs */
    lua_pushcfunction(L, l_type);
    lua_setglobal(L, "type");
    lua_pushcfunction(L, l_ipairs);
    lua_setglobal(L, "ipairs");
    /* Override pcall to avoid dependency on setjmp */
    lua_pushcfunction(L, l_pcall_unprotected);
    lua_setglobal(L, "pcall");

    /* Hook that fires every 1000 VM instructions so Ctrl+C can interrupt. */
    lua_sethook(L, lua_ctrlc_hook, LUA_MASKCOUNT, 1000);

    /* Load buffer with lua_load using a simple reader. */
    LuaReaderBuf R = { (const char *)script_bytes, (size_t)script_size, 0 };
    int load_res = lua_load(L, lua_reader_once, &R, name, NULL);
    if (load_res != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        printf("lua: load error: %s\n", err ? err : "unknown");
        lua_close(L);
        return -1;
    }

    /* Protect runtime call; if longjmp unwinds, we still return control. */
    /* Disable coroutines by overriding coroutine.create to error */
    lua_createtable(L, 0, 1);
    lua_pushcfunction(L, l_coroutine_disabled);
    lua_setfield(L, -2, "create");
    lua_setglobal(L, "coroutine");

    int call_res = lua_pcall(L, 0, LUA_MULTRET, 0);
    if (call_res != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        printf("lua: runtime error: %s\n", err ? err : "unknown");
        lua_close(L);
        set_program_running(false);
        set_terminate_flag(false);
        return -1;
    }

    lua_close(L);
    set_program_running(false);
    set_terminate_flag(false);
    return 0;
}

#endif /* ENABLE_LUA */


