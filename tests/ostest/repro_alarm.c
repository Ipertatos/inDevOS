/* Bug 1: alarm()/setitimer() must deliver SIGALRM and interrupt blocking
 * syscalls (waitpid, nanosleep, poll, read) with EINTR. Read test is last
 * because a broken kernel blocks there forever. */
#include "repro_common.h"
#define TAG "REPRO_ALARM"
static volatile sig_atomic_t got;
static void on_alrm(int s) { (void)s; got++; }

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alrm;            /* no SA_RESTART: syscalls get EINTR */
    sigaction(SIGALRM, &sa, NULL);
    long t0; int r, e;

    /* setitimer/getitimer round trip */
    struct itimerval it = { {0, 0}, {5, 0} }, cur;
    setitimer(ITIMER_REAL, &it, NULL);
    getitimer(ITIMER_REAL, &cur);
    CHECK(TAG, "getitimer", cur.it_value.tv_sec >= 3 && cur.it_value.tv_sec <= 5,
          "remaining=%lds", (long)cur.it_value.tv_sec);
    unsigned left = alarm(0);
    CHECK(TAG, "alarm0", left >= 3 && left <= 5, "alarm(0) returned %u", left);

    /* waitpid */
    pid_t c = fork();
    if (c == 0) { msleep(4000); _exit(0); }
    got = 0; alarm(1); t0 = now_ms();
    r = waitpid(c, NULL, 0); e = errno;
    long dt = now_ms() - t0;
    CHECK(TAG, "waitpid", r == -1 && e == EINTR && got == 1 && dt < 3000,
          "r=%d errno=%d got=%d %ldms", r, e, (int)got, dt);
    alarm(0); waitpid(c, NULL, 0);

    /* nanosleep */
    got = 0; alarm(1); t0 = now_ms();
    struct timespec ts = { 4, 0 }, rem = { 0, 0 };
    r = nanosleep(&ts, &rem); e = errno; dt = now_ms() - t0;
    CHECK(TAG, "nanosleep", r == -1 && e == EINTR && got == 1 && dt < 3000,
          "r=%d errno=%d got=%d %ldms rem=%lds", r, e, (int)got, dt, (long)rem.tv_sec);
    alarm(0);

    /* poll on an empty pipe */
    int pf[2]; pipe(pf);
    struct pollfd pd = { pf[0], POLLIN, 0 };
    got = 0; alarm(1); t0 = now_ms();
    r = poll(&pd, 1, 4000); e = errno; dt = now_ms() - t0;
    CHECK(TAG, "poll", r == -1 && e == EINTR && got == 1 && dt < 3000,
          "r=%d errno=%d got=%d %ldms", r, e, (int)got, dt);
    alarm(0);

    /* default action kills a busy-looping child */
    c = fork();
    if (c == 0) {
        signal(SIGALRM, SIG_DFL); alarm(1);
        long s = now_ms(); volatile unsigned long n = 0;
        while (now_ms() - s < 3000) n++;
        _exit(3);
    }
    int st = 0; t0 = now_ms();
    while ((r = waitpid(c, &st, 0)) < 0 && errno == EINTR) {}
    dt = now_ms() - t0;
    CHECK(TAG, "default-kill", r == c && WIFSIGNALED(st) && WTERMSIG(st) == SIGALRM,
          "status=0x%x %ldms", st, dt);

    /* pure user busy loop gets the handler (IRQ-return delivery) */
    got = 0; alarm(1); t0 = now_ms();
    { volatile unsigned long n = 0; while (!got && now_ms() - t0 < 3000) n++; }
    CHECK(TAG, "busyloop", got == 1, "got=%d %ldms", (int)got, now_ms() - t0);

    /* read on an empty pipe whose write end we still hold (hangs if broken) */
    char b;
    got = 0; alarm(1); t0 = now_ms();
    r = (int)read(pf[0], &b, 1); e = errno; dt = now_ms() - t0;
    CHECK(TAG, "read", r == -1 && e == EINTR && got == 1 && dt < 3000,
          "r=%d errno=%d got=%d %ldms", r, e, (int)got, dt);
    RESULT(TAG);
}
