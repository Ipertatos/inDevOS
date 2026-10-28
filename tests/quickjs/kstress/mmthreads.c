/* mmthreads: do concurrent mmap()/brk from several threads hand out overlapping memory?
 * Each thread mmaps N small anonymous regions, stamps every page with (thread,index),
 * then re-checks all stamps. Any overlap/lost page shows up as a stamp mismatch.
 * Also a malloc phase (musl mallocng uses brk+mmap) with per-thread canaries.
 * Exit 0 = clean. Usage: mmthreads [threads=4] [n=200] */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/wait.h>
#define MAXT 16
static int NT = 4, N = 200, NOMALLOC, STATICSTK;
static char stacks[MAXT][256 * 1024] __attribute__((aligned(4096)));
static uint8_t **maps[MAXT];
static volatile int go;
static int bad_total;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static void *worker(void *arg) {
    int id = (int)(intptr_t)arg, bad = 0;
    while (!go) ;
    for (int i = 0; i < N; i++) {
        size_t len = 4096 * (1 + (i % 3));
        uint8_t *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) { printf("T%d mmap fail %d\n", id, i); bad++; continue; }
        for (size_t o = 0; o < len; o += 4096) { p[o] = (uint8_t)id; p[o + 1] = (uint8_t)i; p[o + 2] = 0x5a; }
        maps[id][i] = p;
    }
    for (int i = 0; i < N; i++) {
        uint8_t *p = maps[id][i]; if (!p) continue;
        size_t len = 4096 * (1 + (i % 3));
        for (size_t o = 0; o < len; o += 4096)
            if (p[o] != (uint8_t)id || p[o + 1] != (uint8_t)i || p[o + 2] != 0x5a) {
                if (bad < 5) printf("T%d region %d @%p+%zu stamp=%02x/%02x/%02x (overlap or lost page)\n", id, i, (void *)p, o, p[o], p[o + 1], p[o + 2]);
                bad++;
            }
    }
    /* malloc phase */
    static __thread char *m[512];
    for (int r = 0; r < (NOMALLOC ? 0 : 20); r++) {
        for (int i = 0; i < 512; i++) { size_t s = 16 + ((i * 37 + r) % 3000); m[i] = malloc(s); memset(m[i], id + 1, s); m[i][s - 1] = (char)(id + 1); }
        for (int i = 0; i < 512; i++) { size_t s = 16 + ((i * 37 + r) % 3000); if (m[i][0] != id + 1 || m[i][s - 1] != id + 1) { if (bad < 8) printf("T%d malloc block %d corrupted\n", id, i); bad++; } free(m[i]); }
    }
    for (int i = 0; i < N; i++) if (maps[id][i]) munmap(maps[id][i], 4096 * (1 + (i % 3)));
    __atomic_add_fetch(&bad_total, bad, __ATOMIC_SEQ_CST);
    return NULL;
}
int main(int argc, char **argv) {
    if (argc > 1) NT = atoi(argv[1]); if (NT > MAXT) NT = MAXT;
    if (argc > 2) N = atoi(argv[2]);
    if (argc > 3 && !strcmp(argv[3], "fork")) { pid_t c = fork(); if (c == 0) _exit(3); int st; waitpid(c, &st, 0); printf("forked child exit=%d\n", WEXITSTATUS(st)); }
    NOMALLOC = getenv("MMT_NOMALLOC") != NULL; STATICSTK = getenv("MMT_STATICSTACK") != NULL;
    printf("mode nomalloc=%d staticstack=%d\n", NOMALLOC, STATICSTK); fflush(stdout);
    for (int i = 0; i < NT; i++) maps[i] = calloc(N, sizeof(uint8_t *));
    pthread_t t[MAXT];
    for (int i = 0; i < NT; i++) {
        pthread_attr_t a; pthread_attr_init(&a);
        if (STATICSTK) pthread_attr_setstack(&a, stacks[i], sizeof(stacks[i]));
        int e = pthread_create(&t[i], &a, worker, (void *)(intptr_t)i);
        if (e) printf("pthread_create %d failed %d\n", i, e);
    }
    go = 1;
    for (int i = 0; i < NT; i++) pthread_join(t[i], NULL);
    printf("mmthreads threads=%d n=%d bad=%d -> %s\n", NT, N, bad_total, bad_total ? "FAIL" : "OK");
    return bad_total ? 1 : 0;
}
