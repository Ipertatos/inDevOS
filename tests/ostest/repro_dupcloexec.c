/* repro_dupcloexec: dup2()/dup() copied FD_CLOEXEC to the new descriptor, so
 * a shell that saved stdout with F_DUPFD_CLOEXEC and restored it with dup2
 * lost stdout at the next exec (busybox `echo x > f; cat < f` -> EBADF). */
#include "repro_common.h"
int main(void) {
    int s = fcntl(1, F_DUPFD_CLOEXEC, 10);
    CHECK("dupcloexec", "dupfd_cloexec", s >= 10 && fcntl(s, F_GETFD) == FD_CLOEXEC, "fd=%d flags=%d", s, fcntl(s, F_GETFD));
    int r = dup2(s, 20);
    CHECK("dupcloexec", "dup2_clears", r == 20 && fcntl(20, F_GETFD) == 0, "dup2=%d flags=%d", r, fcntl(20, F_GETFD));
    int d = dup(s);
    CHECK("dupcloexec", "dup_clears", d >= 0 && fcntl(d, F_GETFD) == 0, "dup=%d flags=%d", d, fcntl(d, F_GETFD));
    /* exec keeps a dup2-restored stdout */
    int pf[2]; pipe(pf); pid_t c = fork();
    if (c == 0) {
        dup2(pf[1], 1); close(pf[0]); close(pf[1]);
        int sv = fcntl(1, F_DUPFD_CLOEXEC, 10); int nul = open("/dev/null", O_WRONLY);
        dup2(nul, 1); close(nul); dup2(sv, 1); close(sv);
        char *av[] = { "/bin/busybox.exe", "echo", "survived", NULL }; execv(av[0], av); _exit(127);
    }
    close(pf[1]); char o[64] = ""; int n = read(pf[0], o, 63); o[n > 0 ? n : 0] = 0; close(pf[0]); waitpid(c, NULL, 0);
    CHECK("dupcloexec", "exec_stdout", !strncmp(o, "survived", 8), "child out '%.30s'", o);
    RESULT("dupcloexec");
}
