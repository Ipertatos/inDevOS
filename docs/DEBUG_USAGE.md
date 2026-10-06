# Function Entry Debugging System

This document describes the function entry debugging system implemented in inDevOS.

## Overview

The debug system provides macros that print function entry, exit, and debug information when the `DEBUG` macro is defined during compilation. When `DEBUG` is not defined, these macros compile to nothing, ensuring no performance impact in production builds.

## Available Debug Macros

### `DEBUG_ENTER()`
Prints when entering a function.
```c
void my_function() {
    DEBUG_ENTER();
    // ... function body
}
```
Output: `DEBUG: Entering my_function()`

### `DEBUG_ENTER_ARGS(fmt, ...)`
Prints when entering a function with arguments.
```c
void my_function(int arg1, const char* arg2) {
    DEBUG_ENTER_ARGS("arg1=%d, arg2='%s'", arg1, arg2);
    // ... function body
}
```
Output: `DEBUG: Entering my_function(arg1=42, arg2='hello')`
### `DEBUG_EXIT()`
Prints when exiting a function.
```c
void my_function() {
    DEBUG_ENTER();
    // ... function body
    DEBUG_EXIT();
}
```
Output: `DEBUG: Exiting my_function()`

### `DEBUG_MSG(fmt, ...)`
Prints a debug message within a function.
```c
void my_function() {
    DEBUG_ENTER();
    int result = calculate_something();
    DEBUG_MSG("Calculation result: %d", result);
    DEBUG_EXIT();
}
```
Output: `DEBUG: my_function: Calculation result: 42`

### `DEBUG_POINT(msg)`
Prints a debug checkpoint message.
```c
void my_function() {
    DEBUG_ENTER();
    DEBUG_POINT("About to initialize subsystem");
    initialize_subsystem();
    DEBUG_POINT("Subsystem initialized successfully");
    DEBUG_EXIT();
}
```
Output: 
```
DEBUG: my_function: About to initialize subsystem
DEBUG: my_function: Subsystem initialized successfully
```

## Build Targets

### Regular Build
```bash
make run
```
Builds without DEBUG defined. Debug macros are compiled out.

### Debug Build
```bash
make debug-build
```
Builds with DEBUG defined. All debug macros are active.

### Debug Run
```bash
make debug-run
```
Builds and runs the debug version with serial logging.

## Example Usage

Here's how the debug macros are used in kernel functions:

### Kernel Main Function
```c
void kmain(void) {
    DEBUG_ENTER();
    
    serial_init();
    DEBUG_POINT("Serial initialized");
    
    if(LIMINE_BASE_REVISION_SUPPORTED == false) {
        DEBUG_POINT("Limine revision not supported - exiting");
        return;
    }

    DEBUG_POINT("Initializing GDT");
    gdt_init();
    DEBUG_POINT("Initializing IDT");
    initIDT();
    
    // ... rest of function
}
```

### Memory Management Functions
```c
void kmem_init() {
    DEBUG_ENTER();
    printf("Initializing PMM...\n");

    DEBUG_POINT("Initializing memory statistics");
    _init_stats();
    DEBUG_POINT("Creating page bitmap");
    _create_page_bitmap();
    DEBUG_POINT("Creating entry map");
    _create_entry_map();
    DEBUG_POINT("Getting free pages");
    _get_free_pages();

    printf("PMM initialized.\n");
    DEBUG_EXIT();
}
```

### Command Processing
```c
void cmd(char* cmd, uint64_t len) {
    DEBUG_ENTER_ARGS("cmd='%.20s', len=%llu", cmd, len);
    
    char* token = strtok(cmd, " ");     
    token = remove_nl(token);
    DEBUG_MSG("Processing command: '%s'", token);
    
    if(strEql(token, "help")) {
        show_help();
    } else if(strEql(token, "ls")) {
        list_files();
    }
    // ... rest of command processing
}
```

### Interrupt Handlers
```c
void keyboard_handler() {
    DEBUG_ENTER();
    spinlock_lock(&keyboard_lock);
    
    uint16_t scancode = inb(0x60);
    DEBUG_MSG("Received scancode: 0x%02x", scancode);
    
    // ... process scancode
    
    spinlock_unlock(&keyboard_lock);
    apic_eoi();
    DEBUG_EXIT();
}
```

### File System Operations
```c
int fat32_mount(uint8_t storage_device_id) {
    DEBUG_ENTER_ARGS("device_id=%d", storage_device_id);
    
    storage_device_t *dev = storage_get_device(storage_device_id);
    if (!dev) {
        DEBUG_POINT("Storage device not found - returning error");
        return -1;
    }
    
    DEBUG_POINT("Reading boot sector");
    // ... mount logic
    
    DEBUG_POINT("Filesystem mounted successfully");
    return 0;
}
```

## How It Works

1. **Compilation**: When `DEBUG` is defined, macros expand to printf statements
2. **Runtime**: Debug messages are printed to the console/framebuffer
3. **Performance**: When `DEBUG` is not defined, macros compile to empty statements

## Currently Instrumented Functions

The following functions already have debug macros added:

- `kmain()` - Kernel entry point with initialization steps
- `kmem_init()` - Memory manager initialization
- `cmd()` - Command processor with argument logging
- `keyboard_handler()` - Keyboard interrupt handler
- `handle_page_fault()` - Page fault handler with detailed error info
- `fat32_mount()` - Filesystem mounting
- `fat32_open()` - File opening operations

## Adding Debug Macros to New Functions

1. Include the debug header:
```c
#include "debug.h"
```

2. Add `DEBUG_ENTER()` at the beginning of functions
3. Add `DEBUG_EXIT()` before returns (optional)
4. Use `DEBUG_POINT()` for important checkpoints
5. Use `DEBUG_MSG()` for variable values and state information

## Best Practices

1. **Always use `DEBUG_ENTER()`** at function start
2. **Use `DEBUG_ENTER_ARGS()`** for functions with important parameters
3. **Use `DEBUG_POINT()`** for major initialization steps
4. **Use `DEBUG_MSG()`** for variable values and calculations
5. **Use `DEBUG_EXIT()`** for long or complex functions
6. **Don't overuse** - focus on critical paths and error conditions

## Performance Impact

- **Debug Build**: Slight performance overhead due to printf calls
- **Release Build**: Zero performance impact (macros compile to nothing)
- **Memory**: No additional memory usage in release builds

## Example Debug Output

When running with DEBUG enabled, you'll see output like:
```
DEBUG: Entering kmain()
DEBUG: kmain: Initializing GDT
DEBUG: kmain: Initializing IDT
DEBUG: kmain: Initializing framebuffer
DEBUG: Entering kmem_init()
DEBUG: kmem_init: Initializing memory statistics
DEBUG: kmem_init: Creating page bitmap
DEBUG: Exiting kmem_init()
DEBUG: Entering cmd(cmd='help', len=4)
DEBUG: cmd: Processing command: 'help'
DEBUG: Entering keyboard_handler()
DEBUG: keyboard_handler: Received scancode: 0x1c
DEBUG: Exiting keyboard_handler()
```

This system provides comprehensive function tracing for debugging kernel issues, race conditions, and understanding execution flow. 
