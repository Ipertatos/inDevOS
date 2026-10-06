/*
 * rm - remove files and empty directories.
 * Real user program: unlink, falling back to rmdir for directories.
 */
#include "syscalls.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: rm <path>...\n");
        return 1;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (unlink(argv[i]) == 0) continue;
        if (rmdir(argv[i]) == 0) continue;
        printf("rm: cannot remove '%s'\n", argv[i]);
        rc = 1;
    }
    return rc;
}
