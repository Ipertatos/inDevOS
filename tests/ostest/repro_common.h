#define _GNU_SOURCE
/* Shared helpers for the inDevOS kernel repro programs (static musl). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/time.h>

static long now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}
static __attribute__((unused)) void msleep(long ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {}
}
static int g_fail;
#define CHECK(tag, sub, cond, ...) do { int ok_ = (cond); if (!ok_) g_fail++; \
    printf("%s %s %s ", tag, sub, ok_ ? "PASS" : "FAIL"); printf(__VA_ARGS__); \
    printf("\n"); fflush(stdout); } while (0)
#define RESULT(tag) do { printf("%s RESULT %s (%d failed)\n", tag, g_fail ? "FAIL" : "PASS", g_fail); \
    fflush(stdout); return g_fail ? 1 : 0; } while (0)
