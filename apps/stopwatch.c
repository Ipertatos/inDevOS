/*
 * stopwatch - count elapsed time until a key is pressed.
 *
 * Real user program: uses SYS_TIME (milliseconds) and SYS_KB_READ; formerly the
 * in-kernel stopwatch_test() in src/cmd.c.
 */
#include "syscalls.h"

static void print_elapsed(uint32_t ms) {
    uint32_t secs  = ms / 1000u;
    uint32_t hunds = (ms % 1000u) / 10u;
    printf("\r%02u:%02u.%02u   ",
           secs / 60u, secs % 60u, hunds);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    printf("stopwatch: press any key to stop\n");

    /* Drop the Enter that launched us so the timer does not stop instantly. */
    kb_clear();

    uint32_t start = (uint32_t)syscall(SYS_TIME, 0, 0, 0, 0, 0);
    uint32_t last_draw = 0;

    for (;;) {
        char kbuf[8];
        if (kb_read(kbuf, sizeof(kbuf)) > 0 || terminate_requested()) break;

        uint32_t elapsed = (uint32_t)syscall(SYS_TIME, 0, 0, 0, 0, 0) - start;
        if (elapsed - last_draw >= 10u) {
            print_elapsed(elapsed);
            last_draw = elapsed;
        }

        /* 10 ms pacing; see ktime_yield_until() for why this is the safe
         * primitive for a program running on a kernel thread. */
        timespec_ind_t ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 10 * 1000000L;
        nanosleep(&ts, 0);
    }

    uint32_t elapsed = (uint32_t)syscall(SYS_TIME, 0, 0, 0, 0, 0) - start;
    printf("\nstopwatch: %u.%02u s\n", elapsed / 1000u, (elapsed % 1000u) / 10u);
    return 0;
}
