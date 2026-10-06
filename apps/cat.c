/*
 * cat - write file contents to stdout.
 * Real user program: open/read/write only (was kernel fat32_* calls before).
 */
#include "syscalls.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: cat <file> [file...]\n");
        return 1;
    }

    /* 32 KB per read: a read() syscall costs ~13 ms here regardless of size,
     * so small buffers dominate the time for large files. */
    static char buf[32768];
    int rc = 0;

    for (int a = 1; a < argc; a++) {
        int fd = open(argv[a], O_RDONLY);
        if (fd < 0) {
            printf("cat: %s: cannot open\n", argv[a]);
            rc = 1;
            continue;
        }
        for (;;) {
            int n = read(fd, buf, sizeof(buf));
            if (n <= 0) break;
            int off = 0;
            while (off < n) {
                int w = write(STDOUT_FILENO, buf + off, (size_t)(n - off));
                if (w <= 0) { close(fd); return 1; }
                off += w;
            }
        }
        close(fd);
    }
    return rc;
}
