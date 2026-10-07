#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdlib.h>
static volatile int g = 7;
int main(void) {
    pid_t p = fork();
    if (p < 0) { perror("fork"); return 1; }
    if (p == 0) {
        g = 99;
        printf("child g=%d\n", g);
        fflush(stdout);
        _exit(0);
    }
    int st = 0;
    waitpid(p, &st, 0);
    printf("parent g=%d (expect 7)\n", g);
    return (g == 7) ? 0 : 2;
}
