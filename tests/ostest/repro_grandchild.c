/* Bug 2: a child must be scheduled while its parent blocks reading a pipe,
 * including a grandchild created by a fork child. */
#include "repro_common.h"
#define TAG "REPRO_GRANDCHILD"

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    long t0 = now_ms();
    /* 1: parent reads a pipe its own child writes */
    int p[2]; pipe(p);
    pid_t c = fork();
    if (c == 0) { close(p[0]); write(p[1], "x", 1); _exit(0); }
    close(p[1]);
    char b = 0; int r = (int)read(p[0], &b, 1);
    CHECK(TAG, "child-pipe", r == 1 && b == 'x', "r=%d b=%c %ldms", r, b ? b : '?', now_ms() - t0);
    close(p[0]); waitpid(c, NULL, 0);

    /* 2: fork child forks a grandchild and blocks reading its pipe */
    t0 = now_ms();
    c = fork();
    if (c == 0) {
        int q[2]; pipe(q);
        pid_t g = fork();
        if (g == 0) { close(q[0]); write(q[1], "gc", 2); _exit(0); }
        close(q[1]);
        char buf[4] = {0}; int n = (int)read(q[0], buf, 2);
        waitpid(g, NULL, 0);
        _exit(n == 2 && memcmp(buf, "gc", 2) == 0 ? 0 : 1);
    }
    int st = -1; r = waitpid(c, &st, 0);
    CHECK(TAG, "grandchild-pipe", r == c && WIFEXITED(st) && WEXITSTATUS(st) == 0,
          "status=0x%x %ldms", st, now_ms() - t0);
    RESULT(TAG);
}
