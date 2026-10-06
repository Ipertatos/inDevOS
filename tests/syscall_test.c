#include "include/syscalls.h"

int main() {
    // Test basic syscalls
    print("Hello from user program using syscalls!\n");
    
    // Test process info
    pid_t pid = getpid();
    print("Process ID: ");
    // Simple integer to string conversion
    char buf[20];
    int i = 0;
    int temp = pid;
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
    print("\n");
    
    // Test time
    uint64_t current_time = time(NULL);
    print("Time since boot: ");
    // Convert time to string (simplified)
    i = 0;
    temp = current_time;
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
    print(" seconds\n");
    
    // Test file operations if filesystem is mounted
    print("Testing file operations...\n");
    int fd = open("syscall_test.txt", O_CREAT | O_WRONLY);
    if (fd >= 0) {
        print("Created test file successfully\n");
        write(fd, "Hello from syscall test!", 24);
        close(fd);
        
        // Try to read it back
        fd = open("syscall_test.txt", O_RDONLY);
        if (fd >= 0) {
            char read_buf[256];
            int bytes_read = read(fd, read_buf, sizeof(read_buf) - 1);
            if (bytes_read > 0) {
                read_buf[bytes_read] = '\0';
                print("Read back: ");
                print(read_buf);
                print("\n");
            }
            close(fd);
        }
    } else {
        print("Could not create test file (filesystem not mounted?)\n");
    }
    
    // Test sleep
    print("Sleeping for 2 seconds...\n");
    sleep(2);
    print("Awake!\n");
    
    print("Syscall test completed successfully!\n");
    
    // Exit
    exit(0);
    return 0;  // Should never reach here
}











