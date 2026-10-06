/*
 * grep - print lines of a file that contain a pattern.
 *
 * Real user program: open/read only.  Formerly the in-kernel zeus_cmd_grep().
 *   grep <pattern> <file> [file...]
 */
#include "syscalls.h"

/* 32 KB per read: syscall overhead (~13 ms per read here) dominates when
 * scanning large files. */
static char buf[32768];
static char line[1024];

/** Case-sensitive substring search; returns 1 when `needle` occurs in `hay`. */
static int contains(const char *hay, size_t hlen, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0) return 1;
    if (nlen > hlen) return 0;
    for (size_t i = 0; i + nlen <= hlen; i++) {
        size_t j = 0;
        while (j < nlen && hay[i + j] == needle[j]) j++;
        if (j == nlen) return 1;
    }
    return 0;
}

static int grep_file(const char *path, const char *pattern, int show_name) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("grep: %s: cannot open\n", path);
        return 0;   /* not a match, but not fatal either */
    }

    size_t linelen = 0;
    int matches = 0;
    int eof = 0;

    while (!eof) {
        int n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            printf("grep: %s: read error\n", path);
            break;
        }
        if (n == 0) eof = 1;

        for (int i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n' || linelen >= sizeof(line) - 1) {
                line[linelen] = '\0';
                if (linelen > 0 && contains(line, linelen, pattern)) {
                    if (show_name) printf("%s:", path);
                    printf("%s\n", line);
                    matches++;
                }
                linelen = 0;
            } else {
                line[linelen++] = c;
            }
        }
    }

    /* Last line without a trailing newline */
    if (linelen > 0) {
        line[linelen] = '\0';
        if (contains(line, linelen, pattern)) {
            if (show_name) printf("%s:", path);
            printf("%s\n", line);
            matches++;
        }
    }

    close(fd);
    return matches;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("usage: grep <pattern> <file> [file...]\n");
        return 1;
    }

    const char *pattern = argv[1];
    int files = argc - 2;
    int total = 0;

    for (int a = 2; a < argc; a++)
        total += grep_file(argv[a], pattern, files > 1);

    return total > 0 ? 0 : 1;   /* exit 1 when nothing matched, like grep */
}
