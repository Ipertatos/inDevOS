/*
 * wc - count lines, words and bytes.
 * Real user program: open/read only.
 */
#include "syscalls.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: wc <file>...\n");
        return 1;
    }

    /* 32 KB per read: syscall overhead dominates for large files (a 4 MB WAD
     * took 55 s with 1 KB reads, because every read() call costs ~13 ms in
     * this kernel regardless of size). */
    static char buf[32768];
    int rc = 0;

    for (int a = 1; a < argc; a++) {
        int fd = open(argv[a], O_RDONLY);
        if (fd < 0) {
            printf("wc: %s: cannot open\n", argv[a]);
            rc = 1;
            continue;
        }
        unsigned long bytes = 0, lines = 0, words = 0;
        int in_word = 0;
        for (;;) {
            int n = read(fd, buf, sizeof(buf));
            if (n <= 0) break;
            bytes += (unsigned long)n;
            for (int i = 0; i < n; i++) {
                char c = buf[i];
                if (c == '\n') lines++;
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                    in_word = 0;
                } else if (!in_word) {
                    in_word = 1;
                    words++;
                }
            }
        }
        close(fd);
        printf("%8lu %8lu %8lu  %s\n", lines, words, bytes, argv[a]);
    }
    return rc;
}
