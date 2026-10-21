/* Fork children that exit without reaping their own children must not leak
 * process slots: after many rounds, 64 concurrent forks still succeed. */
#include "repro_common.h"
#define TAG "REPRO_ORPHANS"
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int bad = 0;
    for (int r = 0; r < 8; r++) {              /* 8 x 10 leaked grandchildren */
        pid_t c = fork();
        if (c == 0) {
            for (int i = 0; i < 10; i++) if (fork() == 0) _exit(0);
            msleep(50);
            _exit(0);                          /* exits without wait() */
        }
        if (c < 0) bad++; else waitpid(c, NULL, 0);
    }
    CHECK(TAG, "rounds", bad == 0, "fork failures %d", bad);
    pid_t p[64]; int ok = 0, n = 0;
    for (int i = 0; i < 64; i++) { p[i] = fork(); if (p[i] == 0) { msleep(30); _exit(i); } if (p[i] > 0) n++; }
    for (int i = 0; i < 64; i++) { int st; if (p[i] > 0 && waitpid(p[i], &st, 0) == p[i] && WEXITSTATUS(st) == i) ok++; }
    CHECK(TAG, "fork64", n == 64 && ok == 64, "forked %d, right status %d", n, ok);
    RESULT(TAG);
}
