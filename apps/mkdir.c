/*
 * mkdir - create directories.
 * Real user program: uses the mkdir syscall.
 */
#include "syscalls.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: mkdir <dir>...\n");
        return 1;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (mkdir(argv[i]) != 0)
            printf("mkdir: cannot create '%s'\n", argv[i]), rc = 1;
    }
    return rc;
}
