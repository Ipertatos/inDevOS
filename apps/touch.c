/*
 * touch - create empty files (or truncate nothing if they exist).
 * Real user program: open(O_CREAT) + close.
 */
#include "syscalls.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: touch <file>...\n");
        return 1;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_WRONLY | O_CREAT);
        if (fd < 0) {
            printf("touch: cannot create '%s'\n", argv[i]);
            rc = 1;
            continue;
        }
        close(fd);
    }
    return rc;
}
