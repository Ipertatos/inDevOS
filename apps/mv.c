/*
 * mv - move/rename a file.  FAT32 has no rename operation here, so this is a
 * copy followed by an unlink of the source; the source is only removed after
 * the copy has completed successfully.
 */
#include "syscalls.h"

static char buf[4096];

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("usage: mv <source> <dest>\n");
        return 1;
    }

    const char *src = argv[1];
    const char *dst = argv[2];

    if (strcmp(src, dst) == 0) {
        printf("mv: '%s' and '%s' are the same file\n", src, dst);
        return 1;
    }

    int in = open(src, O_RDONLY);
    if (in < 0) {
        printf("mv: %s: cannot open\n", src);
        return 1;
    }

    int out = open3(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        printf("mv: %s: cannot create\n", dst);
        close(in);
        return 1;
    }

    long total = 0;
    for (;;) {
        int n = read(in, buf, sizeof(buf));
        if (n < 0) {
            printf("mv: %s: read error\n", src);
            close(in); close(out);
            return 1;
        }
        if (n == 0) break;

        int off = 0;
        while (off < n) {
            int w = write(out, buf + off, (size_t)(n - off));
            if (w <= 0) {
                printf("mv: %s: write error\n", dst);
                close(in); close(out);
                return 1;
            }
            off += w;
        }
        total += n;
    }

    close(in);
    close(out);

    if (unlink(src) != 0) {
        printf("mv: copied to %s but could not remove %s\n", dst, src);
        return 1;
    }

    printf("mv: %s -> %s (%ld bytes)\n", src, dst, total);
    return 0;
}
