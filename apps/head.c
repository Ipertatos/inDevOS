/*
 * head - print the first N lines of a file (default 10).
 * Real user program: open/read only.
 */
#include "syscalls.h"

int main(int argc, char **argv) {
    int want = 10;
    const char *path = (char *)0;

    if (argc >= 2 && argv[1][0] == '-') {
        want = atoi(argv[1] + 1);
        if (argc >= 3) path = argv[2];
    } else if (argc >= 2) {
        path = argv[1];
        if (argc >= 3) want = atoi(argv[2]);
    }

    if (!path) {
        printf("usage: head [-N] <file> [N]\n");
        return 1;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("head: %s: cannot open\n", path);
        return 1;
    }

    static char buf[512];
    int lines = 0;
    while (lines < want) {
        int n = read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        int stop = n;
        for (int i = 0; i < n; i++) {
            if (buf[i] == '\n' && ++lines >= want) { stop = i + 1; break; }
        }
        int off = 0;
        while (off < stop) {
            int w = write(STDOUT_FILENO, buf + off, (size_t)(stop - off));
            if (w <= 0) { close(fd); return 1; }
            off += w;
        }
    }
    close(fd);
    return 0;
}
