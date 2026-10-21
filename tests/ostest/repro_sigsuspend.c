/* repro_sigsuspend: rt_sigsuspend restored the old mask before the return
 * path delivered the signal, so the handler never ran (ostest sig.sigprocmask).
 * Expect: handler runs inside sigsuspend, EINTR, old mask back afterwards. */
#include "repro_common.h"
static volatile sig_atomic_t got; static sigset_t in_handler_mask;
static void h(int s) { got = s; sigprocmask(SIG_BLOCK, NULL, &in_handler_mask); }
int main(void) {
    struct sigaction sa = {0}; sa.sa_handler = h; sigaction(SIGUSR2, &sa, NULL);
    sigset_t blk, empty, cur; sigemptyset(&blk); sigaddset(&blk, SIGUSR2); sigemptyset(&empty);
    sigprocmask(SIG_BLOCK, &blk, NULL);
    for (int round = 0; round < 3; round++) {
        got = 0; pid_t parent = getpid(); pid_t c = fork();
        if (c == 0) { msleep(50); kill(parent, SIGUSR2); _exit(0); }
        long t0 = now_ms(); int r = sigsuspend(&empty); int e = errno; long dt = now_ms() - t0;
        sigprocmask(SIG_BLOCK, NULL, &cur);
        CHECK("sigsuspend", "handler", got == SIGUSR2 && r == -1 && e == EINTR,
              "round %d r=%d errno=%d got=%d after %ldms", round, r, e, (int)got, dt);
        CHECK("sigsuspend", "mask", sigismember(&cur, SIGUSR2) == 1, "round %d SIGUSR2 blocked again after return", round);
        waitpid(c, NULL, 0);
    }
    /* pending before sigsuspend: must run immediately */
    got = 0; raise(SIGUSR2); CHECK("sigsuspend", "deferred", got == 0, "blocked signal not run yet");
    int r = sigsuspend(&empty); sigprocmask(SIG_BLOCK, NULL, &cur);
    CHECK("sigsuspend", "pending", r == -1 && got == SIGUSR2 && sigismember(&cur, SIGUSR2) == 1, "r=%d got=%d", r, (int)got);
    RESULT("sigsuspend");
}
