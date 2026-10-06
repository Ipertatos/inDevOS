/*
 * cp - copy a file.  Real user program: open/read/write only, so it also
 * exercises the FAT32 create + data-write path from Ring 3.
 */
#include "syscalls.h"

static char buf[4096];

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("usage: cp <source> <dest>\n");
        return 1;
    }

    const char *src = argv[1];
    const char *dst = argv[2];

    int in = open(src, O_RDONLY);
    if (in < 0) {
        printf("cp: %s: cannot open\n", src);
        return 1;
    }

    int out = open3(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        printf("cp: %s: cannot create\n", dst);
        close(in);
        return 1;
    }

    long total = 0;
    for (;;) {
        int n = read(in, buf, sizeof(buf));
        if (n < 0) {
            printf("cp: %s: read error\n", src);
            close(in); close(out);
            return 1;
        }
        if (n == 0) break;

        int off = 0;
        while (off < n) {
            int w = write(out, buf + off, (size_t)(n - off));
            if (w <= 0) {
                printf("cp: %s: write error\n", dst);
                close(in); close(out);
                return 1;
            }
            off += w;
        }
        total += n;
    }

    close(in);
    close(out);
    printf("cp: %s -> %s (%ld bytes)\n", src, dst, total);
    return 0;
}
