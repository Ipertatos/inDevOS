/*
 * neofetch - a small system summary, as a real user program.
 * Everything here comes from syscalls (uid/gid, cwd, display, time, readdir).
 */
#include "syscalls.h"

int main(int argc, char **argv) {
    char cwd[256];
    static indevos_dirent_t ents[64];
    (void)argc; (void)argv;

    printf("        /\\        inDevOS\n");
    printf("       /  \\       --------\n");
    printf("      /    \\      user : uid=%u gid=%u\n",
           (unsigned)getuid(), (unsigned)getgid());

    if (getcwd(cwd, sizeof(cwd)))
        printf("     /  /\\  \\     cwd  : %s\n", cwd);

    uint64_t disp = indevos_get_display_size();
    printf("    /  /  \\  \\    video: %ux%u\n",
           (unsigned)(disp >> 32), (unsigned)(disp & 0xffffffffu));

    uint64_t ms = time((uint64_t *)0);
    printf("   /__/    \\__\\   up   : %u.%03u s\n",
           (unsigned)(ms / 1000), (unsigned)(ms % 1000));
    printf("                  pid  : %u\n", (unsigned)getpid());

    int fd = open("/", O_RDONLY);
    int files = 0, dirs = 0;
    if (fd >= 0) {
        for (;;) {
            int n = readdir(fd, ents, 64);
            if (n <= 0) break;
            for (int i = 0; i < n; i++) {
                if (ents[i].d_name[0] == '.' && ents[i].d_name[1] == '\0') continue;
                if (ents[i].d_type == INDEVOS_DT_DIR) dirs++;
                else files++;
            }
            if (n < 64) break;
        }
        close(fd);
    }
    printf("                  root : %d files, %d dirs\n", files, dirs);
    return 0;
}
