/*
 * ls - list directory contents.
 *
 * Real user program: uses only open/readdir/close, no kernel internals.
 */
#include "syscalls.h"

int main(int argc, char **argv) {
    /* Default to the current working directory; the VFS does not resolve ".",
     * so ask for the real path instead. */
    char cwd[256];
    const char *path = (argc > 1) ? argv[1] : (getcwd(cwd, sizeof(cwd)) ? cwd : "/");
    static indevos_dirent_t ents[64];

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("ls: cannot open '%s'\n", path);
        return 1;
    }

    printf("Type  %10s  Name\n", "Size");
    printf("----  ----------  ----\n");

    int total = 0;
    for (;;) {
        int n = readdir(fd, ents, 64);
        if (n < 0) {
            printf("ls: '%s' is not a directory\n", path);
            close(fd);
            return 1;
        }
        if (n == 0) break;

        for (int i = 0; i < n; i++) {
            const char *nm = ents[i].d_name;
            /* skip "." and ".." - every real filesystem reports them */
            if (nm[0] == '.' &&
                (nm[1] == '\0' || (nm[1] == '.' && nm[2] == '\0')))
                continue;
            int is_dir = (ents[i].d_type == INDEVOS_DT_DIR);
            printf("%-4s  %10u  %s%s\n",
                   is_dir ? "DIR" : "FILE",
                   ents[i].d_size, nm,
                   is_dir ? "/" : "");
            total++;
        }
        if (n < 64) break;
    }

    close(fd);
    printf("\n%d entr%s\n", total, total == 1 ? "y" : "ies");
    return 0;
}
