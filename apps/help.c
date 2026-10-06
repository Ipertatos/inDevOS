/*
 * help - list the programs actually installed on the filesystem.
 *
 * This is a real user program: it scans "/" through the readdir syscall and
 * reports the executables it finds, instead of hard-coding a command list.
 */
#include "syscalls.h"

static int ends_with(const char *s, const char *suf) {
    size_t ls = strlen(s), lf = strlen(suf);
    if (lf > ls) return 0;
    return strcmp(s + (ls - lf), suf) == 0;
}

int main(int argc, char **argv) {
    static indevos_dirent_t ents[64];
    (void)argc; (void)argv;

    printf("Programs installed in /\n");
    printf("----------------------\n");

    int fd = open("/", O_RDONLY);
    if (fd < 0) {
        printf("help: cannot open '/'\n");
        return 1;
    }

    int shown = 0;
    for (;;) {
        int n = readdir(fd, ents, 64);
        if (n <= 0) break;
        for (int i = 0; i < n; i++) {
            if (!ends_with(ents[i].d_name, ".exe")) continue;
            printf("  %-22s %8u bytes\n", ents[i].d_name, ents[i].d_size);
            shown++;
        }
        if (n < 64) break;
    }
    close(fd);

    printf("\n%d program(s) on disk - run one by typing its name\n", shown);
    printf("Shell built-ins (in the kernel): cd, pwd, exit, exec, spawn,\n");
    printf("  mount/unmount, fsinfo, net, dhcp, usb, ps, kill, prof, desktop\n");
    return 0;
}
