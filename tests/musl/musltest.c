/* Stretch-goal Linux-ABI test for inDevOS: exercises the syscalls a typical
 * static musl program needs (brk/mmap via malloc, files, getdents64, cwd,
 * uname, clock_gettime, isatty, writev, auxv, environment).
 * Each check prints "ok" or "FAIL"; a summary line is printed at the end. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/auxv.h>
#include <sys/mman.h>

static int fails;
#define CHECK(name, cond) do { int _c = (cond); \
    printf("  %-28s %s\n", name, _c ? "ok" : "FAIL"); \
    if (!_c) { fails++; printf("    (errno=%d %s)\n", errno, strerror(errno)); } } while (0)

static __thread int tls_counter = 41;

int main(int argc, char **argv, char **envp)
{
    printf("musltest: argc=%d\n", argc);
    for (int i = 0; i < argc; i++) printf("  argv[%d]=%s\n", i, argv[i]);
    int nenv = 0; while (envp && envp[nenv]) nenv++;
    printf("  envc=%d PATH=%s\n", nenv, getenv("PATH") ? getenv("PATH") : "(unset)");

    tls_counter++;
    CHECK("TLS (__thread)", tls_counter == 42);
    CHECK("AT_PAGESZ", getauxval(AT_PAGESZ) == 4096);
    CHECK("AT_RANDOM", getauxval(AT_RANDOM) != 0);
    CHECK("getpid", getpid() > 0);

    /* malloc: small (brk) and large (mmap) */
    char *small = malloc(100);
    CHECK("malloc small", small != NULL);
    if (small) { strcpy(small, "brk heap"); free(small); }
    size_t big = 4u << 20;
    char *large = malloc(big);
    CHECK("malloc 4MiB", large != NULL);
    if (large) { memset(large, 0xA5, big); CHECK("touch 4MiB", (unsigned char)large[big-1] == 0xA5); free(large); }
    void *m = mmap(NULL, 65536, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    CHECK("mmap anon", m != MAP_FAILED);
    if (m != MAP_FAILED) CHECK("munmap", munmap(m, 65536) == 0);

    struct utsname u;
    CHECK("uname", uname(&u) == 0);
    printf("    %s %s %s %s\n", u.sysname, u.release, u.version, u.machine);

    struct timespec ts;
    CHECK("clock_gettime", clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    CHECK("time()", time(NULL) > 0);
    printf("    isatty(1)=%d\n", isatty(1));

    char cwd[256];
    CHECK("getcwd", getcwd(cwd, sizeof cwd) != NULL);
    printf("    cwd=%s\n", cwd);
    CHECK("chdir /", chdir("/") == 0);
    CHECK("getcwd after chdir", getcwd(cwd, sizeof cwd) && strcmp(cwd, "/") == 0);

    const char *path = "/MTXT.TXT";  /* short 8.3: FAT32 LFN delete is flaky */
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    CHECK("open O_CREAT", fd >= 0);
    if (fd >= 0) {
        struct iovec iov[2] = { { "hello ", 6 }, { "writev\n", 7 } };
        CHECK("writev", writev(fd, iov, 2) == 13);
        CHECK("close", close(fd) == 0);
    }
    fd = open(path, O_RDONLY);
    CHECK("open O_RDONLY", fd >= 0);
    if (fd >= 0) {
        char buf[64] = {0};
        CHECK("read", read(fd, buf, sizeof buf - 1) == 13 && strcmp(buf, "hello writev\n") == 0);
        CHECK("lseek SEEK_SET", lseek(fd, 6, SEEK_SET) == 6);
        memset(buf, 0, sizeof buf);
        CHECK("read after lseek", read(fd, buf, 6) == 6 && memcmp(buf, "writev", 6) == 0);
        struct stat st;
        CHECK("fstat", fstat(fd, &st) == 0 && st.st_size == 13 && S_ISREG(st.st_mode));
        close(fd);
    }
    struct stat st2;
    CHECK("stat", stat(path, &st2) == 0 && st2.st_size == 13);
    FILE *f = fopen(path, "r");
    CHECK("fopen/fgets", f != NULL);
    if (f) { char line[64]; CHECK("fgets content", fgets(line, sizeof line, f) && strncmp(line, "hello", 5) == 0); fclose(f); }
    CHECK("unlink", unlink(path) == 0);
    CHECK("stat after unlink fails", stat(path, &st2) != 0 && errno == ENOENT);

    DIR *d = opendir("/");
    CHECK("opendir /", d != NULL);
    if (d) {
        int n = 0; struct dirent *e;
        printf("    / :");
        while ((e = readdir(d)) != NULL) { if (n < 12) printf(" %s", e->d_name); n++; }
        printf("  (%d entries)\n", n);
        CHECK("readdir", n > 0);
        closedir(d);
    }

    printf("musltest: %s (%d failure%s)\n", fails ? "SOME CHECKS FAILED" : "ALL OK", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
