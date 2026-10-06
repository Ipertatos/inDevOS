#include "include/syscalls.h"

void _start() {
    // Test the fixed print() function
    print("Hello from print()!\n");
    print("Testing user-space strings...\n");
    print("If you can see this, the fix worked!\n");
    
    // Test other functions too
    print("PID: ");
    pid_t pid = getpid();
    
    // Simple number to string conversion
    char pid_str[10];
    int i = 0;
    if (pid == 0) {
        pid_str[i++] = '0';
    } else {
        int temp = pid;
        int digits[10];
        int j = 0;
        while (temp > 0) {
            digits[j++] = temp % 10;
            temp /= 10;
        }
        for (int k = j - 1; k >= 0; k--) {
            pid_str[i++] = '0' + digits[k];
        }
    }
    pid_str[i] = '\0';
    
    print(pid_str);
    print("\n");
    
    print("All tests completed!\n");
    exit(42);
}










