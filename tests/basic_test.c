#include "include/syscalls.h"

// Entry point that the kernel expects (no main() when using -nostdlib)
void _start(void) {
    print("Basic test starting...\n");
    
    // Test basic syscall
    int result = print("Hello from basic test!\n");
    
    // Test exit with a different status to see if it's consistent
    print("Exiting with status 5...\n");
    exit(5);
}
