/* Bug 4: O_NONBLOCK on pipes (pipe2 flag and fcntl F_SETFL). */
#include "repro_common.h"
#define TAG "REPRO_NONBLOCK"

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    char b; int r, e;
    int p[2];
    /* fcntl path first */
    pipe(p);
    fcntl(p[0], F_SETFL, fcntl(p[0], F_GETFL) | O_NONBLOCK);
    CHECK(TAG, "getfl", (fcntl(p[0], F_GETFL) & O_NONBLOCK) != 0, "flags=0x%x", fcntl(p[0], F_GETFL));
    alarm(3);   /* guard: SIGALRM kills us instead of hanging forever */
    r = (int)read(p[0], &b, 1); e = errno;
    CHECK(TAG, "fcntl-read", r == -1 && e == EAGAIN, "r=%d errno=%d", r, e);
    write(p[1], "z", 1);
    r = (int)read(p[0], &b, 1);
    CHECK(TAG, "fcntl-data", r == 1 && b == 'z', "r=%d", r);
    close(p[0]); close(p[1]);
    /* pipe2(O_NONBLOCK) */
    r = pipe2(p, O_NONBLOCK);
    CHECK(TAG, "pipe2-getfl", r == 0 && (fcntl(p[0], F_GETFL) & O_NONBLOCK), "r=%d flags=0x%x", r, fcntl(p[0], F_GETFL));
    r = (int)read(p[0], &b, 1); e = errno;
    CHECK(TAG, "pipe2-read", r == -1 && e == EAGAIN, "r=%d errno=%d", r, e);
    alarm(0);
    RESULT(TAG);
}
