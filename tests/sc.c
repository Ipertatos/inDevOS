#include "include/syscalls.h"

// Simple entry point that the kernel expects
void _start() {
    // Test basic syscall functionality
    print("Testing syscalls...\n");
    
    // Test getpid
    pid_t pid = getpid();
    print("PID: ");
    
    // Simple number printing (since we don't have full printf yet)
    if (pid == 0) {
        print("0");
    } else {
        char buf[20];
        int i = 0;
        int temp = pid;
        
        // Convert to string
        if (temp == 0) {
            buf[i++] = '0';
        } else {
            int digits[20];
            int j = 0;
            while (temp > 0) {
                digits[j++] = temp % 10;
                temp /= 10;
            }
            for (int k = j - 1; k >= 0; k--) {
                buf[i++] = '0' + digits[k];
            }
        }
        buf[i] = '\0';
        print(buf);
    }
    print("\n");
    
    // Test time
    uint64_t current_time = time(NULL);
    print("Time: ");
    
    // Convert time to string (simplified)
    char timebuf[20];
    int ti = 0;
    if (current_time == 0) {
        timebuf[ti++] = '0';
    } else {
        int digits[20];
        int j = 0;
        uint64_t temp = current_time;
        while (temp > 0) {
            digits[j++] = temp % 10;
            temp /= 10;
        }
        for (int k = j - 1; k >= 0; k--) {
            timebuf[ti++] = '0' + digits[k];
        }
    }
    timebuf[ti] = '\0';
    print(timebuf);
    print(" seconds\n");
    
    print("Test complete!\n");
    
    // Exit with explicit status 0
    exit(0);
    
    // Should never reach here
    while(1);
}











