# Syscall System Implementation

## Overview

This document describes the comprehensive syscall system that has been implemented for inDevOS. The syscall system provides a proper interface between user programs and the kernel, replacing the previous direct function call approach.

## What Was Implemented

### 1. Core Syscall Infrastructure

- **Interrupt Handler**: Syscall interrupt handler (int 0x80, vector 128) in `src/syscallasm.asm`
- **Dispatcher**: Main syscall dispatcher in `src/syscall.c` that routes calls to specific implementations
- **Process Management**: Basic process structure to track file descriptors and process state

### 2. Implemented Syscalls

#### File Operations
- `SYS_OPEN` (3) - Open files with various flags (O_RDONLY, O_WRONLY, O_RDWR, O_CREAT)
- `SYS_READ` (1) - Read from files or stdin
- `SYS_WRITE` (2) - Write to files or stdout/stderr
- `SYS_CLOSE` (4) - Close file descriptors
- `SYS_SEEK` (5) - Seek within files

#### File System Operations
- `SYS_MKDIR` (16) - Create directories
- `SYS_UNLINK` (18) - Delete files
- `SYS_STAT` (19) - Get file information
- `SYS_GETCWD` (20) - Get current working directory
- `SYS_CHDIR` (21) - Change current directory
- `SYS_MOUNT` (22) - Mount filesystems
- `SYS_UNMOUNT` (23) - Unmount filesystems

#### Memory Management
- `SYS_BRK` (6) - Manage heap break point
- `SYS_MMAP` (7) - Memory mapping (simplified implementation)
- `SYS_MUNMAP` (8) - Unmap memory

#### Process Management
- `SYS_EXIT` (0) - Exit program
- `SYS_GETPID` (9) - Get process ID
- `SYS_EXEC` (11) - Execute programs
- `SYS_FORK` (10) - Fork processes (not yet implemented)

#### I/O Operations
- `SYS_PRINT` (14) - Print strings to console
- `SYS_GETCHAR` (15) - Read character from keyboard

#### Time Operations
- `SYS_TIME` (13) - Get system time
- `SYS_SLEEP` (12) - Sleep for specified time

#### Network Operations
- `SYS_NETWORK` (25) - Network operations (get MAC, status, etc.)

### 3. User Space Interface

#### Header File: `include/syscalls.h`
- Syscall number definitions
- Wrapper functions for easy use
- Basic C library functions (printf, puts, strlen, etc.)
- Type definitions (stat_t, off_t, pid_t)

#### Low-level Interface
- Inline assembly syscall() function
- Register-based parameter passing following System V ABI

## How to Use Syscalls

### In User Programs

1. Include the syscalls header:
```c
#include "include/syscalls.h"
```

2. Use the convenient wrapper functions:
```c
int main() {
    print("Hello from syscalls!\n");
    
    int fd = open("test.txt", O_CREAT | O_WRONLY);
    if (fd >= 0) {
        write(fd, "Hello World!", 12);
        close(fd);
    }
    
    sleep(1);
    exit(0);
}
```

3. Or use the low-level interface directly:
```c
uint64_t result = syscall(SYS_WRITE, STDOUT_FILENO, (uint64_t)"Hello\n", 6, 0, 0);
```

### Building User Programs

Use the provided build script as a template:
```bash
./build_syscall_test.sh
```

Or compile manually:
```bash
gcc -g -m64 -ffreestanding -fno-pie -mcmodel=kernel -nostdlib -nodefaultlibs -I. -o myprogram.exe myprogram.c
```

## Architecture

### Syscall Flow

1. User program executes `int 0x80` instruction
2. CPU transfers control to `syscall_asm_handler` in `src/syscallasm.asm`
3. Assembly handler saves registers and calls `syscall_handler()` in C
4. C dispatcher routes to appropriate syscall implementation
5. Result is returned through registers back to user program

### Parameter Passing

Syscalls follow the System V ABI for parameter passing:
- `rax`: Syscall number
- `rdi`: First argument
- `rsi`: Second argument  
- `rdx`: Third argument
- `r10`: Fourth argument
- `r8`: Fifth argument
- `r9`: Sixth argument

Return value is placed in `rax`.

### File Descriptor Management

- Each process has up to 16 file descriptors
- FD 0, 1, 2 are reserved for stdin, stdout, stderr
- File descriptors 3-15 are available for user files
- Automatic cleanup when closing file descriptors

## Testing

A comprehensive test program `syscall_test.c` has been created that tests:
- Process information (getpid)
- Time functions
- File operations (open, write, read, close)
- Console I/O
- Sleep functionality

To test:
1. Build the test program: `./build_syscall_test.sh`
2. Copy `syscall_test.exe` to your disk image
3. Boot the OS and run: `exec syscall_test.exe`

## Integration with Existing OS

The syscall system integrates seamlessly with existing OS components:
- **FAT32 filesystem**: File operations use existing FAT32 driver
- **Memory manager**: Memory syscalls use existing kalloc/kfree
- **Timer system**: Sleep uses HPET timer
- **Keyboard/Console**: I/O syscalls use existing drivers
- **Storage**: File operations work with mounted storage devices

## Future Improvements

1. **Process isolation**: Implement proper user/kernel mode separation
2. **Signal handling**: Add signal delivery mechanism
3. **Pipe/IPC**: Inter-process communication
4. **Advanced memory management**: Proper virtual memory for user processes
5. **Fork implementation**: Complete process creation
6. **Directory listing**: Implement readdir functionality
7. **Permission system**: Add file and process permissions

## Files Added/Modified

### New Files
- `src/syscall.h` - Syscall definitions and declarations
- `src/syscall.c` - Syscall implementations
- `src/syscallasm.asm` - Assembly syscall handler
- `include/syscalls.h` - User space syscall interface
- `syscall_test.c` - Test program
- `build_syscall_test.sh` - Build script for test program

### Modified Files
- `src/kernel.c` - Added syscall_init() call
- `src/idtasm.asm` - Added syscall_asm_handler reference
- `Makefile` - Automatic inclusion of new source files

## Conclusion

This syscall implementation provides a solid foundation for user-kernel interaction in inDevOS. It follows standard Unix-like conventions while being tailored to the specific needs and capabilities of your operating system. The system is extensible and can be enhanced as the OS grows in complexity.











