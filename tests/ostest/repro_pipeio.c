/* repro_pipeio: FIONREAD on a pipe always returned 0, and poll() on the read
 * end never reported POLLHUP after the last writer closed (misc.fcntl/poll). */
#include "repro_common.h"
#include <sys/ioctl.h>
int main(void) {
    int p[2]; pipe(p); int n = -1;
    write(p[1], "hello", 5);
    CHECK("pipeio", "FIONREAD", ioctl(p[0], FIONREAD, &n) == 0 && n == 5, "n=%d", n);
    struct pollfd pf = { p[0], POLLIN, 0 };
    int r = poll(&pf, 1, 0);
    CHECK("pipeio", "open_nohup", r == 1 && (pf.revents & POLLIN) && !(pf.revents & POLLHUP), "r=%d rev=%x", r, pf.revents);
    close(p[1]); pf.revents = 0; r = poll(&pf, 1, 100);
    CHECK("pipeio", "data_hup", r == 1 && (pf.revents & POLLIN) && (pf.revents & POLLHUP), "r=%d rev=%x", r, pf.revents);
    char b[8]; read(p[0], b, 5); pf.revents = 0; r = poll(&pf, 1, 100);
    CHECK("pipeio", "eof_hup", r == 1 && (pf.revents & POLLHUP), "r=%d rev=%x", r, pf.revents);
    CHECK("pipeio", "eof_read", read(p[0], b, 1) == 0, "read 0 at EOF");
    close(p[0]);
    /* forked writer exits -> POLLHUP in parent */
    pipe(p); pid_t c = fork(); if (c == 0) { close(p[0]); msleep(50); _exit(0); }
    close(p[1]); pf.fd = p[0]; pf.revents = 0; long t0 = now_ms(); r = poll(&pf, 1, 2000);
    CHECK("pipeio", "child_hup", r == 1 && (pf.revents & POLLHUP), "r=%d rev=%x after %ldms", r, pf.revents, now_ms() - t0);
    waitpid(c, NULL, 0);
    RESULT("pipeio");
}
