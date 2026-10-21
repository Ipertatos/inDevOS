/* Bug 3: nanosleep must block/yield so newly forked children run while the
 * parent polls with sleeps (no blocking wait). */
#include "repro_common.h"
#define TAG "REPRO_NANOSLEEP"

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    /* 1: WNOHANG + nanosleep polling must see the child exit */
    long t0 = now_ms();
    pid_t c = fork();
    if (c == 0) _exit(7);
    int st = 0, r = 0, i;
    for (i = 0; i < 200; i++) {
        r = waitpid(c, &st, WNOHANG);
        if (r == c) break;
        struct timespec ts = { 0, 10 * 1000000L }; nanosleep(&ts, NULL);
    }
    CHECK(TAG, "wnohang-poll", r == c && WIFEXITED(st) && WEXITSTATUS(st) == 7,
          "r=%d iters=%d %ldms", r, i, now_ms() - t0);
    if (r != c) { kill(c, SIGKILL); waitpid(c, NULL, 0); }

    /* 2: child writes a pipe; parent checks it with poll(0) + usleep */
    t0 = now_ms();
    int p[2]; pipe(p);
    c = fork();
    if (c == 0) { write(p[1], "y", 1); _exit(0); }
    struct pollfd pd = { p[0], POLLIN, 0 };
    for (i = 0; i < 200; i++) {
        if (poll(&pd, 1, 0) == 1) break;
        usleep(10000);
    }
    CHECK(TAG, "pipe-usleep", i < 200, "iters=%d %ldms", i, now_ms() - t0);
    waitpid(c, NULL, 0);

    /* 3: clock_nanosleep should also let the child run */
    t0 = now_ms();
    c = fork();
    if (c == 0) _exit(5);
    for (i = 0; i < 200; i++) {
        r = waitpid(c, &st, WNOHANG);
        if (r == c) break;
        struct timespec ts = { 0, 10 * 1000000L };
        clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);
    }
    CHECK(TAG, "clock-nanosleep", r == c, "r=%d iters=%d %ldms", r, i, now_ms() - t0);
    if (r != c) { kill(c, SIGKILL); waitpid(c, NULL, 0); }

    /* 4: sleep duration sanity (200 ms really takes ~200 ms) */
    t0 = now_ms(); msleep(200); long dt = now_ms() - t0;
    CHECK(TAG, "duration", dt >= 190 && dt < 1000, "slept %ldms", dt);
    RESULT(TAG);
}
