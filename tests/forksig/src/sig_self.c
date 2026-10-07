#include <stdio.h>
#include <signal.h>
#include <unistd.h>
static volatile sig_atomic_t hit;
static void handler(int sig) { (void)sig; hit = 1; }
int main(void) {
    struct sigaction sa;
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGUSR1, &sa, NULL) < 0) { perror("sigaction"); return 1; }
    if (kill(getpid(), SIGUSR1) < 0) { perror("kill"); return 1; }
    for (int i = 0; i < 1000000 && !hit; i++) { }
    printf("sig_self hit=%d\n", (int)hit);
    return hit ? 0 : 2;
}
