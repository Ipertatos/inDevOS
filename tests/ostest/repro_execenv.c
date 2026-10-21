/* repro_execenv: execve ignored envp (sys_exec took only argv), so the new
 * image saw the built-in default environment (ostest proc.argv_env). */
#include "repro_common.h"
extern char **environ;
int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--child")) {
        const char *v = getenv("RP_ENV"); int n = 0; for (char **e = environ; *e; e++) n++;
        printf("child env=%s n=%d path=%s\n", v ? v : "(null)", n, getenv("PATH") ? getenv("PATH") : "(null)");
        return (v && !strcmp(v, "hello world") && n == 2) ? 7 : 3;
    }
    for (int round = 0; round < 3; round++) {
        int pf[2]; pipe(pf); pid_t c = fork();
        if (c == 0) {
            dup2(pf[1], 1); close(pf[0]);
            char *av[] = { "/bin/reproexecenv.exe", "--child", NULL };
            char *ev[] = { "RP_ENV=hello world", "PATH=/bin", NULL };
            execve(av[0], av, ev); _exit(127);
        }
        close(pf[1]); char out[256] = ""; int o = 0, r;
        while (o < 255 && (r = read(pf[0], out + o, 255 - o)) > 0) o += r;
        close(pf[0]); int st = 0; waitpid(c, &st, 0);
        out[strcspn(out, "\n")] = 0;
        CHECK("execenv", "envp", WIFEXITED(st) && WEXITSTATUS(st) == 7, "round %d status 0x%x '%s'", round, st, out);
    }
    RESULT("execenv");
}
