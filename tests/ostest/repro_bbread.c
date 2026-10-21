/* repro_bbread: `busybox find /bin > f; busybox wc -l < f` printed nothing
 * (ostest fs.readback_busybox).  Runs variants to isolate shell redirection,
 * stdin-from-file and the file contents themselves. */
#include "repro_common.h"
static int run(const char *cmd, char *out, int cap) {
    int pf[2]; pipe(pf); pid_t c = fork();
    if (c == 0) { dup2(pf[1], 1); dup2(pf[1], 2); close(pf[0]); close(pf[1]);
        char *av[] = { "/bin/busybox.exe", "sh", "-c", (char *)cmd, NULL }; execv(av[0], av); _exit(127); }
    close(pf[1]); int o = 0, r; long t0 = now_ms();
    while (o < cap - 1 && (r = read(pf[0], out + o, cap - 1 - o)) > 0) o += r;
    out[o] = 0; close(pf[0]); int st = 0; waitpid(c, &st, 0);
    for (char *q = out; *q; q++) if (*q == '\n') *q = '/';
    (void)t0; return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}
int main(void) {
    char o[200]; int r;
    r = run("busybox find /bin > /tmp/rb.txt; busybox wc -l < /tmp/rb.txt", o, sizeof o);
    CHECK("bbread", "find_wc_redir", r == 0 && atoi(o) > 0, "exit %d out '%.80s'", r, o);
    r = run("busybox ls -l /tmp/rb.txt", o, sizeof o);
    CHECK("bbread", "ls_size", r == 0, "exit %d out '%.100s'", r, o);
    r = run("busybox wc -l /tmp/rb.txt", o, sizeof o);
    CHECK("bbread", "wc_path", r == 0 && atoi(o) > 0, "exit %d out '%.80s'", r, o);
    r = run("busybox wc -l < /tmp/rb.txt", o, sizeof o);
    CHECK("bbread", "wc_stdin_file", r == 0 && atoi(o) > 0, "exit %d out '%.80s'", r, o);
    r = run("echo hello > /tmp/rb2.txt; busybox cat < /tmp/rb2.txt", o, sizeof o);
    CHECK("bbread", "cat_stdin_file", r == 0 && !strncmp(o, "hello", 5), "exit %d out '%.80s'", r, o);
    r = run("busybox find /bin | busybox wc -l", o, sizeof o);
    CHECK("bbread", "find_pipe_wc", r == 0 && atoi(o) > 0, "exit %d out '%.80s'", r, o);
    r = run("busybox wc -l < /tmp/rb.txt > /tmp/rb3.txt; busybox cat /tmp/rb3.txt", o, sizeof o);
    CHECK("bbread", "wc_to_file", r == 0 && atoi(o) > 0, "exit %d out '%.80s'", r, o);
    /* no shell: stdin = file fd, exec wc */
    int pf[2]; pipe(pf); pid_t c = fork();
    if (c == 0) { int fd = open("/tmp/rb.txt", O_RDONLY); dup2(fd, 0); close(fd); dup2(pf[1], 1); close(pf[0]);
        char *av[] = { "/bin/busybox.exe", "wc", "-l", NULL }; execv(av[0], av); _exit(127); }
    close(pf[1]); int n = read(pf[0], o, sizeof o - 1); o[n > 0 ? n : 0] = 0; close(pf[0]); int st; waitpid(c, &st, 0);
    CHECK("bbread", "exec_stdin_file", atoi(o) > 0, "status 0x%x out '%.60s'", st, o);
    unlink("/tmp/rb.txt"); unlink("/tmp/rb2.txt"); unlink("/tmp/rb3.txt");
    RESULT("bbread");
}
