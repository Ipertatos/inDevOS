/* repro_fdpoll: fd table was 16 (dup2 to 60 / F_DUPFD 40 -> EBADF/EMFILE),
 * poll() never reported POLLOUT on a pipe write end, select() always -EFAULT
 * (kernel pollfd buffer), O_APPEND ignored on FAT32 files. */
#include "repro_common.h"
#include <sys/select.h>
#include <sys/resource.h>
int main(void) {
    struct rlimit rl; getrlimit(RLIMIT_NOFILE, &rl);
    CHECK("fdpoll", "nofile", rl.rlim_cur >= 64, "RLIMIT_NOFILE cur=%ld", (long)rl.rlim_cur);
    int a = dup2(1, 60); CHECK("fdpoll", "dup2", a == 60, "dup2(1,60)=%d errno=%d", a, errno);
    int b = fcntl(1, F_DUPFD, 40); CHECK("fdpoll", "F_DUPFD", b >= 40, "F_DUPFD 40 -> %d errno=%d", b, errno);
    if (a >= 0) close(a);
    if (b >= 0) close(b);
    int p[2]; pipe(p);
    struct pollfd pf[2] = {{p[0], POLLIN, 0}, {p[1], POLLOUT, 0}};
    int r = poll(pf, 2, 0);
    CHECK("fdpoll", "POLLOUT", r == 1 && (pf[1].revents & POLLOUT) && !(pf[0].revents & POLLIN), "r=%d rev0=%x rev1=%x", r, pf[0].revents, pf[1].revents);
    write(p[1], "x", 1); pf[0].revents = pf[1].revents = 0; r = poll(pf, 2, 0);
    CHECK("fdpoll", "POLLIN", r == 2 && (pf[0].revents & POLLIN), "r=%d rev0=%x", r, pf[0].revents);
    fd_set rs; FD_ZERO(&rs); FD_SET(p[0], &rs); struct timeval tv = {0, 0};
    r = select(p[0] + 1, &rs, NULL, NULL, &tv);
    CHECK("fdpoll", "select_rd", r == 1 && FD_ISSET(p[0], &rs), "r=%d errno=%d", r, errno);
    tv.tv_sec = 0; tv.tv_usec = 150000; long t0 = now_ms(); r = select(0, NULL, NULL, NULL, &tv); long dt = now_ms() - t0;
    CHECK("fdpoll", "select_empty", r == 0 && dt >= 100 && dt < 1500, "r=%d errno=%d %ldms", r, errno, dt);
    const char *f = "/tmp/rpapp.txt"; int fd = open(f, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { f = "rpapp.txt"; fd = open(f, O_WRONLY | O_CREAT | O_TRUNC, 0644); }
    write(fd, "abc", 3); close(fd);
    fd = open(f, O_WRONLY | O_APPEND); lseek(fd, 0, SEEK_SET); write(fd, "def", 3); close(fd);
    char buf[16] = ""; fd = open(f, O_RDONLY); int n = read(fd, buf, 15); close(fd); unlink(f);
    CHECK("fdpoll", "O_APPEND", n == 6 && !memcmp(buf, "abcdef", 6), "n=%d '%.15s'", n, buf);
    RESULT("fdpoll");
}
