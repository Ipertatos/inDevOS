#include "include/syscalls.h"

// Signal handler function
void my_signal_handler(int signum) {
    print("Received signal ");
    
    // Convert signal number to string
    char sig_str[10];
    int i = 0;
    int temp = signum;
    if (temp == 0) {
        sig_str[i++] = '0';
    } else {
        int digits[10];
        int j = 0;
        while (temp > 0) {
            digits[j++] = temp % 10;
            temp /= 10;
        }
        for (int k = j - 1; k >= 0; k--) {
            sig_str[i++] = '0' + digits[k];
        }
    }
    sig_str[i] = '\0';
    
    print(sig_str);
    print(" in handler!\n");
}

void _start() {
    print("Testing signal handling...\n");
    
    // Install signal handler for SIGUSR1
    print("Installing signal handler for SIGUSR1...\n");
    sighandler_t old_handler = signal(SIGUSR1, my_signal_handler);
    
    if (old_handler == SIG_DFL) {
        print("Signal handler installed successfully\n");
    }
    
    // Get our PID
    pid_t pid = getpid();
    
    print("Sending SIGUSR1 to self...\n");
    kill(pid, SIGUSR1);
    
    print("Sending SIGTERM to self...\n");
    kill(pid, SIGTERM);
    
    print("This should not be printed if SIGTERM worked\n");
    exit(0);
}










