#include "include/syscalls.h"

void _start() {
    // Test syscalls using the write() syscall instead of print()
    // write() to stdout should work since it goes through the kernel
    
    char msg1[] = "Hello from write!\n";
    char msg2[] = "PID: ";
    char msg3[] = "\nDone!\n";
    
    write(STDOUT_FILENO, msg1, 18);
    write(STDOUT_FILENO, msg2, 5);
    
    // Get PID and print it
    pid_t pid = getpid();
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
    
    write(STDOUT_FILENO, pid_str, i);
    write(STDOUT_FILENO, msg3, 7);
    
    exit(123);  // Exit with distinctive status
}











