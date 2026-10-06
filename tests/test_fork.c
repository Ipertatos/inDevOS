#include "include/syscalls.h"

void _start() {
    print("Testing fork() and wait()...\n");
    
    pid_t pid = fork();
    
    if (pid == 0) {
        // This would be the child process in a real OS
        print("This should be child process (but fork doesn't actually create new process yet)\n");
        exit(42);
    } else if (pid > 0) {
        // Parent process
        print("Parent: fork() returned PID ");
        
        // Convert PID to string
        char pid_str[10];
        int i = 0;
        int temp = pid;
        if (temp == 0) {
            pid_str[i++] = '0';
        } else {
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
        
        print("Parent: calling wait()...\n");
        int status;
        pid_t waited_pid = wait(&status);
        
        if (waited_pid > 0) {
            print("Parent: wait() returned PID ");
            // Convert waited_pid to string (reuse logic)
            i = 0;
            temp = waited_pid;
            if (temp == 0) {
                pid_str[i++] = '0';
            } else {
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
            print(" with status ");
            
            // Convert status to string
            i = 0;
            temp = status;
            if (temp == 0) {
                pid_str[i++] = '0';
            } else {
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
        } else {
            print("Parent: wait() failed\n");
        }
    } else {
        print("fork() failed\n");
    }
    
    print("Fork test completed!\n");
    exit(0);
}










