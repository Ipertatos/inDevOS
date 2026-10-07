/* fork_stack: detect COW/TLB corruption of parent stack by child writes. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/wait.h>

volatile int g_marker = 0x11111111;

int main(void) {
    int pipefd[2] = {0x41, 0x42};
    int a = 0xA5A5A5A5;
    int b = 0x5A5A5A5A;
    long c = 0xDEADBEEFCAFEBABEULL;
    char buf[64];
    memset(buf, 0x5C, sizeof(buf));
    buf[0] = 'P'; buf[1] = 'A'; buf[2] = 'R'; buf[3] = 0;

    if (pipe(pipefd) != 0) {
        perror("pipe");
        return 2;
    }

    int saved_r = pipefd[0], saved_w = pipefd[1];
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 3;
    }
    if (pid == 0) {
        /* Child: smash stack locals and exit quickly. */
        a = 0; b = 0; c = 0;
        pipefd[0] = -1; pipefd[1] = -1;
        memset(buf, 0xCC, sizeof(buf));
        g_marker = 0x22222222;
        _exit(0);
    }

    /* Give child time to run and write (and for TLB races to show). */
    usleep(50000);
    int st = 0;
    waitpid(pid, &st, 0);

    int fail = 0;
    if (a != (int)0xA5A5A5A5) { printf("FAIL a=0x%x\n", a); fail++; }
    if (b != (int)0x5A5A5A5A) { printf("FAIL b=0x%x\n", b); fail++; }
    if (c != (long)0xDEADBEEFCAFEBABEULL) { printf("FAIL c=0x%lx\n", (unsigned long)c); fail++; }
    if (pipefd[0] != saved_r) { printf("FAIL pipefd[0]=%d want %d\n", pipefd[0], saved_r); fail++; }
    if (pipefd[1] != saved_w) { printf("FAIL pipefd[1]=%d want %d\n", pipefd[1], saved_w); fail++; }
    if (buf[0] != 'P' || buf[1] != 'A' || buf[2] != 'R') {
        printf("FAIL buf='%.4s'\n", buf); fail++;
    }
    if (g_marker != 0x11111111) { printf("FAIL g_marker=0x%x\n", g_marker); fail++; }

    close(pipefd[0]); close(pipefd[1]);
    if (fail) {
        printf("fork_stack FAIL count=%d\n", fail);
        return 1;
    }
    printf("fork_stack OK\n");
    return 0;
}
