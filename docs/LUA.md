## Lua scripting in inDevOS

inDevOS can execute Lua scripts using the `exec` command. This is useful for rapid prototyping and extending the OS with high-level logic.

### Running scripts

- Place a script on the mounted FAT32 volume (e.g., copy via your tooling).
- From the shell:
  - `exec myscript.lua`
  - or add a shebang and still use `exec myscript`:

```lua
#!/usr/bin/lua
print("Hello from Lua!")
```

Both `.lua` extension and shebang lines containing `lua` are recognized.

If Lua is not enabled in the build, the OS will print:

```
Lua is not enabled. Cannot run 'myscript.lua'.
See docs/LUA.md for setup instructions.
```

### Enabling Lua (building the interpreter in-tree)

By default, Lua is stubbed out to keep the kernel minimal. To enable a real interpreter:

1) Vendor Lua sources

- Download Lua 5.4.x source release.
- Place it at `third_party/lua-5.4/` inside the repo.
- Ensure headers `lua.h`, `lauxlib.h`, `lualib.h` are available under that path.

2) Add to build

- Edit your build to compile Lua sources into the kernel and define `ENABLE_LUA`.
- A minimal set of sources typically includes: `lapi.c lcode.c lctype.c ldebug.c ldo.c ldump.c lfunc.c lgc.c llex.c lmem.c lobject.c lopcodes.c lparser.c lstate.c lstring.c ltable.c ltm.c lundump.c lvm.c lzio.c lauxlib.c lbaselib.c linit.c lmathlib.c loslib.c liolib.c lstrlib.c ltablib.c`.
- Add `-DENABLE_LUA` to `CFLAGS` and add `-Ithird_party/lua-5.4` to the include path.

3) Memory allocator

- The embedding uses a custom allocator that routes Lua allocations through `kalloc`/`kfree`. This avoids requiring libc `malloc`.

4) Build

- Rebuild the kernel. On success, `exec myscript.lua` should run and print output.

### Extending the OS API exposed to Lua

Once Lua is enabled, you can register C functions as Lua globals to call into OS services.

Example (extend `src/lua_runtime.c` under the `#else /* ENABLE_LUA */` section):

```c
static int os_print(lua_State *L) {
    int n = lua_gettop(L);
    for (int i = 1; i <= n; i++) {
        const char *s = lua_tostring(L, i);
        if (!s) s = "<non-string>";
        printf("%s", s);
        if (i < n) printf(" ");
    }
    printf("\n");
    return 0;
}

static void register_os_api(lua_State *L) {
    lua_pushcfunction(L, os_print);
    lua_setglobal(L, "printk");
}

// Call register_os_api(L) right after luaL_openlibs(L);
```

Then from Lua:

```lua
printk("time:", 123)
```

You can add more bindings for filesystem (read-only), timers, networking status, etc., following the same pattern. Keep bindings simple and non-blocking.

### Writing Lua programs

- Use standard Lua 5.4 syntax.
- Keep scripts small; all code is loaded into memory and runs in kernel context.
- Avoid infinite loops; there is no preemption in userland. Cooperative yielding is recommended if you implement it.
- For portability, avoid `os.*` libraries unless you provide custom implementations.

Example script:

```lua
-- list files demo (pseudo, requires you to bind an API first)
for _, name in ipairs(fs_list("/")) do
  print(name)
end
```

### Safety notes

- Scripts run in kernel context. Treat Lua as trusted for now.
- Validate and bound-check in your C bindings.




















































