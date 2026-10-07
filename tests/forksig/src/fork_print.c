#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdlib.h>
int main(void) {
    pid_t p = fork();
    if (p < 0) { perror("fork"); return 1; }
    if (p == 0) {
        printf("child pid=%d ppid=%d\n", (int)getpid(), (int)getppid());
        fflush(stdout);
        _exit(42);
    }
    printf("parent pid=%d child=%d\n", (int)getpid(), (int)p);
    fflush(stdout);
    int st = 0;
    pid_t w = waitpid(p, &st, 0);
    printf("waited w=%d status=%d exit=%d\n", (int)w, st, WIFEXITED(st) ? WEXITSTATUS(st) : -1);
    return (w == p && WIFEXITED(st) && WEXITSTATUS(st) == 42) ? 0 : 2;
}
