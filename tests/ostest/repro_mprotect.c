/* PROT_READ pages must fault on write, also in a fork child. */
#include "repro_common.h"
#include <sys/mman.h>
#define TAG "REPRO_MPROTECT"
static int child_write(volatile char *p) {
    pid_t c = fork();
    if (c == 0) { p[0] = 1; _exit(0); }
    int st = 0; waitpid(c, &st, 0); return st;
}
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    char *a = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    a[0] = 5; mprotect(a, 4096, PROT_READ);
    int st = child_write(a);
    CHECK(TAG, "mprotect-ro", WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV, "status 0x%x", st);
    char *b = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    st = child_write(b);
    CHECK(TAG, "mmap-ro", WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV, "status 0x%x", st);
    mprotect(a, 4096, PROT_READ | PROT_WRITE); a[0] = 9;
    CHECK(TAG, "rw-again", a[0] == 9, "a0=%d", a[0]);
    pid_t c = fork(); if (c == 0) { a[0] = 3; _exit(a[0]); }
    waitpid(c, &st, 0);
    CHECK(TAG, "cow-after-rw", WEXITSTATUS(st) == 3 && a[0] == 9, "child %d parent %d", WEXITSTATUS(st), a[0]);
    int *s = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    *s = 1; c = fork(); if (c == 0) { *s = 42; _exit(0); } waitpid(c, NULL, 0);
    CHECK(TAG, "shared-anon", *s == 42, "parent sees %d", *s);
    RESULT(TAG);
}
