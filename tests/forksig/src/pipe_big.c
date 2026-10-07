#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <string.h>
#define N (128 * 1024)
int main(void) {
    int fds[2];
    if (pipe(fds) < 0) { perror("pipe"); return 1; }
    pid_t p = fork();
    if (p < 0) { perror("fork"); return 1; }
    if (p == 0) {
        close(fds[0]);
        char buf[4096];
        memset(buf, 'A', sizeof buf);
        size_t left = N;
        while (left) {
            size_t chunk = left > sizeof buf ? sizeof buf : left;
            ssize_t w = write(fds[1], buf, chunk);
            if (w < 0) { perror("write"); _exit(1); }
            left -= (size_t)w;
        }
        close(fds[1]);
        _exit(0);
    }
    close(fds[1]);
    size_t got = 0;
    char buf[4096];
    for (;;) {
        ssize_t r = read(fds[0], buf, sizeof buf);
        if (r < 0) { perror("read"); return 1; }
        if (r == 0) break;
        got += (size_t)r;
    }
    close(fds[0]);
    int st = 0;
    waitpid(p, &st, 0);
    printf("pipe_big got=%zu expect=%d\n", got, N);
    return (got == N && WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : 2;
}
