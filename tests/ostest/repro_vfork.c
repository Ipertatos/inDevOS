/* vfork: _exit status, exec, and many in a row (proc.vfork #DF RIP=0). */
#include "repro_common.h"
#define TAG "REPRO_VFORK"
extern char **environ;
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("%s start\n", TAG);
    int st = -1;
    pid_t c = vfork();
    if (c == 0) _exit(3);
    pid_t w = waitpid(c, &st, 0);
    CHECK(TAG, "exit", c > 0 && w == c && WIFEXITED(st) && WEXITSTATUS(st) == 3, "pid=%d w=%d status=0x%x", c, w, st);

    if (access("/bin/busybox.exe", X_OK) == 0) {
        char *av[] = { "/bin/busybox.exe", "sh", "-c", "exit 7", NULL };
        c = vfork();
        if (c == 0) { execve(av[0], av, environ); _exit(127); }
        st = -1; w = waitpid(c, &st, 0);
        CHECK(TAG, "exec", w == c && WIFEXITED(st) && WEXITSTATUS(st) == 7, "status=0x%x", st);
    } else {
        printf("%s exec SKIP no /bin/busybox.exe\n", TAG);
    }

    int ok = 0;
    for (int i = 0; i < 50; i++) {
        c = vfork();
        if (c == 0) _exit(i);
        st = -1;
        if (c > 0 && waitpid(c, &st, 0) == c && WIFEXITED(st) && WEXITSTATUS(st) == i) ok++;
    }
    CHECK(TAG, "loop50", ok == 50, "%d/50 right status", ok);

    /* vfork child shares memory until _exit: parent sees its write */
    volatile int shared = 0;
    c = vfork();
    if (c == 0) { shared = 11; _exit(0); }
    waitpid(c, NULL, 0);
    CHECK(TAG, "shared-mem", shared == 11, "parent sees %d", shared);
    RESULT(TAG);
}
