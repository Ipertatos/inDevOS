/*
 * ostest - inDevOS system test (static musl).
 *
 *   ostest                 all safe tests
 *   ostest -g fs,proc      only these groups (net.dns_connect needs -g net)
 *   ostest -l              list tests
 *   ostest -v              verbose (-v draw: also draw a square on /dev/fb0)
 *   ostest --destructive   also run tests marked destructive (none touch user data)
 *
 * Every test runs in its own forked process group with a timeout; a hung
 * test is killed and reported as FAIL (timeout). Temporary files live under
 * /tmp/ostest.<pid> and are removed at the end. The report is written to
 * /home/user/ostest_report.txt (root disk). Exit code = number of FAILs.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <dirent.h>
#include <poll.h>
#include <netdb.h>
#include <termios.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stddef.h>
#include <sys/sysinfo.h>
#include <sys/statfs.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/file.h>
#include <sys/uio.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/timerfd.h>
#include <sys/signalfd.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include "wm.h"

#define PASS 0
#define FAIL 1
#define SKIP 2
#define TF_EXPLICIT 1      /* only when its group is named with -g */
#define TF_DESTRUCT 2      /* only with --destructive */

typedef int (*tfn_t)(char *d, size_t n);
struct test { const char *group, *name; tfn_t fn; int timeout; int flags; };

static int verbose, destructive, draw;
static char tmpdir[128];
static const char *self_path = "/bin/ostest.exe";

#define R(code, ...) do { snprintf(d, n, __VA_ARGS__); return code; } while (0)
#define CHECK(c, ...) do { if (!(c)) R(FAIL, __VA_ARGS__); } while (0)
#define CHECKE(c, what) do { if (!(c)) R(FAIL, "%s: %s", what, strerror(errno)); } while (0)

static long long now_ns(clockid_t c) { struct timespec t; clock_gettime(c, &t); return t.tv_sec * 1000000000LL + t.tv_nsec; }
static long long mono_ms(void) { return now_ns(CLOCK_MONOTONIC) / 1000000; }
static void msleep(int ms) { struct timespec t = { ms / 1000, (ms % 1000) * 1000000L }; while (nanosleep(&t, &t) && errno == EINTR) {} }
static void tpath(char *b, size_t n, const char *name) { snprintf(b, n, "%s/%s", tmpdir, name); }
static uint32_t fnv(const void *p, size_t n, uint32_t h) { const uint8_t *b = p; while (n--) { h ^= *b++; h *= 16777619u; } return h; }
static void fill(uint8_t *b, size_t n, uint32_t seed) { for (size_t i = 0; i < n; i++) { seed = seed * 1103515245u + 12345u; b[i] = (uint8_t)(seed >> 16); } }
static ssize_t writeall(int fd, const void *b, size_t n) { size_t o = 0; while (o < n) { ssize_t r = write(fd, (const char *)b + o, n - o); if (r < 0 && errno == EINTR) continue; if (r <= 0) return r < 0 ? r : (ssize_t)o; o += r; } return o; }
static ssize_t readall(int fd, void *b, size_t n) { size_t o = 0; while (o < n) { ssize_t r = read(fd, (char *)b + o, n - o); if (r < 0 && errno == EINTR) continue; if (r <= 0) return r < 0 && o == 0 ? r : (ssize_t)o; o += r; } return o; }
static int put_file(const char *p, const void *b, size_t n) { int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (fd < 0) return -1; ssize_t w = writeall(fd, b, n); close(fd); return w == (ssize_t)n ? 0 : -1; }
static ssize_t get_file(const char *p, void *b, size_t n) { int fd = open(p, O_RDONLY); if (fd < 0) return -1; ssize_t r = readall(fd, b, n); close(fd); return r; }

/* run argv, capture stdout+stderr (up to cap) via a temp file; exit status, -2 = timeout.
 * Uses only fork/exec/waitpid(WNOHANG)/nanosleep, so a kernel that ignores
 * O_NONBLOCK on pipes cannot hang the harness. */
static int run_capture(char *const argv[], char *out, size_t cap, int timeout_ms) {
    static int seq; char f[160]; snprintf(f, sizeof(f), "%s/cap.%d.%d", tmpdir, getpid(), seq++);
    pid_t c = fork();
    if (c < 0) return -1;
    if (c == 0) { int fd = open(f, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); } execv(argv[0], argv); _exit(127); }
    (void)timeout_ms; int st = 0, done = 0;      /* the test's own timeout bounds this wait */
    if (waitpid(c, &st, 0) == c) done = 1;
    ssize_t r = get_file(f, out, cap - 1); out[r > 0 ? r : 0] = 0; unlink(f);
    if (!done) return -2;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

/* ===================== sys ===================== */
static int t_uname(char *d, size_t n) {
    struct utsname u; CHECKE(uname(&u) == 0, "uname");
    CHECK(u.sysname[0] && u.machine[0], "empty fields");
    R(PASS, "%s %s %s %s", u.sysname, u.release, u.version, u.machine);
}
static int t_pids(char *d, size_t n) {
    pid_t p = getpid(), pp = getppid();
    CHECK(p > 0 && pp > 0 && p != pp, "pid=%d ppid=%d", p, pp);
    int pf[2]; pipe(pf);
    pid_t c = fork(); CHECKE(c >= 0, "fork");
    if (c == 0) { pid_t v[2] = { getpid(), getppid() }; write(pf[1], v, sizeof(v)); _exit(0); }
    pid_t v[2] = {0}; read(pf[0], v, sizeof(v)); waitpid(c, NULL, 0);
    CHECK(v[0] == c && v[1] == p, "child sees pid=%d ppid=%d (want %d %d)", v[0], v[1], c, p);
    CHECK(gettid() == p, "gettid %d != pid %d", gettid(), p);
    R(PASS, "pid=%d ppid=%d child ok", p, pp);
}
static int t_ids(char *d, size_t n) {
    mode_t m = umask(022); umask(m);
    R(PASS, "uid=%d euid=%d gid=%d pgrp=%d sid=%d umask=%03o", getuid(), geteuid(), getgid(), getpgrp(), getsid(0), m);
}
static int t_time(char *d, size_t n) {
    time_t t = time(NULL); struct timeval tv; gettimeofday(&tv, NULL);
    struct timespec rt; clock_gettime(CLOCK_REALTIME, &rt);
    CHECK(llabs((long long)t - tv.tv_sec) <= 1 && llabs((long long)t - rt.tv_sec) <= 1, "time=%lld tv=%lld rt=%lld", (long long)t, (long long)tv.tv_sec, (long long)rt.tv_sec);
    struct tm tm; gmtime_r(&t, &tm);
    CHECK(tm.tm_year + 1900 >= 2024 && tm.tm_year + 1900 < 2100, "RTC year %d not sane", tm.tm_year + 1900);
    R(PASS, "%04d-%02d-%02d %02d:%02d:%02d UTC", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
}
static int t_clock(char *d, size_t n) {
    struct timespec r; CHECKE(clock_getres(CLOCK_MONOTONIC, &r) == 0, "clock_getres");
    long long a = now_ns(CLOCK_MONOTONIC), prev = a; int back = 0;
    for (int i = 0; i < 100000; i++) { long long x = now_ns(CLOCK_MONOTONIC); if (x < prev) back++; prev = x; }
    CHECK(!back, "monotonic went backwards %d times", back);
    CHECK(prev > a, "monotonic did not advance over 100k reads");
    long long b = now_ns(CLOCK_BOOTTIME);
    R(PASS, "res=%ldns, 100k reads in %lldus, boottime=%llds", r.tv_nsec, (prev - a) / 1000, b / 1000000000LL);
}
static int t_nanosleep(char *d, size_t n) {
    int want[3] = { 10, 100, 500 }; char b[128] = ""; int o = 0;
    for (int i = 0; i < 3; i++) {
        long long t0 = now_ns(CLOCK_MONOTONIC); msleep(want[i]);
        long long el = (now_ns(CLOCK_MONOTONIC) - t0) / 1000000;
        o += snprintf(b + o, sizeof(b) - o, "%dms->%lldms ", want[i], el);
        CHECK(el >= want[i] - 1 && el <= want[i] * 2 + 20, "%s", b);
    }
    R(PASS, "%s", b);
}
static int t_sysinfo(char *d, size_t n) {
    struct sysinfo s; CHECKE(sysinfo(&s) == 0, "sysinfo");
    CHECK(s.totalram > 0, "totalram 0");
    struct rlimit rl; CHECKE(getrlimit(RLIMIT_NOFILE, &rl) == 0, "getrlimit");
    R(PASS, "uptime=%lds ram=%luMB free=%luMB procs=%u nofile=%llu", s.uptime, (unsigned long)(s.totalram * s.mem_unit >> 20), (unsigned long)(s.freeram * s.mem_unit >> 20), s.procs, (unsigned long long)rl.rlim_cur);
}
static int t_getrandom(char *d, size_t n) {
    uint8_t a[64] = {0}, b[64] = {0};
    CHECKE(getrandom(a, 64, 0) == 64, "getrandom");
    getrandom(b, 64, 0);
    CHECK(memcmp(a, b, 64), "two getrandom calls identical");
    R(PASS, "64 bytes, differ");
}

/* ===================== mem ===================== */
static int t_malloc_big(char *d, size_t n) {
    size_t sz = 64u << 20; uint8_t *p = malloc(sz); CHECK(p, "malloc 64MB failed");
    for (size_t i = 0; i < sz; i += 4096) p[i] = (uint8_t)(i >> 12);
    for (size_t i = 0; i < sz; i += 4096) CHECK(p[i] == (uint8_t)(i >> 12), "bad byte at %zu", i);
    free(p);
    void *q[256]; for (int i = 0; i < 256; i++) { q[i] = malloc(100 + i * 37); CHECK(q[i], "small malloc %d", i); memset(q[i], i, 100 + i * 37); }
    for (int i = 0; i < 256; i++) { CHECK(((uint8_t *)q[i])[99] == (uint8_t)i, "small block %d corrupted", i); free(q[i]); }
    R(PASS, "64MB touched + 256 small blocks");
}
static int t_mmap_anon(char *d, size_t n) {
    size_t sz = 16u << 20; uint8_t *p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECKE(p != MAP_FAILED, "mmap");
    for (size_t i = 0; i < sz; i += 4096) CHECK(p[i] == 0, "not zero-filled at %zu", i);
    for (size_t i = 0; i < sz; i += 4096) p[i] = 0x5A;
    CHECKE(munmap(p, sz) == 0, "munmap");
    R(PASS, "16MB zero-filled, writable, unmapped");
}
static int t_mmap_file(char *d, size_t n) {
    char p[160]; tpath(p, sizeof(p), "mmapf.bin");
    static uint8_t b[3 * 4096 + 100]; fill(b, sizeof(b), 7);
    CHECKE(put_file(p, b, sizeof(b)) == 0, "write file");
    int fd = open(p, O_RDONLY); CHECKE(fd >= 0, "open");
    uint8_t *m = mmap(NULL, sizeof(b), PROT_READ, MAP_PRIVATE, fd, 0); CHECKE(m != MAP_FAILED, "mmap file");
    CHECK(memcmp(m, b, sizeof(b)) == 0, "mapped contents differ");
    munmap(m, sizeof(b));
    m = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, fd, 4096); CHECKE(m != MAP_FAILED, "mmap offset 4096");
    CHECK(memcmp(m, b + 4096, 4096) == 0, "offset mapping differs");
    munmap(m, 4096); close(fd); unlink(p);
    R(PASS, "%zu bytes, offset map ok", sizeof(b));
}
static int t_mmap_shared(char *d, size_t n) {
    volatile int *s = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECKE(s != MAP_FAILED, "mmap shared");
    s[0] = 1;
    pid_t c = fork(); CHECKE(c >= 0, "fork");
    if (c == 0) { s[0] = 42; s[1] = 7; _exit(0); }
    waitpid(c, NULL, 0);
    CHECK(s[0] == 42 && s[1] == 7, "parent sees %d,%d after child wrote 42,7", s[0], s[1]);
    R(PASS, "child write visible in parent");
}
static int t_mmap_shared_file(char *d, size_t n) {
    char p[160]; tpath(p, sizeof(p), "mshared.bin");
    char z[8192]; memset(z, 'a', sizeof(z)); CHECKE(put_file(p, z, sizeof(z)) == 0, "write");
    int fd = open(p, O_RDWR); CHECKE(fd >= 0, "open");
    char *m = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); CHECKE(m != MAP_FAILED, "mmap MAP_SHARED file");
    memcpy(m + 100, "SHARED", 6); msync(m, 8192, MS_SYNC); munmap(m, 8192); close(fd);
    char r[8192]; get_file(p, r, sizeof(r)); unlink(p);
    CHECK(memcmp(r + 100, "SHARED", 6) == 0, "store through MAP_SHARED file mapping not in file after msync+munmap");
    R(PASS, "write-back ok");
}
static int t_mmap_shared_coh(char *d, size_t n) {
    char p[160]; tpath(p, sizeof(p), "mshcoh.bin");
    char z[8192]; memset(z, 'b', sizeof(z)); CHECKE(put_file(p, z, sizeof(z)) == 0, "write");
    int fd = open(p, O_RDWR); CHECKE(fd >= 0, "open");
    char *m = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); CHECKE(m != MAP_FAILED, "mmap");
    memcpy(m + 5000, "MAPW", 4);
    char r[8]; memset(r, 0, sizeof(r));
    CHECKE(pread(fd, r, 4, 5000) == 4, "pread");
    CHECK(memcmp(r, "MAPW", 4) == 0, "read() does not see store through mapping");
    CHECKE(pwrite(fd, "FILW", 4, 300) == 4, "pwrite");
    CHECK(memcmp(m + 300, "FILW", 4) == 0, "mapping does not see write()");
    pid_t c = fork(); CHECKE(c >= 0, "fork");
    if (c == 0) { memcpy(m + 7000, "KID!", 4); _exit(0); }
    int st; waitpid(c, &st, 0);
    CHECK(memcmp(m + 7000, "KID!", 4) == 0, "child store not visible to parent");
    munmap(m, 8192); close(fd);
    char f[8192]; get_file(p, f, sizeof(f)); unlink(p);
    CHECK(memcmp(f + 7000, "KID!", 4) == 0 && memcmp(f + 5000, "MAPW", 4) == 0, "file content after munmap");
    R(PASS, "map<->read/write coherent, fork child store written back");
}

/* ---- batch 6: symlinks, hard links, inode permissions ----
 * lk_sym/lk_hard/lk_perm run in directory @b on a filesystem that supports
 * them (tmpfs, ext4); verified against Linux tmpfs/ext4 on the build box. */
static int lk_sym(const char *b, char *d, size_t n) {
    char f[300], l[300], p[300], q[300], buf[1100]; struct stat st, lst;
    snprintf(f, sizeof f, "%s/tgt.txt", b); snprintf(l, sizeof l, "%s/l_rel", b);
    CHECKE(put_file(f, "target-data", 11) == 0, "create target");
    CHECKE(symlink("tgt.txt", l) == 0, "symlink rel");
    ssize_t r = readlink(l, buf, sizeof buf); CHECK(r == 7 && !memcmp(buf, "tgt.txt", 7), "readlink rel: %zd '%.*s'", r, (int)(r > 0 ? r : 0), buf);
    CHECK(readlink(l, buf, 3) == 3 && !memcmp(buf, "tgt", 3), "readlink truncation");
    CHECK(readlink(f, buf, sizeof buf) == -1 && errno == EINVAL, "readlink of a file: errno %d (want EINVAL)", errno);
    CHECKE(lstat(l, &lst) == 0, "lstat link");
    CHECK(S_ISLNK(lst.st_mode) && lst.st_size == 7 && (lst.st_mode & 0777) == 0777, "lstat: mode %o size %lld", lst.st_mode, (long long)lst.st_size);
    CHECKE(stat(l, &st) == 0, "stat via link");
    CHECK(S_ISREG(st.st_mode) && st.st_size == 11, "stat via link: mode %o size %lld", st.st_mode, (long long)st.st_size);
    CHECK(get_file(l, buf, sizeof buf) == 11 && !memcmp(buf, "target-data", 11), "read through link");
    CHECKE(put_file(l, "written-via-link", 16) == 0, "write through link");
    CHECK(get_file(f, buf, sizeof buf) == 16 && !memcmp(buf, "written-via-link", 16), "target content after write via link");
    CHECK(lstat(l, &lst) == 0 && S_ISLNK(lst.st_mode), "link replaced by write");
    /* directory link, walk through it, physical ".." */
    snprintf(p, sizeof p, "%s/dA", b); CHECKE(mkdir(p, 0755) == 0, "mkdir dA");
    snprintf(p, sizeof p, "%s/dA/sub", b); CHECKE(mkdir(p, 0755) == 0, "mkdir dA/sub");
    snprintf(p, sizeof p, "%s/dA/sub/in.txt", b); CHECKE(put_file(p, "inner", 5) == 0, "create dA/sub/in.txt");
    snprintf(l, sizeof l, "%s/l_dir", b); CHECKE(symlink("dA/sub", l) == 0, "symlink dir");
    snprintf(p, sizeof p, "%s/l_dir/in.txt", b);
    CHECK(get_file(p, buf, sizeof buf) == 5 && !memcmp(buf, "inner", 5), "read l_dir/in.txt: %s", strerror(errno));
    snprintf(p, sizeof p, "%s/l_dir/../sub/in.txt", b);       /* ".." of dA/sub is dA */
    CHECK(stat(p, &st) == 0 && st.st_size == 5, "physical '..' after a dir link: %s", strerror(errno));
    snprintf(p, sizeof p, "%s/l_dir/", b); CHECK(stat(p, &st) == 0 && S_ISDIR(st.st_mode), "trailing slash follows");
    CHECK(lstat(l, &lst) == 0 && S_ISLNK(lst.st_mode), "lstat dir link");
    snprintf(p, sizeof p, "%s/l_dir/new.txt", b); CHECKE(put_file(p, "nw", 2) == 0, "create through dir link");
    snprintf(p, sizeof p, "%s/dA/sub/new.txt", b); CHECK(stat(p, &st) == 0 && st.st_size == 2, "file created via dir link missing");
    CHECK(rmdir(l) == -1 && errno == ENOTDIR, "rmdir on dir link: errno %d (want ENOTDIR)", errno);
    /* getdents d_type */
    DIR *D = opendir(b); CHECKE(D, "opendir"); struct dirent *e; int tl = 0, tr = 0;
    while ((e = readdir(D))) { if (!strcmp(e->d_name, "l_dir") && e->d_type == DT_LNK) tl++; if (!strcmp(e->d_name, "tgt.txt") && e->d_type == DT_REG) tr++; }
    closedir(D); CHECK(tl == 1 && tr == 1, "d_type: l_dir DT_LNK %d, tgt.txt DT_REG %d", tl, tr);
    /* absolute + slow (long) target */
    char lt[400]; int k = snprintf(lt, sizeof lt, "%s", b);
    while (k < 200) k += snprintf(lt + k, sizeof lt - k, "/.");
    snprintf(lt + k, sizeof lt - k, "/tgt.txt");
    snprintf(l, sizeof l, "%s/l_slow", b); CHECKE(symlink(lt, l) == 0, "symlink long abs");
    r = readlink(l, buf, sizeof buf); CHECK(r == (ssize_t)strlen(lt) && !memcmp(buf, lt, r), "readlink slow: %zd want %zu", r, strlen(lt));
    CHECK(lstat(l, &lst) == 0 && lst.st_size == (off_t)strlen(lt), "lstat slow size %lld", (long long)lst.st_size);
    CHECK(get_file(l, buf, sizeof buf) == 16, "read via slow link: %s", strerror(errno));
    /* dangling, O_CREAT through dangling, O_NOFOLLOW, O_EXCL */
    snprintf(l, sizeof l, "%s/l_dang", b); CHECKE(symlink("dang.txt", l) == 0, "symlink dangling");
    CHECK(stat(l, &st) == -1 && errno == ENOENT, "stat dangling: errno %d", errno);
    CHECK(lstat(l, &lst) == 0 && S_ISLNK(lst.st_mode), "lstat dangling");
    CHECK(open(l, O_RDONLY) == -1 && errno == ENOENT, "open dangling: errno %d", errno);
    CHECK(open(l, O_WRONLY | O_CREAT | O_EXCL, 0644) == -1 && errno == EEXIST, "O_CREAT|O_EXCL on link: errno %d", errno);
    int fd = open(l, O_WRONLY | O_CREAT, 0644); CHECKE(fd >= 0, "O_CREAT through dangling link"); write(fd, "dg", 2); close(fd);
    snprintf(p, sizeof p, "%s/dang.txt", b); CHECK(stat(p, &st) == 0 && st.st_size == 2, "target of dangling link not created");
    CHECK(open(l, O_RDONLY | O_NOFOLLOW) == -1 && errno == ELOOP, "O_NOFOLLOW: errno %d (want ELOOP)", errno);
    fd = open(f, O_RDONLY | O_NOFOLLOW); CHECK(fd >= 0, "O_NOFOLLOW on a plain file: %s", strerror(errno)); close(fd);
    /* fstatat / readlinkat with dirfd */
    int dfd = open(b, O_RDONLY | O_DIRECTORY); CHECKE(dfd >= 0, "open dir");
    CHECK(fstatat(dfd, "l_rel", &lst, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(lst.st_mode), "fstatat NOFOLLOW");
    CHECK(fstatat(dfd, "l_rel", &st, 0) == 0 && S_ISREG(st.st_mode), "fstatat follow");
    CHECK(readlinkat(dfd, "l_dir", buf, sizeof buf) == 6 && !memcmp(buf, "dA/sub", 6), "readlinkat");
    CHECK(symlinkat("tgt.txt", dfd, "l_at") == 0, "symlinkat: %s", strerror(errno));
    close(dfd);
    /* loops: self, pair, 40-link chain ok, 41 ELOOP */
    snprintf(l, sizeof l, "%s/l_self", b); CHECKE(symlink("l_self", l) == 0, "symlink self");
    CHECK(stat(l, &st) == -1 && errno == ELOOP, "self loop: errno %d (want ELOOP)", errno);
    CHECK(open(l, O_RDONLY) == -1 && errno == ELOOP, "open self loop: errno %d", errno);
    for (int i = 0; i <= 40; i++) {
        char nm[300], tg[32]; snprintf(nm, sizeof nm, "%s/c%02d", b, i);
        if (i == 40) snprintf(tg, sizeof tg, "tgt.txt"); else snprintf(tg, sizeof tg, "c%02d", i + 1);
        CHECK(symlink(tg, nm) == 0, "chain symlink %d: %s", i, strerror(errno));
    }
    snprintf(p, sizeof p, "%s/c01", b); CHECK(stat(p, &st) == 0, "40-link chain: %s", strerror(errno));
    snprintf(p, sizeof p, "%s/c00", b); CHECK(stat(p, &st) == -1 && errno == ELOOP, "41-link chain: errno %d (want ELOOP)", errno);
    /* rename / unlink a link leave the target alone */
    snprintf(p, sizeof p, "%s/l_rel", b); snprintf(q, sizeof q, "%s/l_ren", b);
    CHECKE(rename(p, q) == 0, "rename link");
    CHECK(readlink(q, buf, sizeof buf) == 7, "renamed link readlink");
    CHECKE(unlink(q) == 0, "unlink link");
    CHECK(stat(f, &st) == 0 && st.st_size == 16, "target gone after unlinking link");
    CHECK(symlink("", q) == -1 && errno == ENOENT, "empty target: errno %d", errno);
    CHECK(symlink("x", f) == -1 && errno == EEXIST, "symlink onto file: errno %d", errno);
    /* cleanup */
    const char *rm[] = { "l_dir/new.txt", "l_dir", "l_slow", "l_dang", "dang.txt", "l_at", "l_self", "dA/sub/in.txt", "dA/sub", "dA", "tgt.txt", NULL };
    for (int i = 0; i <= 40; i++) { snprintf(p, sizeof p, "%s/c%02d", b, i); unlink(p); }
    for (int i = 0; rm[i]; i++) { snprintf(p, sizeof p, "%s/%s", b, rm[i]); if (unlink(p) != 0) CHECK(rmdir(p) == 0, "cleanup %s: %s", p, strerror(errno)); }
    R(PASS, "readlink/lstat/stat, rel+abs+slow(%zu B) targets, dir link walk + physical '..', DT_LNK, dangling + O_CREAT, O_NOFOLLOW ELOOP, O_EXCL EEXIST, ELOOP self/41 (40 ok), rename/unlink", strlen(lt));
}
static int lk_hard(const char *b, char *d, size_t n) {
    char f[300], h[300], h2[300], p[300], buf[64]; struct stat s1, s2;
    snprintf(f, sizeof f, "%s/h_a", b); snprintf(h, sizeof h, "%s/h_b", b); snprintf(h2, sizeof h2, "%s/h_c", b);
    CHECKE(put_file(f, "hard", 4) == 0, "create");
    CHECKE(link(f, h) == 0, "link");
    CHECK(stat(f, &s1) == 0 && stat(h, &s2) == 0, "stat both");
    CHECK(s1.st_ino == s2.st_ino && s1.st_nlink == 2 && s2.st_nlink == 2, "ino %llu/%llu nlink %lu/%lu", (unsigned long long)s1.st_ino, (unsigned long long)s2.st_ino, (unsigned long)s1.st_nlink, (unsigned long)s2.st_nlink);
    CHECKE(linkat(AT_FDCWD, h, AT_FDCWD, h2, 0) == 0, "linkat");
    CHECK(stat(f, &s1) == 0 && s1.st_nlink == 3, "nlink after linkat %lu", (unsigned long)s1.st_nlink);
    int fd = open(h, O_WRONLY | O_APPEND); CHECKE(fd >= 0, "open b"); write(fd, "-more", 5); close(fd);
    CHECK(get_file(f, buf, sizeof buf) == 9 && !memcmp(buf, "hard-more", 9), "write via one name not seen via other");
    CHECK(link(f, h) == -1 && errno == EEXIST, "link onto existing: errno %d", errno);
    snprintf(p, sizeof p, "%s/h_dir", b); CHECKE(mkdir(p, 0755) == 0, "mkdir");
    snprintf(buf, sizeof buf, "x"); char hd[300]; snprintf(hd, sizeof hd, "%s/h_dirl", b);
    CHECK(link(p, hd) == -1 && errno == EPERM, "link of a dir: errno %d (want EPERM)", errno);
    rmdir(p);
    CHECK(rename(f, h) == 0 && stat(f, &s1) == 0 && stat(h, &s2) == 0, "rename between hard links of one inode must be a no-op");
    CHECKE(unlink(f) == 0, "unlink a");
    CHECK(stat(h, &s2) == 0 && s2.st_nlink == 2 && s2.st_size == 9, "after unlink: nlink %lu size %lld", (unsigned long)s2.st_nlink, (long long)s2.st_size);
    fd = open(h, O_RDONLY); CHECKE(fd >= 0, "open b");
    CHECKE(unlink(h) == 0 && unlink(h2) == 0, "unlink last names");
    CHECK(access(h, F_OK) != 0 && access(h2, F_OK) != 0, "names still visible");
    memset(buf, 0, sizeof buf);
    CHECK(pread(fd, buf, 9, 0) == 9 && !memcmp(buf, "hard-more", 9), "read via fd after last unlink: %s", strerror(errno));
    close(fd);
    /* link to a symlink: plain link() does not follow */
    char sl[300], sh[300]; snprintf(sl, sizeof sl, "%s/h_sym", b); snprintf(sh, sizeof sh, "%s/h_symh", b);
    CHECKE(symlink("nowhere", sl) == 0, "symlink");
    CHECKE(link(sl, sh) == 0, "link to a dangling symlink");
    struct stat ls; CHECK(lstat(sh, &ls) == 0 && S_ISLNK(ls.st_mode) && ls.st_nlink == 2, "hard link of symlink: mode %o nlink %lu", ls.st_mode, (unsigned long)ls.st_nlink);
    unlink(sl); unlink(sh);
    R(PASS, "same ino, nlink 2->3->2, shared data, EEXIST, dir EPERM, rename-same-inode no-op, data lives on via open fd, link of symlink");
}
static int lk_perm(const char *b, int owner, char *d, size_t n) {
    char f[300], l[300], p[300]; struct stat st;
    snprintf(f, sizeof f, "%s/p_f", b); snprintf(l, sizeof l, "%s/p_l", b); snprintf(p, sizeof p, "%s/p_d", b);
    mode_t om = umask(022);
    int fd = open(f, O_WRONLY | O_CREAT | O_EXCL, 0777); CHECKE(fd >= 0, "create 0777");
    CHECK(fstat(fd, &st) == 0 && (st.st_mode & 07777) == 0755, "create mode: %o (want 0755 after umask 022)", st.st_mode & 07777);
    CHECKE(fchmod(fd, 0604) == 0, "fchmod");
    close(fd);
    CHECK(stat(f, &st) == 0 && (st.st_mode & 07777) == 0604, "after fchmod: %o", st.st_mode & 07777);
    CHECKE(chmod(f, 04755) == 0, "chmod 04755");
    CHECK(stat(f, &st) == 0 && (st.st_mode & 07777) == 04755, "after chmod: %o", st.st_mode & 07777);
    CHECK(access(f, X_OK) == 0, "X_OK after chmod 755");
    CHECKE(chmod(f, 0644) == 0, "chmod 0644");
    CHECK(access(f, X_OK) == -1, "X_OK on 0644 should fail");
    CHECKE(mkdir(p, 0700) == 0, "mkdir 0700");
    CHECK(stat(p, &st) == 0 && (st.st_mode & 07777) == 0700, "mkdir mode %o", st.st_mode & 07777);
    CHECKE(symlink("p_f", l) == 0, "symlink");
    CHECKE(chmod(l, 0600) == 0, "chmod via link");
    CHECK(stat(f, &st) == 0 && (st.st_mode & 07777) == 0600, "chmod via link did not reach target: %o", st.st_mode & 07777);
    struct stat ls; CHECK(lstat(l, &ls) == 0 && (ls.st_mode & 0777) == 0777, "link mode changed: %o", ls.st_mode);
    if (owner) {
        CHECKE(chmod(f, 06755) == 0, "chmod 06755");
        CHECKE(chown(f, 1234, 4321) == 0, "chown");
        CHECK(stat(f, &st) == 0 && st.st_uid == 1234 && st.st_gid == 4321, "chown: uid %u gid %u", st.st_uid, st.st_gid);
        CHECK((st.st_mode & 07777) == 0755, "chown should clear setuid/setgid: %o", st.st_mode & 07777);
        CHECKE(lchown(l, 77, 88) == 0, "lchown");
        CHECK(lstat(l, &ls) == 0 && ls.st_uid == 77 && ls.st_gid == 88, "lchown: link uid %u gid %u", ls.st_uid, ls.st_gid);
        CHECK(stat(f, &st) == 0 && st.st_uid == 1234, "lchown changed the target");
        fd = open(f, O_RDONLY); CHECKE(fd >= 0, "open");
        CHECKE(fchown(fd, 5, -1) == 0, "fchown");
        close(fd);
        CHECK(stat(f, &st) == 0 && st.st_uid == 5 && st.st_gid == 4321, "fchown uid 5 keep gid: %u %u", st.st_uid, st.st_gid);
        CHECKE(fchownat(AT_FDCWD, p, 9, 9, 0) == 0, "fchownat");
        CHECK(stat(p, &st) == 0 && st.st_uid == 9 && st.st_gid == 9, "fchownat dir: %u %u", st.st_uid, st.st_gid);
        CHECK(fchmodat(AT_FDCWD, l, 0600, AT_SYMLINK_NOFOLLOW) == -1 && (errno == EOPNOTSUPP || errno == ENOTSUP), "fchmodat NOFOLLOW on link: errno %d", errno);
    }
    umask(om);
    unlink(l); unlink(f); rmdir(p);
    R(PASS, "create mode+umask, fchmod/chmod, X_OK, mkdir mode, chmod via link%s", owner ? ", chown/lchown/fchown/fchownat, chown clears suid/sgid" : "");
}
/* sc.symlinks / sc.hardlinks / sc.perms in the ostest tmpdir: tmpfs or a
 * filesystem with links does the full run; FAT32/exFAT (/tmp on the gate
 * disk) must keep refusing with EPERM. */
static int lk_fat_eperm(char *d, size_t n) {
    char f[160], l[160]; tpath(f, sizeof f, "lk_f"); tpath(l, sizeof l, "lk_l");
    CHECKE(put_file(f, "x", 1) == 0, "create");
    CHECK(symlink("lk_f", l) == -1 && errno == EPERM, "symlink on FAT: errno %d (want EPERM)", errno);
    CHECK(link(f, l) == -1 && errno == EPERM, "link on FAT: errno %d (want EPERM)", errno);
    struct stat a, b2; CHECK(lstat(f, &a) == 0 && stat(f, &b2) == 0 && a.st_mode == b2.st_mode, "lstat != stat");
    char buf[16]; CHECK(readlink(f, buf, sizeof buf) == -1 && errno == EINVAL, "readlink: errno %d", errno);
    CHECKE(chmod(f, 0640) == 0, "chmod");
    CHECK(stat(f, &a) == 0 && (a.st_mode & 0777) == 0640, "chmod table: %o", a.st_mode & 0777);
    unlink(f);
    R(SKIP, "no symlink support in %s (FAT32/exFAT): EPERM for symlink/link, lstat==stat, chmod via table", tmpdir);
}
static int lk_has_links(void) {
    char f[160]; tpath(f, sizeof f, "lk_probe"); unlink(f);
    int r = symlink("x", f); unlink(f); return r == 0;
}
static int t_lk_sym(char *d, size_t n) { if (!lk_has_links()) return lk_fat_eperm(d, n); char b[200]; tpath(b, sizeof b, "lks"); CHECKE(mkdir(b, 0755) == 0, "mkdir"); int r = lk_sym(b, d, n); if (r == PASS) rmdir(b); return r; }
static int t_lk_hard(char *d, size_t n) { if (!lk_has_links()) return lk_fat_eperm(d, n); char b[200]; tpath(b, sizeof b, "lkh"); CHECKE(mkdir(b, 0755) == 0, "mkdir"); int r = lk_hard(b, d, n); if (r == PASS) rmdir(b); return r; }
static int t_lk_perm(char *d, size_t n) { if (!lk_has_links()) return lk_fat_eperm(d, n); char b[200]; tpath(b, sizeof b, "lkp"); CHECKE(mkdir(b, 0755) == 0, "mkdir"); int r = lk_perm(b, geteuid() == 0, d, n); if (r == PASS) rmdir(b); return r; }

/* ---- batch 2 item 4: small calls ---- */
static int t_sc_links(char *d, size_t n) {
    char f[160], s2[160], h[160]; tpath(f, sizeof f, "sc_lnk"); tpath(s2, sizeof s2, "sc_sym"); tpath(h, sizeof h, "sc_hard");
    CHECKE(put_file(f, "x", 1) == 0, "create"); unlink(s2); unlink(h);
    int rs = symlink(f, s2); int es = errno;
    CHECK(rs == 0 || es == EPERM, "symlink: want 0 or EPERM, got %d errno %d", rs, es);
    CHECK(symlink(f, f) == -1 && errno == EEXIST, "symlink onto existing: errno %d (want EEXIST)", errno);
    int rl = link(f, h); int el = errno;
    CHECK(rl == 0 || el == EPERM, "link: want 0 or EPERM, got %d errno %d", rl, el);
    CHECK(link(s2[0] ? "/nonexistent_sc" : f, h) == -1 && (errno == ENOENT || errno == EEXIST), "link missing src: errno %d", errno);
    unlink(s2); unlink(h); unlink(f);
    R(PASS, "symlink %s, link %s, EEXIST/ENOENT ok", rs == 0 ? "ok" : "EPERM", rl == 0 ? "ok" : "EPERM");
}
static int t_sc_fifo(char *d, size_t n) {
    char q[160]; tpath(q, sizeof q, "sc_fifo"); unlink(q);
    CHECKE(mkfifo(q, 0644) == 0, "mkfifo");
    struct stat st; CHECKE(stat(q, &st) == 0, "stat");
    CHECK(S_ISFIFO(st.st_mode), "not S_ISFIFO (mode %o)", st.st_mode);
    CHECK(open(q, O_WRONLY | O_NONBLOCK) == -1 && errno == ENXIO, "O_WRONLY|O_NONBLOCK w/o reader: errno %d (want ENXIO)", errno);
    pid_t c = fork(); CHECKE(c >= 0, "fork");
    if (c == 0) { int w = open(q, O_WRONLY); if (w < 0) _exit(2); write(w, "fifo!", 5); close(w); _exit(0); }
    int r = open(q, O_RDONLY); CHECKE(r >= 0, "open reader");
    char b[16]; int got = 0, k;
    while ((k = read(r, b + got, sizeof b - got)) > 0) got += k;
    close(r); int st2; waitpid(c, &st2, 0); unlink(q);
    CHECK(got == 5 && !memcmp(b, "fifo!", 5), "fifo read %d bytes", got);
    CHECK(WIFEXITED(st2) && WEXITSTATUS(st2) == 0, "writer child status %x", st2);
    R(PASS, "mkfifo + blocking open pairing + EOF");
}
static int t_sc_flock(char *d, size_t n) {
    char f[160]; tpath(f, sizeof f, "sc_flock"); CHECKE(put_file(f, "x", 1) == 0, "create");
    int a = open(f, O_RDWR), b = open(f, O_RDWR); CHECKE(a >= 0 && b >= 0, "open");
    CHECKE(flock(a, LOCK_EX) == 0, "LOCK_EX");
    CHECK(flock(b, LOCK_EX | LOCK_NB) == -1 && errno == EWOULDBLOCK, "2nd EX|NB: errno %d", errno);
    pid_t c = fork(); CHECKE(c >= 0, "fork");
    if (c == 0) _exit(flock(b, LOCK_EX) == 0 ? 0 : 1);      /* blocks until parent unlocks */
    usleep(100000);
    CHECKE(flock(a, LOCK_UN) == 0, "LOCK_UN");
    int st; waitpid(c, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "blocked child did not get the lock");
    close(b);                                                  /* last ref: releases */
    CHECK(flock(a, LOCK_EX | LOCK_NB) == 0, "lock not released on close: errno %d", errno);
    close(a); unlink(f);
    R(PASS, "EX conflict, blocking wait, release on close");
}
static int t_sc_fallocate(char *d, size_t n) {
    char f[160]; tpath(f, sizeof f, "sc_falloc"); CHECKE(put_file(f, "abc", 3) == 0, "create");
    int a = open(f, O_RDWR); CHECKE(a >= 0, "open");
    CHECKE(fallocate(a, 0, 0, 20000) == 0, "fallocate");
    struct stat st; fstat(a, &st);
    CHECK(st.st_size == 20000, "size %ld (want 20000)", (long)st.st_size);
    CHECK(fallocate(a, 1, 0, 90000) == 0 && fstat(a, &st) == 0 && st.st_size == 20000, "KEEP_SIZE changed size");
    struct iovec w[2] = { { "12", 2 }, { "345", 3 } };
    CHECK(pwritev(a, w, 2, 9000) == 5, "pwritev: errno %d", errno);
    char x[2], y[3]; struct iovec r[2] = { { x, 2 }, { y, 3 } };
    CHECK(preadv(a, r, 2, 9000) == 5 && !memcmp(x, "12", 2) && !memcmp(y, "345", 3), "preadv mismatch");
    CHECK(lseek(a, 0, SEEK_CUR) == 0, "offset moved");
    CHECK(posix_fadvise(a, 0, 0, POSIX_FADV_WILLNEED) == 0, "fadvise");
    char h[3]; CHECK(pread(a, h, 3, 0) == 3 && !memcmp(h, "abc", 3), "head lost");
    close(a); unlink(f);
    R(PASS, "fallocate grow/KEEP_SIZE, preadv/pwritev, fadvise");
}
static int t_sc_times(char *d, size_t n) {
    char f[160]; tpath(f, sizeof f, "sc_times"); CHECKE(put_file(f, "x", 1) == 0, "create");
    struct timeval tv[2] = { { 1000, 0 }, { 2000, 0 } };
    CHECKE(utimes(f, tv) == 0, "utimes");
    CHECKE(utimensat(AT_FDCWD, f, NULL, 0) == 0, "utimensat");
    unlink(f);
    CHECK(utimes(f, tv) == -1 && errno == ENOENT, "utimes on missing: errno %d", errno);
    R(PASS, "utimes/utimensat accepted, ENOENT on missing");
}
static int t_sc_waitid(char *d, size_t n) {
    pid_t c = fork(); CHECKE(c >= 0, "fork");
    if (c == 0) _exit(5);
    siginfo_t si; memset(&si, 0, sizeof si);
    CHECKE(waitid(P_PID, c, &si, WEXITED | WNOWAIT) == 0, "waitid WNOWAIT");
    CHECK(si.si_pid == c && si.si_code == CLD_EXITED && si.si_status == 5, "WNOWAIT info pid %d code %d st %d", si.si_pid, si.si_code, si.si_status);
    memset(&si, 0, sizeof si);
    CHECKE(waitid(P_PID, c, &si, WEXITED) == 0, "waitid");
    CHECK(si.si_pid == c && si.si_status == 5 && si.si_signo == SIGCHLD, "info pid %d st %d", si.si_pid, si.si_status);
    c = fork(); CHECKE(c >= 0, "fork2");
    if (c == 0) { pause(); _exit(0); }
    memset(&si, 0, sizeof si);
    CHECK(waitid(P_PID, c, &si, WEXITED | WNOHANG) == 0 && si.si_pid == 0, "WNOHANG on running child");
    kill(c, SIGKILL);
    CHECKE(waitid(P_PID, c, &si, WEXITED) == 0, "waitid killed");
    CHECK(si.si_code == CLD_KILLED && si.si_status == SIGKILL, "killed: code %d st %d", si.si_code, si.si_status);
    R(PASS, "waitid exited/killed/WNOWAIT/WNOHANG");
}
static int t_sc_sched(char *d, size_t n) {
    CHECKE(setpriority(PRIO_PROCESS, 0, 7) == 0, "setpriority");
    errno = 0; int v = getpriority(PRIO_PROCESS, 0);
    CHECK(v == 7 && errno == 0, "getpriority %d", v);
    setpriority(PRIO_PROCESS, 0, 0);
    CHECK(getpriority(PRIO_PROCESS, 999999) == -1 && errno == ESRCH, "bad pid: errno %d", errno);
    cpu_set_t cs; CPU_ZERO(&cs);
    CHECKE(sched_getaffinity(0, sizeof cs, &cs) == 0, "getaffinity");
    CHECK(CPU_ISSET(0, &cs), "cpu0 not in mask");
    CPU_ZERO(&cs); CPU_SET(0, &cs);
    CHECKE(sched_setaffinity(0, sizeof cs, &cs) == 0, "setaffinity");
    CPU_ZERO(&cs);
    CHECK(sched_setaffinity(0, sizeof cs, &cs) == -1 && errno == EINVAL, "empty mask: errno %d", errno);
    R(PASS, "get/setpriority, get/setaffinity");
}
static volatile sig_atomic_t got_sig;
static void on_sig(int s) { got_sig = s; }
static int child_fault(void (*fn)(void *), void *arg) {
    pid_t c = fork(); if (c < 0) return -1;
    if (c == 0) { signal(SIGSEGV, SIG_DFL); signal(SIGBUS, SIG_DFL); fn(arg); _exit(0); }
    int st; waitpid(c, &st, 0);
    return WIFSIGNALED(st) ? WTERMSIG(st) : 1000 + WEXITSTATUS(st);
}
static void wr_byte(void *p) { *(volatile char *)p = 1; }
static void rd_byte(void *p) { (void)*(volatile char *)p; }
static int t_munmap(char *d, size_t n) {
    char *p = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); CHECKE(p != MAP_FAILED, "mmap");
    p[0] = 1; p[4096] = 2; CHECKE(munmap(p + 4096, 4096) == 0, "munmap 2nd page");
    CHECK(p[0] == 1, "first page lost");
    int s = child_fault(rd_byte, p + 4096);
    CHECK(s == SIGSEGV, "access after munmap: %s %d (want SIGSEGV)", s >= 1000 ? "exit" : "signal", s >= 1000 ? s - 1000 : s);
    R(PASS, "partial munmap, access -> SIGSEGV");
}
static int t_mprotect(char *d, size_t n) {
    char *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); CHECKE(p != MAP_FAILED, "mmap");
    p[0] = 9;
    if (mprotect(p, 4096, PROT_READ) != 0) R(SKIP, "mprotect: %s", strerror(errno));
    CHECK(p[0] == 9, "read after mprotect");
    int s = child_fault(wr_byte, p);
    CHECK(s == SIGSEGV, "write to PROT_READ page: %s %d (want SIGSEGV)", s >= 1000 ? "exit" : "signal", s >= 1000 ? s - 1000 : s);
    CHECKE(mprotect(p, 4096, PROT_READ | PROT_WRITE) == 0, "mprotect rw");
    p[0] = 10;
    R(PASS, "RO write -> SIGSEGV, back to RW ok");
}
static int t_brk(char *d, size_t n) {
    char *a = sbrk(0); CHECK(a != (void *)-1, "sbrk(0)");
    char *b = sbrk(1 << 20); if (b == (void *)-1) R(SKIP, "sbrk grow: %s (musl malloc does not need brk)", strerror(errno));
    memset(b, 0x33, 1 << 20);
    CHECK(sbrk(0) == b + (1 << 20), "break not advanced");
    CHECK(sbrk(-(1 << 20)) != (void *)-1, "sbrk shrink");
    R(PASS, "grow 1MB, write, shrink");
}
static int t_cow(char *d, size_t n) {
    size_t sz = 1 << 20; uint8_t *p = malloc(sz); CHECK(p, "malloc"); memset(p, 0x11, sz);
    int pf[2]; pipe(pf);
    pid_t c = fork(); CHECKE(c >= 0, "fork");
    if (c == 0) { memset(p, 0x22, sz); uint32_t h = fnv(p, sz, 2166136261u); write(pf[1], &h, 4); _exit(0); }
    uint32_t hc = 0; read(pf[0], &hc, 4); waitpid(c, NULL, 0);
    for (size_t i = 0; i < sz; i += 512) CHECK(p[i] == 0x11, "parent memory changed by child at %zu", i);
    memset(p, 0x22, sz);
    CHECK(hc == fnv(p, sz, 2166136261u), "child saw wrong data");
    free(p);
    R(PASS, "1MB private after fork");
}
static int t_mem_exhaust(char *d, size_t n) {
    size_t total = 0;
    for (int i = 0; i < 4096; i++) { void *p = mmap(NULL, 16u << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); if (p == MAP_FAILED) break; memset(p, 1, 16u << 20); total += 16u << 20; }
    R(PASS, "got %zu MB before mmap failed (freed at exit)", total >> 20);
}

/* ===================== proc ===================== */
static int t_exit_codes(char *d, size_t n) {
    int codes[] = { 0, 1, 42, 255 };
    for (int i = 0; i < 4; i++) {
        pid_t c = fork(); CHECKE(c >= 0, "fork");
        if (c == 0) _exit(codes[i]);
        int st; CHECK(waitpid(c, &st, 0) == c, "waitpid");
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == codes[i], "exit %d -> status 0x%x", codes[i], st);
    }
    pid_t c = fork(); if (c == 0) { for (;;) pause(); }
    msleep(20); kill(c, SIGKILL); int st; waitpid(c, &st, 0);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL, "SIGKILL -> status 0x%x", st);
    R(PASS, "0/1/42/255 + SIGKILL status");
}
/* doom1.wad's lump directory is the last 20 KB of a 4 MB file on the boot
 * volume. Reading it the way w_wad.c does (lseek far into the file, one big
 * read) used to return a full byte count with the wrong clusters' data when
 * the FAT cache could not evict a dirty slot (boot CD), so seeks stopped on
 * the wrong cluster. Compare against a sequential read of the whole file. */
static int t_fs_seek_far_read(char *d, size_t n) {
    const char *path = "/doom1.wad";
    int fd = open(path, O_RDONLY);
    if (fd < 0) R(SKIP, "%s missing", path);
    unsigned char hdr[12];
    CHECK(read(fd, hdr, 12) == 12 && memcmp(hdr + 1, "WAD", 3) == 0, "bad WAD header");
    uint32_t num, ofs; memcpy(&num, hdr + 4, 4); memcpy(&ofs, hdr + 8, 4);
    struct stat st; CHECK(fstat(fd, &st) == 0, "fstat");
    size_t len = (size_t)num * 16;
    CHECK(num > 0 && num < 100000 && (off_t)ofs + (off_t)len <= st.st_size, "num %u ofs %u size %lld", num, ofs, (long long)st.st_size);
    unsigned char *ref = malloc(len), *buf = malloc(len), *chunk = malloc(65536);
    CHECK(ref && buf && chunk, "malloc");
    /* reference: sequential 64 KiB reads from offset 0 */
    CHECK(lseek(fd, 0, SEEK_SET) == 0, "lseek 0");
    off_t pos = 0; size_t got = 0; ssize_t r;
    while ((r = read(fd, chunk, 65536)) > 0) {
        for (ssize_t i = 0; i < r; i++) {
            off_t p = pos + i;
            if (p >= (off_t)ofs && p < (off_t)ofs + (off_t)len) { ref[p - ofs] = chunk[i]; got++; }
        }
        pos += r;
    }
    if (got != len || pos != st.st_size) { free(ref); free(buf); free(chunk); close(fd); R(FAIL, "sequential read got %lld of %lld bytes", (long long)pos, (long long)st.st_size); }
    int bad = 0;
    for (uint32_t i = 0; i < num; i++) {
        const unsigned char *nm = ref + i * 16 + 8;
        if (!nm[0]) { bad++; continue; }
        for (int k = 0; k < 8 && nm[k]; k++) if (nm[k] < 0x20 || nm[k] > 0x7e) { bad++; break; }
    }
    if (bad) { free(ref); free(buf); free(chunk); close(fd); R(FAIL, "sequential read: %d bad lump names", bad); }
    /* w_wad.c pattern, repeated; seek back to the start in between so each
     * far seek walks the whole cluster chain again */
    for (int pass = 0; pass < 8; pass++) {
        if (lseek(fd, 0, SEEK_SET) != 0 || read(fd, hdr, 12) != 12) { free(ref); free(buf); free(chunk); close(fd); R(FAIL, "pass %d: header re-read", pass); }
        off_t o = lseek(fd, ofs, SEEK_SET);
        ssize_t rr = o == (off_t)ofs ? read(fd, buf, len) : -1;
        if (rr != (ssize_t)len) { free(ref); free(buf); free(chunk); close(fd); R(FAIL, "pass %d: lseek %lld read %zd of %zu", pass, (long long)o, rr, len); }
        if (memcmp(buf, ref, len) != 0) {
            size_t k = 0; while (k < len && buf[k] == ref[k]) k++;
            free(ref); free(buf); free(chunk); close(fd);
            R(FAIL, "pass %d: directory differs from sequential read at byte %zu", pass, k);
        }
    }
    free(ref); free(buf); free(chunk); close(fd);
    R(PASS, "%u-entry directory at %u: 8 far seek+reads match sequential read", num, ofs);
}
static int t_exec_busybox(char *d, size_t n) {
    if (access("/bin/busybox.exe", F_OK)) R(SKIP, "/bin/busybox.exe missing");
    char out[256]; char *av[] = { "/bin/busybox.exe", "echo", "hello", "ostest", NULL };
    int r = run_capture(av, out, sizeof(out), 5000);
    CHECK(r == 0, "exit %d out='%.60s'", r, out);
    CHECK(strcmp(out, "hello ostest\n") == 0, "output '%.60s'", out);
    char *av2[] = { "/bin/busybox.exe", "sh", "-c", "exit 7", NULL };
    r = run_capture(av2, out, sizeof(out), 5000);
    CHECK(r == 7, "sh -c 'exit 7' -> %d", r);
    char *av3[] = { "/bin/nonexistent.exe", NULL };
    r = run_capture(av3, out, sizeof(out), 5000);
    CHECK(r == 127, "exec of missing file -> %d (want 127 from child)", r);
    R(PASS, "echo + exit status + ENOENT");
}
static int t_argv_env(char *d, size_t n) {
    int pf[2]; pipe(pf);
    pid_t c = fork(); CHECKE(c >= 0, "fork");
    if (c == 0) {
        dup2(pf[1], 1); close(pf[0]);
        char *av[] = { (char *)self_path, "--child-argv", "a b", "", "x=y", NULL };
        char *ev[] = { "OSTEST_ENV=hello world", "PATH=/bin", NULL };
        execve(self_path, av, ev); _exit(127);
    }
    close(pf[1]);
    char out[512] = ""; readall(pf[0], out, sizeof(out) - 1); close(pf[0]);
    int st; waitpid(c, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 7, "child status 0x%x (exec %s)", st, self_path);
    CHECK(strcmp(out, "argc=5|a b||x=y|env=hello world\n") == 0, "got '%.100s'", out);
    R(PASS, "argv with spaces/empty + env passed");
}
static int t_many_children(char *d, size_t n) {
    int N = 64, ok = 0; pid_t pids[64];
    for (int i = 0; i < N; i++) { pids[i] = fork(); CHECK(pids[i] >= 0, "fork %d: %s", i, strerror(errno)); if (pids[i] == 0) { msleep(10 + (i % 5) * 10); _exit(i); } }
    for (int i = 0; i < N; i++) { int st; pid_t w = waitpid(pids[i], &st, 0); if (w == pids[i] && WIFEXITED(st) && WEXITSTATUS(st) == i) ok++; }
    CHECK(ok == N, "%d/%d children returned the right status", ok, N);
    R(PASS, "%d concurrent children", N);
}
static int t_zombies(char *d, size_t n) {
    for (int i = 0; i < 20; i++) { pid_t c = fork(); CHECK(c >= 0, "fork"); if (c == 0) _exit(0); }
    msleep(200);
    int got = 0; while (waitpid(-1, NULL, WNOHANG) > 0) got++;
    while (got < 20) { if (wait(NULL) < 0) break; got++; }
    CHECK(got == 20, "reaped %d/20", got);
    errno = 0; CHECK(wait(NULL) == -1 && errno == ECHILD, "wait with no children: errno %d (want ECHILD)", errno);
    pid_t c = fork(); if (c == 0) { if (fork() == 0) { msleep(100); _exit(0); } _exit(0); }
    waitpid(c, NULL, 0);
    R(PASS, "20 zombies reaped via WNOHANG/wait, ECHILD after, orphan ok");
}
static int t_vfork(char *d, size_t n) {
    pid_t c = vfork(); CHECKE(c >= 0, "vfork");
    if (c == 0) _exit(3);
    int st; waitpid(c, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 3, "status 0x%x", st);
    R(PASS, "vfork+_exit");
}

/* ===================== sig ===================== */
static int t_sig_kill(char *d, size_t n) {
    struct sigaction sa = {0}; sa.sa_handler = on_sig; sigaction(SIGUSR1, &sa, NULL);
    got_sig = 0; kill(getpid(), SIGUSR1);
    for (int i = 0; i < 100 && !got_sig; i++) msleep(1);
    CHECK(got_sig == SIGUSR1, "handler not run (got %d)", got_sig);
    got_sig = 0; raise(SIGUSR1); CHECK(got_sig == SIGUSR1, "raise");
    pid_t c = fork(); if (c == 0) { got_sig = 0; while (!got_sig) pause(); _exit(got_sig == SIGUSR1 ? 0 : 1); }
    msleep(50); kill(c, SIGUSR1); int st; waitpid(c, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "child handler status 0x%x", st);
    R(PASS, "self, raise, other process");
}
static int t_sigchld(char *d, size_t n) {
    struct sigaction sa = {0}; sa.sa_handler = on_sig; sigaction(SIGCHLD, &sa, NULL);
    got_sig = 0; pid_t c = fork(); if (c == 0) _exit(0);
    for (int i = 0; i < 500 && !got_sig; i++) msleep(2);
    waitpid(c, NULL, 0);
    CHECK(got_sig == SIGCHLD, "no SIGCHLD");
    R(PASS, "delivered");
}
static int t_sigpipe(char *d, size_t n) {
    int pf[2]; pipe(pf); close(pf[0]);
    signal(SIGPIPE, SIG_IGN); errno = 0;
    CHECK(write(pf[1], "x", 1) == -1 && errno == EPIPE, "write to closed pipe with SIG_IGN: errno %d", errno);
    close(pf[1]);
    pid_t c = fork(); if (c == 0) { signal(SIGPIPE, SIG_DFL); int q[2]; pipe(q); close(q[0]); write(q[1], "x", 1); _exit(0); }
    int st; waitpid(c, &st, 0);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGPIPE, "default action: status 0x%x", st);
    R(PASS, "EPIPE when ignored, killed by default");
}
static int t_sigalrm(char *d, size_t n) {
    struct sigaction sa = {0}; sa.sa_handler = on_sig; sigaction(SIGALRM, &sa, NULL);
    got_sig = 0; long long t0 = mono_ms(); alarm(1);
    while (!got_sig && mono_ms() - t0 < 3000) pause();
    long long el = mono_ms() - t0;
    CHECK(got_sig == SIGALRM, "no SIGALRM after %lldms", el);
    CHECK(el >= 900 && el < 2000, "alarm(1) took %lldms", el);
    struct itimerval it = { {0, 0}, {0, 200000} }; got_sig = 0; t0 = mono_ms();
    if (setitimer(ITIMER_REAL, &it, NULL) == 0) { while (!got_sig && mono_ms() - t0 < 2000) pause(); CHECK(got_sig == SIGALRM, "setitimer 200ms: no signal"); }
    alarm(30);
    R(PASS, "alarm(1) %lldms, setitimer %lldms", el, mono_ms() - t0);
}
static int t_sigmask(char *d, size_t n) {
    struct sigaction sa = {0}; sa.sa_handler = on_sig; sigaction(SIGUSR2, &sa, NULL);
    sigset_t s, old, pend; sigemptyset(&s); sigaddset(&s, SIGUSR2);
    CHECKE(sigprocmask(SIG_BLOCK, &s, &old) == 0, "sigprocmask");
    got_sig = 0; kill(getpid(), SIGUSR2); msleep(20);
    CHECK(got_sig == 0, "delivered while blocked");
    sigpending(&pend); CHECK(sigismember(&pend, SIGUSR2), "not pending");
    sigprocmask(SIG_SETMASK, &old, NULL);
    for (int i = 0; i < 100 && !got_sig; i++) msleep(1);
    CHECK(got_sig == SIGUSR2, "not delivered after unblock");
    sigprocmask(SIG_BLOCK, &s, NULL); got_sig = 0;
    pid_t c = fork(); if (c == 0) { msleep(50); kill(getppid(), SIGUSR2); _exit(0); }
    sigset_t w; sigemptyset(&w); sigsuspend(&w); waitpid(c, NULL, 0);
    CHECK(got_sig == SIGUSR2, "sigsuspend did not return with SIGUSR2");
    R(PASS, "block/pending/unblock/sigsuspend");
}
static int t_sigint_pgrp(char *d, size_t n) {
    pid_t leader = fork(); CHECKE(leader >= 0, "fork");
    if (leader == 0) {
        setpgid(0, 0);
        for (int i = 0; i < 2; i++) if (fork() == 0) { for (;;) pause(); }
        for (;;) pause();
    }
    setpgid(leader, leader); msleep(150);
    CHECKE(kill(-leader, SIGINT) == 0, "kill(-pgid, SIGINT)");
    int st = 0; long long t0 = mono_ms(); pid_t w;
    while ((w = waitpid(leader, &st, WNOHANG)) == 0 && mono_ms() - t0 < 2000) msleep(5);
    CHECK(w == leader && WIFSIGNALED(st) && WTERMSIG(st) == SIGINT, "leader status 0x%x", w == leader ? st : -1);
    msleep(100);
    if (kill(-leader, 0) == 0) { kill(-leader, SIGKILL); R(FAIL, "group members survived SIGINT"); }
    R(PASS, "leader + 2 members killed by SIGINT");
}
static int t_eintr(char *d, size_t n) {
    struct sigaction sa = {0}; sa.sa_handler = on_sig; sigaction(SIGUSR1, &sa, NULL);
    int pf[2]; pipe(pf);
    pid_t c = fork(); if (c == 0) { msleep(50); kill(getppid(), SIGUSR1); msleep(50); write(pf[1], "z", 1); _exit(0); }
    char ch; errno = 0; got_sig = 0;
    ssize_t r = read(pf[0], &ch, 1);
    int e = errno; waitpid(c, NULL, 0);
    CHECK(got_sig == SIGUSR1, "handler not run");
    CHECK(r == -1 && e == EINTR, "blocking read without SA_RESTART returned %zd errno %d (want -1 EINTR)", r, e);
    R(PASS, "blocking read -> EINTR");
}

/* ===================== pipe ===================== */
static int t_pipe_basic(char *d, size_t n) {
    int pf[2]; CHECKE(pipe(pf) == 0, "pipe");
    CHECK(write(pf[1], "hello", 5) == 5, "write");
    char b[8] = ""; CHECK(read(pf[0], b, 8) == 5 && !memcmp(b, "hello", 5), "read");
    close(pf[1]); CHECK(read(pf[0], b, 8) == 0, "EOF after writer close");
    close(pf[0]);
    R(PASS, "write/read/EOF");
}
static int t_pipe_block(char *d, size_t n) {
    int pf[2]; pipe(pf);
    pid_t c = fork(); if (c == 0) { close(pf[0]); msleep(200); write(pf[1], "late", 4); _exit(0); }
    close(pf[1]);
    long long t0 = mono_ms(); char b[8]; ssize_t r = read(pf[0], b, 8); long long el = mono_ms() - t0;
    waitpid(c, NULL, 0);
    CHECK(r == 4 && el >= 150, "read returned %zd after %lldms", r, el);
    R(PASS, "reader blocked %lldms", el);
}
static int t_pipe_large(char *d, size_t n) {
    size_t sz = 8u << 20; int pf[2]; pipe(pf);
    pid_t c = fork();
    if (c == 0) { close(pf[0]); uint8_t *b = malloc(65536); uint32_t seed = 1; for (size_t o = 0; o < sz; o += 65536) { fill(b, 65536, seed++); if (writeall(pf[1], b, 65536) != 65536) _exit(1); } _exit(0); }
    close(pf[1]);
    uint8_t *b = malloc(65536), *e = malloc(65536); size_t got = 0; uint32_t seed = 1; long long t0 = mono_ms();
    while (got < sz) { ssize_t r = readall(pf[0], b, 65536); if (r != 65536) break; fill(e, 65536, seed++); CHECK(!memcmp(b, e, 65536), "data mismatch at %zu", got); got += r; }
    long long el = mono_ms() - t0; int st; waitpid(c, &st, 0); close(pf[0]);
    CHECK(got == sz, "got %zu of %zu", got, sz);
    R(PASS, "8MB verified, %lld KB/s", el ? (long long)(sz / 1024) * 1000 / el : 0);
}
static int t_pipe_concurrent(char *d, size_t n) {
    int pf[2]; pipe(pf); int W = 4, M = 500;
    for (int w = 0; w < W; w++) if (fork() == 0) { close(pf[0]); char m[64]; memset(m, 'A' + w, 64); for (int i = 0; i < M; i++) if (write(pf[1], m, 64) != 64) _exit(1); _exit(0); }
    close(pf[1]);
    long cnt[4] = {0}; char b[64]; int torn = 0;
    while (readall(pf[0], b, 64) == 64) { int k = b[0] - 'A'; for (int i = 1; i < 64; i++) if (b[i] != b[0]) torn++; if (k >= 0 && k < 4) cnt[k]++; }
    for (int w = 0; w < W; w++) wait(NULL);
    close(pf[0]);
    CHECK(!torn && cnt[0] == M && cnt[1] == M && cnt[2] == M && cnt[3] == M, "counts %ld %ld %ld %ld torn=%d", cnt[0], cnt[1], cnt[2], cnt[3], torn);
    R(PASS, "4 writers x %d atomic 64-byte writes", M);
}
static int t_pipe_nonblock(char *d, size_t n) {
    int pf[2]; CHECKE(pipe2(pf, O_NONBLOCK) == 0, "pipe2 O_NONBLOCK");
    char b[4096]; errno = 0;
    CHECK(read(pf[0], b, 1) == -1 && errno == EAGAIN, "empty read: errno %d", errno);
    memset(b, 1, sizeof(b)); size_t tot = 0; ssize_t r;
    while ((r = write(pf[1], b, sizeof(b))) > 0) { tot += r; if (tot > (64u << 20)) break; }
    CHECK(r == -1 && errno == EAGAIN, "full pipe write: r=%zd errno %d after %zu bytes", r, errno, tot);
    close(pf[0]); close(pf[1]);
    R(PASS, "EAGAIN empty + full (capacity %zu)", tot);
}

/* ===================== fs (root disk, under /tmp/ostest.*) ===================== */
static int t_fs_rw(char *d, size_t n) {
    char p[160]; tpath(p, sizeof(p), "a.txt");
    int fd = open(p, O_RDWR | O_CREAT | O_EXCL, 0644); CHECKE(fd >= 0, "create");
    CHECK(write(fd, "0123456789", 10) == 10, "write");
    CHECK(lseek(fd, 0, SEEK_SET) == 0, "lseek set");
    char b[32] = ""; CHECK(read(fd, b, 32) == 10 && !memcmp(b, "0123456789", 10), "read back '%s'", b);
    CHECK(lseek(fd, -3, SEEK_END) == 7, "SEEK_END");
    CHECK(read(fd, b, 3) == 3 && !memcmp(b, "789", 3), "read at end-3");
    CHECK(lseek(fd, -5, SEEK_CUR) == 5, "SEEK_CUR");
    CHECK(write(fd, "XY", 2) == 2, "overwrite");
    CHECK(pwrite(fd, "P", 1, 0) == 1, "pwrite");
    memset(b, 0, sizeof(b)); CHECK(pread(fd, b, 10, 0) == 10 && !memcmp(b, "P1234XY789", 10), "pread '%s'", b);
    close(fd);
    errno = 0; CHECK(open(p, O_CREAT | O_EXCL | O_WRONLY, 0644) < 0 && errno == EEXIST, "O_EXCL on existing: errno %d", errno);
    R(PASS, "create/write/seek/pread/pwrite/O_EXCL");
}
static int t_fs_append(char *d, size_t n) {
    char p[160]; tpath(p, sizeof(p), "app.txt"); put_file(p, "abc", 3);
    int fd = open(p, O_WRONLY | O_APPEND); CHECKE(fd >= 0, "open append");
    lseek(fd, 0, SEEK_SET); write(fd, "def", 3); close(fd);
    fd = open(p, O_WRONLY | O_APPEND); write(fd, "g", 1); close(fd);
    char b[16] = ""; ssize_t r = get_file(p, b, 16);
    CHECK(r == 7 && !memcmp(b, "abcdefg", 7), "got %zd '%.16s'", r, b);
    R(PASS, "O_APPEND ignores lseek");
}
static int t_fs_truncate(char *d, size_t n) {
    char p[160]; tpath(p, sizeof(p), "tr.bin"); static char z[10000]; memset(z, 'x', sizeof(z)); put_file(p, z, sizeof(z));
    CHECKE(truncate(p, 3000) == 0, "truncate shrink");
    struct stat st; stat(p, &st); CHECK(st.st_size == 3000, "size %lld after shrink", (long long)st.st_size);
    CHECKE(truncate(p, 20000) == 0, "truncate grow");
    stat(p, &st); CHECK(st.st_size == 20000, "size %lld after grow", (long long)st.st_size);
    static char b[20000]; ssize_t r = get_file(p, b, sizeof(b)); CHECK(r == 20000, "read grown file: %zd", r);
    for (int i = 3000; i < 20000; i++) CHECK(b[i] == 0, "grown area not zero at %d (0x%02x)", i, (uint8_t)b[i]);
    int fd = open(p, O_RDWR); CHECKE(ftruncate(fd, 5) == 0, "ftruncate"); fstat(fd, &st); close(fd);
    CHECK(st.st_size == 5, "fstat size %lld after ftruncate", (long long)st.st_size);
    fd = open(p, O_WRONLY | O_TRUNC); close(fd); stat(p, &st); CHECK(st.st_size == 0, "O_TRUNC size %lld", (long long)st.st_size);
    R(PASS, "shrink/grow(zero-fill)/ftruncate/O_TRUNC");
}
static int t_fs_rename(char *d, size_t n) {
    char a[160], b[160], dd[160], c[200]; tpath(a, sizeof(a), "ren_a"); tpath(b, sizeof(b), "ren_b"); tpath(dd, sizeof(dd), "rendir"); snprintf(c, sizeof(c), "%s/moved", dd);
    put_file(a, "AAA", 3); put_file(b, "BBB", 3); mkdir(dd, 0755);
    CHECKE(rename(a, b) == 0, "rename over existing");
    char t[8] = ""; CHECK(get_file(b, t, 8) == 3 && !memcmp(t, "AAA", 3), "target content '%.8s'", t);
    CHECK(access(a, F_OK) != 0, "source still exists");
    CHECKE(rename(b, c) == 0, "rename into subdir");
    CHECK(access(c, F_OK) == 0, "moved file missing");
    char d2[160]; tpath(d2, sizeof(d2), "rendir2"); CHECKE(rename(dd, d2) == 0, "rename directory");
    snprintf(c, sizeof(c), "%s/moved", d2); CHECK(access(c, F_OK) == 0, "file inside renamed dir missing");
    unlink(c); rmdir(d2);
    R(PASS, "overwrite, into dir, dir rename");
}

/* > 2048 entries in one directory: getdents is served from a per-open-directory
 * listing (no fixed cap); interleaved streams, rewinddir, every name seen once */
static int t_fs_readdir_many(char *d, size_t n) {
    char dir[160], p[300]; tpath(dir, sizeof(dir), "rdm"); mkdir(dir, 0755);
    enum { NF = 2100 };
    for (int i = 0; i < NF; i++) { snprintf(p, sizeof(p), "%s/f%04d", dir, i); int fd = open(p, O_WRONLY | O_CREAT, 0644); CHECKE(fd >= 0, "create"); close(fd); }
    static unsigned char seen[NF];
    int dup = 0, bad = 0, total = 0, other = 0;
    memset(seen, 0, sizeof(seen));
    DIR *A = opendir(dir), *B = opendir(tmpdir); CHECKE(A && B, "opendir");
    struct dirent *e;
    while ((e = readdir(A))) {
        if (e->d_name[0] == '.') continue;
        int k; if (sscanf(e->d_name, "f%d", &k) == 1 && k >= 0 && k < NF) { if (seen[k]++) dup++; total++; } else bad++;
        if ((total % 300) == 1 && readdir(B)) other++;          /* interleave a second stream */
    }
    rewinddir(A); int again = 0; while ((e = readdir(A))) if (e->d_name[0] != '.') again++;
    closedir(A); closedir(B);
    int miss = 0; for (int i = 0; i < NF; i++) if (!seen[i]) miss++;
    for (int i = 0; i < NF; i++) { snprintf(p, sizeof(p), "%s/f%04d", dir, i); unlink(p); }
    rmdir(dir);
    CHECK(total == NF && miss == 0 && dup == 0 && bad == 0, "listed %d of %d (missing %d, dup %d, foreign %d)", total, NF, miss, dup, bad);
    CHECK(again == NF, "after rewinddir %d", again);
    R(PASS, "%d entries listed once each, rewinddir %d, interleaved stream %d", total, again, other);
}
static int t_fs_dirs(char *d, size_t n) {
    char a[160], f[200]; tpath(a, sizeof(a), "d1"); CHECKE(mkdir(a, 0755) == 0, "mkdir");
    errno = 0; CHECK(mkdir(a, 0755) == -1 && errno == EEXIST, "mkdir existing: errno %d", errno);
    snprintf(f, sizeof(f), "%s/f", a); put_file(f, "x", 1);
    errno = 0; CHECK(rmdir(a) == -1 && errno == ENOTEMPTY, "rmdir non-empty: errno %d (want ENOTEMPTY)", errno);
    CHECKE(unlink(f) == 0, "unlink");
    errno = 0; CHECK(unlink(f) == -1 && errno == ENOENT, "unlink missing: errno %d", errno);
    CHECKE(rmdir(a) == 0, "rmdir empty");
    CHECK(access(a, F_OK) != 0, "dir still there");
    char deep[256]; tpath(deep, sizeof(deep), "x"); mkdir(deep, 0755);
    for (int i = 0; i < 8; i++) { strcat(deep, "/y"); CHECKE(mkdir(deep, 0755) == 0, "deep mkdir"); }
    strcat(deep, "/leaf.txt"); CHECKE(put_file(deep, "deep", 4) == 0, "deep file");
    R(PASS, "mkdir/EEXIST/ENOTEMPTY/unlink/rmdir/8-deep");
}
static int t_fs_readdir_stat(char *d, size_t n) {
    char dir[160], p[300]; tpath(dir, sizeof(dir), "rd"); mkdir(dir, 0755);
    const char *names[] = { "one.txt", "Two Words.txt", "a_very_long_file_name_for_lfn_entries_0123456789.dat", "sub" };
    for (int i = 0; i < 3; i++) { snprintf(p, sizeof(p), "%s/%s", dir, names[i]); put_file(p, names[i], strlen(names[i])); }
    snprintf(p, sizeof(p), "%s/sub", dir); mkdir(p, 0755);
    DIR *D = opendir(dir); CHECKE(D, "opendir");
    int seen = 0, dots = 0; struct dirent *e;
    while ((e = readdir(D))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) { dots++; continue; }
        for (int i = 0; i < 4; i++) if (!strcmp(e->d_name, names[i])) {
            seen |= 1 << i;
            snprintf(p, sizeof(p), "%s/%s", dir, names[i]); struct stat st; CHECKE(stat(p, &st) == 0, "stat");
            if (i == 3) CHECK(S_ISDIR(st.st_mode), "sub not S_ISDIR (mode %o)", st.st_mode);
            else { CHECK(S_ISREG(st.st_mode), "%s not S_ISREG", names[i]); CHECK(st.st_size == (off_t)strlen(names[i]), "%s size %lld", names[i], (long long)st.st_size); }
            if (e->d_type != DT_UNKNOWN) CHECK(e->d_type == (i == 3 ? DT_DIR : DT_REG), "d_type %d for %s", e->d_type, names[i]);
        }
    }
    closedir(D);
    CHECK(seen == 15, "missing entries (mask %x)", seen);
    struct stat st; errno = 0; CHECK(stat("/nonexistent/zz", &st) == -1 && errno == ENOENT, "stat missing errno %d", errno);
    struct statfs sf; CHECKE(statfs(tmpdir, &sf) == 0, "statfs");
    R(PASS, "readdir+stat+d_type ok%s", dots ? "" : " (no ./.. entries)");
}
static int t_fs_many(char *d, size_t n) {
    char dir[160], p[220]; tpath(dir, sizeof(dir), "many"); mkdir(dir, 0755);
    int N = 300;
    for (int i = 0; i < N; i++) { snprintf(p, sizeof(p), "%s/file_%04d.txt", dir, i); char c[16]; int l = snprintf(c, sizeof(c), "%d", i); CHECK(put_file(p, c, l) == 0, "create #%d: %s", i, strerror(errno)); }
    DIR *D = opendir(dir); int cnt = 0; struct dirent *e; while ((e = readdir(D))) if (e->d_name[0] != '.') cnt++; closedir(D);
    CHECK(cnt == N, "readdir sees %d of %d", cnt, N);
    for (int i = 0; i < N; i += 37) { snprintf(p, sizeof(p), "%s/file_%04d.txt", dir, i); char c[16] = ""; get_file(p, c, 15); CHECK(atoi(c) == i, "content of #%d = '%.15s'", i, c); }
    for (int i = 0; i < N; i++) { snprintf(p, sizeof(p), "%s/file_%04d.txt", dir, i); CHECK(unlink(p) == 0, "unlink #%d: %s", i, strerror(errno)); }
    CHECKE(rmdir(dir) == 0, "rmdir after deleting all");
    R(PASS, "%d files create/list/read/delete", N);
}
static int t_fs_large(char *d, size_t n) {
    char p[160]; tpath(p, sizeof(p), "large.bin"); size_t sz = 24u << 20, ch = 1 << 20;
    uint8_t *b = malloc(ch); int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644); CHECKE(fd >= 0, "create");
    uint32_t hw = 2166136261u; long long t0 = mono_ms();
    for (size_t o = 0; o < sz; o += ch) { fill(b, ch, (uint32_t)o); hw = fnv(b, ch, hw); CHECK(writeall(fd, b, ch) == (ssize_t)ch, "write at %zu: %s", o, strerror(errno)); }
    CHECKE(fsync(fd) == 0, "fsync"); close(fd); long long tw = mono_ms() - t0;
    struct stat st; stat(p, &st); CHECK(st.st_size == (off_t)sz, "size %lld", (long long)st.st_size);
    fd = open(p, O_RDONLY); uint32_t hr = 2166136261u; t0 = mono_ms();
    for (size_t o = 0; o < sz; o += ch) { CHECK(readall(fd, b, ch) == (ssize_t)ch, "read at %zu", o); hr = fnv(b, ch, hr); }
    long long tr = mono_ms() - t0;
    lseek(fd, 13u << 20, SEEK_SET); uint8_t x[16]; read(fd, x, 16); close(fd);
    fill(b, ch, 13u << 20);
    CHECK(hr == hw, "checksum mismatch");
    CHECK(!memcmp(x, b, 16), "seek+read at 13MB wrong");
    unlink(p); free(b);
    R(PASS, "24MB: write %lldms read %lldms, checksum ok", tw, tr);
}
static int t_fs_readback_busybox(char *d, size_t n) {
    if (access("/bin/busybox.exe", F_OK)) R(SKIP, "/bin/busybox.exe missing");
    char p[160]; tpath(p, sizeof(p), "rb.txt");
    char cmd[512]; snprintf(cmd, sizeof(cmd), "busybox find /bin > %s; busybox wc -l < %s", p, p);
    char *av[] = { "/bin/busybox.exe", "sh", "-c", cmd, NULL };
    char out[256]; int r = run_capture(av, out, sizeof(out), 8000);
    CHECK(r != -2, "busybox find > f; wc -l < f: timeout (hang reproduced)");
    CHECK(r == 0 && atoi(out) > 0, "exit %d out '%.60s'", r, out);
    static char big[20000], b2[20000]; memset(big, 'q', sizeof(big)); put_file(p, big, sizeof(big));
    CHECK(get_file(p, b2, sizeof(b2)) == 20000 && !memcmp(big, b2, sizeof(big)), "in-process read-back");
    R(PASS, "busybox find>f;wc<f = %d lines; in-process ok", atoi(out));
}
static int t_fs_fd_offsets(char *d, size_t n) {
    char p[160]; tpath(p, sizeof(p), "fds.txt"); put_file(p, "abcdef", 6);
    int a = open(p, O_RDONLY), b = open(p, O_RDONLY); char x;
    read(a, &x, 1); read(a, &x, 1); read(b, &x, 1);
    CHECK(x == 'a', "independent open has own offset");
    int c = dup(a); read(c, &x, 1); CHECK(x == 'c', "dup shares offset (got %c)", x);
    pid_t k = fork(); if (k == 0) { read(a, &x, 1); _exit(x); }
    int st; waitpid(k, &st, 0); CHECK(WEXITSTATUS(st) == 'd', "fork shares offset (child read %c)", WEXITSTATUS(st));
    read(a, &x, 1); CHECK(x == 'e', "offset after child read (got %c)", x);
    close(a); close(b); close(c);
    unlink(p);
    int fd = open(p, O_RDONLY); errno = 0; CHECK(fd < 0, "open after unlink succeeded");
    R(PASS, "dup/fork share offsets, separate opens don't");
}

/* Links -dump of a page served on the host (python3 -m http.server 8080 in the
 * slirp host, 10.0.2.2; --http-port N overrides 8080).  SKIP when /bin/links or the
 * host server is absent. */
static int links_dump(const char *url, char *out, size_t cap) {
    static int seq; char f[160]; snprintf(f, sizeof(f), "%s/lk.%d.%d", tmpdir, getpid(), seq++);
    pid_t c = fork();
    if (c < 0) return -1;
    if (c == 0) {
        int fd = open(f, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
        char home[160]; snprintf(home, sizeof(home), "HOME=%s", tmpdir);
        char *av[] = { "/bin/links", "-no-connect", "-dump", (char *)url, NULL };
        char *ev[] = { home, "TERM=vt100", "PATH=/bin", NULL };
        execve(av[0], av, ev); _exit(127);
    }
    int st = 0; long long t0 = mono_ms(); pid_t w = 0;
    while ((w = waitpid(c, &st, WNOHANG)) == 0 && mono_ms() - t0 < 20000) msleep(50);
    if (w == 0) { kill(c, SIGKILL); waitpid(c, &st, 0); }
    ssize_t r = get_file(f, out, cap - 1); out[r > 0 ? r : 0] = 0; unlink(f);
    if (w == 0) return -2;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}
static int http_port = 8080;   /* --http-port N: host test server on 10.0.2.2 */
static void oneline(char *s) { for (; *s; s++) if (*s == '\n' || *s == '\r') *s = '|'; }
static int t_net_links_http(char *d, size_t n) {
    if (access("/bin/links", X_OK) && access("/bin/links", F_OK)) R(SKIP, "/bin/links missing");
    static char out[4096];
    char url[64]; snprintf(url, sizeof(url), "http://10.0.2.2:%d/", http_port);
    int r = links_dump(url, out, sizeof(out));
    if (r == -2) { oneline(out); R(FAIL, "links -dump %s timed out (20 s): '%.120s' (guest net not on slirp 10.0.2.x? run ddhcp)", url, out); }
    if (!strstr(out, "LINKS_OK")) {
        if (strstr(out, "efused") || strstr(out, "rror")) { oneline(out); R(SKIP, "no host server on 10.0.2.2:%d (rc %d: %.80s)", http_port, r, out); }
        oneline(out); R(FAIL, "rc %d, no LINKS_OK marker: '%.160s'", r, out);
    }
    char *p = strstr(out, "LINKS_OK"); char snip[64]; snprintf(snip, sizeof(snip), "%.40s", p); oneline(snip);
    /* by name through /etc/hosts (musl reads it in the resolver child) */
    static char hosts[8192]; ssize_t hl = get_file("/etc/hosts", hosts, sizeof(hosts) - 64);
    const char *byname = "not tried";
    if (hl >= 0) {
        hosts[hl] = 0; static char nh[8256];
        snprintf(nh, sizeof(nh), "%s%s10.0.2.2 testhost\n", hosts, (hl && hosts[hl - 1] != '\n') ? "\n" : "");
        if (put_file("/etc/hosts", nh, strlen(nh)) == 0) {
            snprintf(url, sizeof(url), "http://testhost:%d/", http_port);
            int r2 = links_dump(url, out, sizeof(out));
            put_file("/etc/hosts", hosts, (size_t)hl);
            byname = (r2 == 0 && strstr(out, "LINKS_OK")) ? "testhost ok" : "testhost FAILED";
            if (strcmp(byname, "testhost ok")) { oneline(out); R(FAIL, "by IP ok, by /etc/hosts name rc %d: '%.120s'", r2, out); }
        }
    }
    R(PASS, "rc %d '%s'; %s", r, snip, byname);
}

/* ===================== vol (/mnt/<x>: exFAT / ext4 / FAT) ===================== */
struct walk { int files, dirs, errors, symlinks; long long bytes; char err[200]; };
static void walk_dir(const char *dir, int depth, struct walk *w, int limit) {
    if (depth > 12 || w->files >= limit) return;
    DIR *D = opendir(dir); if (!D) { w->errors++; snprintf(w->err, sizeof(w->err), "opendir %s: %s", dir, strerror(errno)); return; }
    struct dirent *e; static uint8_t buf[65536];
    while ((e = readdir(D)) && w->files < limit) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[1024]; snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        struct stat st, lst;
        int islnk = lstat(p, &lst) == 0 && S_ISLNK(lst.st_mode);
        if (islnk) w->symlinks++;
        if (stat(p, &st) != 0 && islnk && errno == ENOENT) continue;   /* dangling link (e.g. absolute target off this volume) */
        if (stat(p, &st) != 0) { w->errors++; snprintf(w->err, sizeof(w->err), "stat %s: %s", p, strerror(errno)); continue; }
        if (S_ISDIR(st.st_mode)) { w->dirs++; walk_dir(p, depth + 1, w, limit); continue; }
        w->files++;
        int fd = open(p, O_RDONLY); if (fd < 0) { w->errors++; snprintf(w->err, sizeof(w->err), "open %s: %s", p, strerror(errno)); continue; }
        long long got = 0, cap = 4ll << 20; ssize_t r;
        while (got < cap && (r = read(fd, buf, sizeof(buf))) > 0) got += r;
        close(fd); w->bytes += got;
        long long want = st.st_size < cap ? st.st_size : cap;
        if (got < want) { w->errors++; snprintf(w->err, sizeof(w->err), "%s: read %lld of size %lld", p, got, (long long)st.st_size); }
    }
    closedir(D);
}
static int vol_list(char names[][64], int max) {
    DIR *D = opendir("/mnt"); if (!D) return 0; int k = 0; struct dirent *e;
    while ((e = readdir(D)) && k < max) { if (e->d_name[0] == '.') continue; char p[128]; snprintf(p, sizeof(p), "/mnt/%s", e->d_name); struct stat st; if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) snprintf(names[k++], 64, "%s", e->d_name); }
    closedir(D); return k;
}
static int t_vol_read(char *d, size_t n) {
    char names[8][64]; int k = vol_list(names, 8);
    if (!k) R(SKIP, "nothing mounted under /mnt");
    char res[600]; int o = 0, bad = 0; char err[200] = "";
    for (int i = 0; i < k; i++) {
        if (!strcmp(names[i], "badcs")) { o += snprintf(res + o, sizeof(res) - o, "badcs: skipped (bad-checksum fixture); "); continue; }
        char p[128]; snprintf(p, sizeof(p), "/mnt/%s", names[i]);
        struct walk w; memset(&w, 0, sizeof(w)); walk_dir(p, 0, &w, 400);
        o += snprintf(res + o, sizeof(res) - o, "%s: %d files %d dirs %d links %lldKB%s; ", names[i], w.files, w.dirs, w.symlinks, w.bytes >> 10, w.errors ? " ERR" : "");
        if (w.errors) { bad++; snprintf(err, sizeof(err), "%s", w.err); }
    }
    CHECK(!bad, "%s first error: %s", res, err);
    R(PASS, "%s", res);
}
static int t_vol_rw(char *d, size_t n) {
    char names[8][64]; int k = vol_list(names, 8); if (!k) R(SKIP, "nothing mounted under /mnt");
    char res[400] = ""; int o = 0, tested = 0;
    for (int i = 0; i < k; i++) {
        char dir[160]; snprintf(dir, sizeof(dir), "/mnt/%s/ostest.%d", names[i], getpid());
        if (mkdir(dir, 0755) != 0) { o += snprintf(res + o, sizeof(res) - o, "%s: ro (%s); ", names[i], strerror(errno)); continue; }
        tested++;
        char f[200], g[200]; snprintf(f, sizeof(f), "%s/t.bin", dir); snprintf(g, sizeof(g), "%s/u.bin", dir);
        size_t sz = 300000; uint8_t *b = malloc(sz), *c = malloc(sz); fill(b, sz, 99);
        int ok = put_file(f, b, sz) == 0 && get_file(f, c, sz) == (ssize_t)sz && !memcmp(b, c, sz);
        ok = ok && rename(f, g) == 0 && truncate(g, 1000) == 0;
        struct stat st; ok = ok && stat(g, &st) == 0 && st.st_size == 1000;
        unlink(f); int cleaned = (unlink(g) == 0 || access(g, F_OK) != 0) && rmdir(dir) == 0;
        sync(); free(b); free(c);
        o += snprintf(res + o, sizeof(res) - o, "%s: rw %s%s; ", names[i], ok ? "ok" : "FAILED", cleaned ? "" : " (cleanup failed!)");
        CHECK(ok && cleaned, "%s", res);
    }
    if (!tested) R(SKIP, "%s", res);
    R(PASS, "%s", res);
}

/* ext2/3/4 read-only: real stat() metadata and content vs the host manifest
 * (/home/user/ext_manifest.txt, written by the test-disk fixture).
 *   F path mode(octal) uid gid size mtime crc32   D path mode uid gid nentries
 *   C path size crc32 (content only)              X path (must fail: bad checksum) */
static uint32_t crc32_tab[256];
static uint32_t crc32_upd(const uint8_t *p, size_t n, uint32_t c) {
    if (!crc32_tab[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t x = i; for (int k = 0; k < 8; k++) x = (x >> 1) ^ (0xEDB88320u & (0u - (x & 1))); crc32_tab[i] = x; }
    c = ~c; while (n--) c = crc32_tab[(c ^ *p++) & 0xFF] ^ (c >> 8); return ~c;
}
static int file_crc(const char *p, uint32_t *crc, long long *len) {
    int fd = open(p, O_RDONLY); if (fd < 0) return -1;
    static uint8_t b[65536]; ssize_t r; uint32_t c = 0; long long t = 0;
    while ((r = read(fd, b, sizeof(b))) > 0) { c = crc32_upd(b, r, c); t += r; }
    close(fd); *crc = c; *len = t; return r < 0 ? -1 : 0;
}

/* ext4 write step 1 on the scratch volume /mnt/e4rw (switched read-write by
 * the harness): in-place overwrites only; the host diffs the image against
 * the same script (e4w_check.py) and runs e2fsck -fn. */
/* Shared ext4 write script (host_e4w.c and ostest vol.ext_write).
 * mode 0: at off, 1: at EOF (O_APPEND), 2: at EOF+off (leaves a hole). */
static const char *E4F[3] = { "w1.bin", "holes.bin", "keep.txt" };
static const struct { int file, mode, off, len, want; } E4OPS[] = {
    {0,0,5,100,100}, {0,0,4096,4096,4096}, {0,0,10000,3000,3000}, {0,0,19990,50,50}, {0,0,20000,5,5},
    {0,1,0,3000,3000}, {0,1,0,70000,70000},
    {0,2,9000,2000,2000}, {0,2,9000,2000,2000}, {0,2,9000,2000,2000}, {0,2,9000,2000,2000}, {0,2,9000,2000,2000}, {0,2,9000,2000,2000},
    {1,0,8192,100,100}, {1,0,4000,200,200}, {2,1,0,10,10},
};
#define E4NOPS ((int)(sizeof(E4OPS) / sizeof(E4OPS[0])))
static void e4w_pat(unsigned char *b, int n, int j) { for (int k = 0; k < n; k++) b[k] = (unsigned char)(k * 31 + j * 17 + 1); }
static int t_vol_extwrite(char *d, size_t n) {
    struct stat st;
    if (stat("/mnt/e4rw/w1.bin", &st) != 0) R(SKIP, "no /mnt/e4rw scratch volume");
    time_t t0 = time(NULL);
    static unsigned char ref[3][200000], b[80000], got[200000];
    long sz[3]; int res[64]; char rs[300]; int o = 0;
    for (int f = 0; f < 3; f++) {
        char p[64]; snprintf(p, sizeof(p), "/mnt/e4rw/%s", E4F[f]);
        CHECK(stat(p, &st) == 0, "stat %s", p); sz[f] = st.st_size;
        CHECK(get_file(p, ref[f], sizeof(ref[f])) == sz[f], "read %s", p);
    }
    int ro = open("/mnt/e4rw/w1.bin", O_RDONLY);          /* opened before the writes */
    for (int j = 0; j < E4NOPS; j++) {
        int f = E4OPS[j].file; char p[64]; snprintf(p, sizeof(p), "/mnt/e4rw/%s", E4F[f]);
        int fd = open(p, O_RDWR | (E4OPS[j].mode == 1 ? O_APPEND : 0));
        CHECK(fd >= 0, "open %s O_RDWR: %s (volume not read-write?)", p, strerror(errno));
        long off = E4OPS[j].mode == 0 ? E4OPS[j].off : E4OPS[j].mode == 1 ? sz[f] : sz[f] + E4OPS[j].off;
        if (E4OPS[j].mode == 2) CHECK(lseek(fd, E4OPS[j].off, SEEK_END) == off, "lseek END+%d", E4OPS[j].off);
        else if (E4OPS[j].mode == 0) CHECK(lseek(fd, off, SEEK_SET) == off, "lseek %ld", off);
        e4w_pat(b, E4OPS[j].len, j);
        errno = 0; ssize_t k = write(fd, b, E4OPS[j].len); res[j] = k < 0 ? -errno : (int)k;
        close(fd);
        o += snprintf(rs + o, sizeof(rs) - o, "%s%d", j ? "," : "", res[j]);
        CHECK(res[j] == E4OPS[j].want, "op%d %s off %ld len %d -> %d (%s), want %d", j, p, off, E4OPS[j].len, res[j], strerror(errno), E4OPS[j].want);
        if (k > 0) { if (off > sz[f]) memset(ref[f] + sz[f], 0, off - sz[f]); e4w_pat(ref[f] + off, (int)k, j); if (off + k > sz[f]) sz[f] = off + k; }
    }
    for (int f = 0; f < 3; f++) {
        char p[64]; snprintf(p, sizeof(p), "/mnt/e4rw/%s", E4F[f]);
        CHECK(stat(p, &st) == 0 && st.st_size == sz[f], "%s size %lld want %ld", p, (long long)st.st_size, sz[f]);
        CHECK(get_file(p, got, sizeof(got)) == sz[f] && !memcmp(got, ref[f], sz[f]), "%s readback differs", p);
    }
    CHECK(ro >= 0 && pread(ro, got, sizeof(got), 0) == sz[0] && !memcmp(got, ref[0], sz[0]), "reader opened earlier does not see the new size/data");
    close(ro);
    CHECK(stat("/mnt/e4rw/w1.bin", &st) == 0 && st.st_mtime >= t0 - 2 && st.st_mtime <= t0 + 60, "mtime %lld not updated (t0 %lld)", (long long)st.st_mtime, (long long)t0);
    int fd = open("/mnt/ext4d/tiny.txt", O_WRONLY);
    CHECK(fd < 0, "journaled ext4d opened for write");
    fd = open("/mnt/old2/sub/old.txt", O_WRONLY);
    CHECK(fd < 0, "read-only ext2 fixture opened for write");
    sync();
    R(PASS, "E4W_RES %s w1.bin %ld bytes", rs, sz[0]);
}

/* ext4 write step 3 on /mnt/e4rw (after vol.ext_write): truncate/ftruncate/
 * O_TRUNC, create (inode + dirent alloc, dir growth), unlink; htree create
 * refused.  The host replays this (e4w_check.py ... ns) and runs e2fsck. */
static int t_vol_extns(char *d, size_t n) {
    struct stat st; char nm[96]; static unsigned char b[80000], got[80000];
    if (stat("/mnt/e4rw/w1.bin", &st) != 0) R(SKIP, "no /mnt/e4rw scratch volume");
    CHECK(truncate("/mnt/e4rw/w1.bin", 50000) == 0, "truncate grow: %s", strerror(errno));
    int fd = open("/mnt/e4rw/holes.bin", O_RDWR); CHECK(fd >= 0, "open holes");
    CHECK(ftruncate(fd, 30000) == 0, "ftruncate: %s", strerror(errno));
    CHECK(fstat(fd, &st) == 0 && st.st_size == 30000, "fstat after ftruncate %lld", (long long)st.st_size);
    CHECK(pread(fd, got, 30000 - 12288, 12288) == 30000 - 12288, "pread tail");
    for (int i = 0; i < 30000 - 12288; i++) CHECK(got[i] == 0, "grown tail not zero at %d", 12288 + i);
    close(fd);
    fd = open("/mnt/e4rw/keep.txt", O_WRONLY | O_TRUNC); CHECK(fd >= 0, "O_TRUNC open: %s", strerror(errno));
    CHECK(fstat(fd, &st) == 0 && st.st_size == 0, "size after O_TRUNC %lld", (long long)st.st_size);
    CHECK(write(fd, "kept\n", 5) == 5, "write kept"); close(fd);
    CHECK(truncate("/mnt/e4rw/w1.bin", 12000) == 0, "truncate shrink: %s", strerror(errno));
    CHECK(stat("/mnt/e4rw/w1.bin", &st) == 0 && st.st_size == 12000, "w1 size %lld", (long long)st.st_size);
    static const struct { const char *n; int len, j; } NF[] = { {"new1.txt", 5000, 50}, {"new2.txt", 100, 51}, {"new3.bin", 80000, 52} };
    for (int i = 0; i < 3; i++) {
        snprintf(nm, sizeof(nm), "/mnt/e4rw/%s", NF[i].n);
        fd = open(nm, O_WRONLY | O_CREAT, 0644); CHECK(fd >= 0, "create %s: %s", nm, strerror(errno));
        e4w_pat(b, NF[i].len, NF[i].j); CHECK(write(fd, b, NF[i].len) == NF[i].len, "write %s", nm); close(fd);
    }
    CHECK(get_file("/mnt/e4rw/new1.txt", got, sizeof(got)) == 5000, "read new1");
    e4w_pat(b, 5000, 50); CHECK(!memcmp(got, b, 5000), "new1 content");
    for (int i = 0; i < 300; i++) { snprintf(nm, sizeof(nm), "/mnt/e4rw/cfile_%03d", i); fd = open(nm, O_WRONLY | O_CREAT, 0644); CHECK(fd >= 0, "create %s: %s", nm, strerror(errno)); close(fd); }
    CHECK(unlink("/mnt/e4rw/new2.txt") == 0, "unlink new2: %s", strerror(errno));
    CHECK(unlink("/mnt/e4rw/new3.bin") == 0, "unlink new3: %s", strerror(errno));
    for (int i = 0; i < 300; i += 2) { snprintf(nm, sizeof(nm), "/mnt/e4rw/cfile_%03d", i); CHECK(unlink(nm) == 0, "unlink %s: %s", nm, strerror(errno)); }
    fd = open("/mnt/e4rw/hdir/x.txt", O_WRONLY | O_CREAT, 0644);
    CHECK(fd >= 0, "create in htree directory: %s", strerror(errno)); if (fd >= 0) close(fd);
    CHECK(unlink("/mnt/e4rw/hdir/f_7") == 0, "unlink from htree dir: %s", strerror(errno));
    errno = 0; CHECK(unlink("/mnt/e4rw/nosuch") != 0 && errno == ENOENT, "unlink missing: %s", strerror(errno));
    CHECK(access("/mnt/e4rw/new2.txt", F_OK) != 0 && access("/mnt/e4rw/cfile_000", F_OK) != 0 && access("/mnt/e4rw/cfile_001", F_OK) == 0, "listing after unlink");
    DIR *D = opendir("/mnt/e4rw"); int nc = 0; struct dirent *e;
    CHECK(D, "opendir"); while ((e = readdir(D))) if (!strncmp(e->d_name, "cfile_", 6)) nc++; closedir(D);
    CHECK(nc == 150, "%d cfile entries, want 150", nc);
    D = opendir("/mnt/e4rw/hdir"); int nh = 0; CHECK(D, "opendir hdir"); while ((e = readdir(D))) if (e->d_name[0] != '.') nh++; closedir(D);
    CHECK(nh == 2000, "hdir %d entries, want 2000", nh);
    sync();
    R(PASS, "truncate/ftruncate/O_TRUNC, 303 creates (dir grew), 153 unlinks, htree create ok, htree unlink ok");
}

/* ext4 write step 4 on /mnt/e4rw (after vol.ext_ns): mkdir/rmdir/rename,
 * same script as host_e4w.c; host replays (e4w_check.py ... dir) + e2fsck. */
static int e4d_wf(const char *p, const char *str) { int fd = open(p, O_WRONLY | O_CREAT, 0644); if (fd < 0) return -1; int k = write(fd, str, strlen(str)); close(fd); return k == (int)strlen(str) ? 0 : -1; }
#define E4D(call, werr) do { errno = 0; int _r = (call); int _e = _r < 0 ? errno : 0; CHECK(_e == (werr), "%s -> %d errno %d (%s), want errno %d", #call, _r, _e, strerror(_e), werr); } while (0)
static int t_vol_extdir(char *d, size_t n) {
    struct stat st; char b[64];
    if (stat("/mnt/e4rw/w1.bin", &st) != 0) R(SKIP, "no /mnt/e4rw scratch volume");
#define M(x) "/mnt/e4rw/" x
    E4D(mkdir(M("d1"), 0755), 0); E4D(mkdir(M("d1/d2"), 0755), 0); E4D(mkdir(M("d1/d2/d3"), 0755), 0); E4D(mkdir(M("d1"), 0755), EEXIST);
    CHECK(stat(M("d1"), &st) == 0 && S_ISDIR(st.st_mode) && st.st_nlink == 3, "d1 nlink %d", (int)st.st_nlink);
    CHECK(e4d_wf(M("d1/f1"), "f1data") == 0 && e4d_wf(M("d1/d2/f2"), "f2data") == 0, "write files in new dirs");
    E4D(rename(M("keep.txt"), M("d1/keep2.txt")), 0);
    E4D(rename(M("new1.txt"), M("new1b.txt")), 0);
    CHECK(e4d_wf(M("d1/tgt.txt"), "old") == 0, "tgt");
    E4D(rename(M("d1/f1"), M("d1/tgt.txt")), 0);
    E4D(rename(M("d1/d2"), M("d2moved")), 0);
    E4D(rename(M("d2moved"), M("d2moved/d3/x")), EINVAL);
    E4D(mkdir(M("e1"), 0755), 0);
    E4D(rename(M("d1"), M("e1")), 0);
    E4D(rmdir(M("e1")), ENOTEMPTY); E4D(rmdir(M("d2moved/d3")), 0);
    E4D(mkdir(M("rmd"), 0755), 0); E4D(rmdir(M("rmd")), 0);
    E4D(rename(M("cfile_001"), M("hdir/x")), 0);
    E4D(rename(M("hdir/f_9"), M("f9out")), 0);
    E4D(unlink(M("e1")), EISDIR); E4D(rmdir(M("new1b.txt")), ENOTDIR);
    E4D(rename(M("cfile_003"), M("hdir/f_11")), 0);
    memset(b, 0, sizeof(b));
    CHECK(get_file(M("e1/tgt.txt"), b, sizeof(b)) == 6 && !strcmp(b, "f1data"), "e1/tgt.txt '%s'", b);
    memset(b, 0, sizeof(b));
    CHECK(get_file(M("e1/keep2.txt"), b, sizeof(b)) == 5 && !strcmp(b, "kept\n"), "e1/keep2.txt '%s'", b);
    CHECK(access(M("d1"), F_OK) != 0 && access(M("e1/f1"), F_OK) != 0 && access(M("d2moved/f2"), F_OK) == 0, "listing after renames");
    CHECK(stat(M("d2moved/.."), &st) == 0 && st.st_ino == 2, "d2moved/.. ino %lu", (unsigned long)st.st_ino);
    CHECK(stat(M("e1"), &st) == 0 && st.st_nlink == 2, "e1 nlink %d", (int)st.st_nlink);
    sync();
    R(PASS, "mkdir x6, rmdir x2 (+ENOTEMPTY/ENOTDIR), rename file/dir same+cross dir, replace file/dir/htree entry, subtree move refused");
#undef M
}

/* ext4 write step 5 on /mnt/e4rw (after vol.ext_dir): hashed inserts into the
 * htree directory hdir (leaf splits), lookups through the index (namei uses
 * it), mkdir/rename inside it; host replays (e4w_check.py ... dir) + e2fsck. */
static int t_vol_exthtree(char *d, size_t n) {
    struct stat st; char nm[96]; int bad = 0, miss = 0, extra = 0;
    if (stat("/mnt/e4rw/hdir", &st) != 0) R(SKIP, "no /mnt/e4rw scratch volume");
    for (int i = 0; i < 1000; i++) {
        snprintf(nm, sizeof(nm), "/mnt/e4rw/hdir/h_%d", i);
        int fd = open(nm, O_WRONLY | O_CREAT, 0644);
        if (fd < 0) { if (!bad++) CHECK(0, "create %s: %s", nm, strerror(errno)); } else close(fd);
    }
    CHECK(bad == 0, "%d hashed creates failed", bad);
    for (int i = 0; i < 1000; i += 7) { snprintf(nm, sizeof(nm), "/mnt/e4rw/hdir/h_%d", i); CHECK(unlink(nm) == 0, "unlink %s: %s", nm, strerror(errno)); }
    CHECK(mkdir("/mnt/e4rw/hdir/hsub", 0755) == 0, "mkdir in htree: %s", strerror(errno));
    CHECK(rename("/mnt/e4rw/hdir/h_1", "/mnt/e4rw/hdir/h_1r") == 0, "rename in htree: %s", strerror(errno));
    int found = 0;
    for (int i = 0; i < 1000; i++) {
        snprintf(nm, sizeof(nm), "/mnt/e4rw/hdir/h_%d", i);
        int want = i % 7 != 0 && i != 1, got = stat(nm, &st) == 0;
        if (want && !got) miss++; else if (!want && got) extra++; else if (got) found++;
    }
    for (int i = 1; i <= 2000; i++) {
        snprintf(nm, sizeof(nm), "/mnt/e4rw/hdir/f_%d", i);
        int want = i != 7 && i != 9, got = stat(nm, &st) == 0;
        if (want && !got) miss++; else if (!want && got) extra++; else if (got) found++;
    }
    CHECK(stat("/mnt/e4rw/hdir/h_1r", &st) == 0 && stat("/mnt/e4rw/hdir/x", &st) == 0 && stat("/mnt/e4rw/hdir/x.txt", &st) == 0, "renamed/created names");
    CHECK(miss == 0 && extra == 0, "index lookups: %d missing, %d unexpected", miss, extra);
    DIR *D = opendir("/mnt/e4rw/hdir"); int nh = 0; struct dirent *e;
    CHECK(D, "opendir hdir"); while ((e = readdir(D))) if (e->d_name[0] != '.') nh++; closedir(D);
    CHECK(nh == 2858, "hdir %d entries, want 2858", nh);
    sync();
    snprintf(d, n, "1000 hashed inserts (leaf splits), 143 unlinks, mkdir+rename in htree, %d names found via index, %d listed (readdir uncapped)", found, nh);
    return PASS;
}

/* ext4 write step 6 on /mnt/e4rw (after vol.ext_htree): sparse file, writes
 * into holes in scattered order (extent root -> leaf, leaf splits on the 1K
 * volume), a partial write into a hole, readback; host checks content (sp6)
 * and e2fsck. */
static unsigned char e4s_pat(long o) { return (unsigned char)(o * 7 + (o >> 10) * 13 + 1); }
static int t_vol_extsparse(char *d, size_t n) {
    struct stat st; static unsigned char b[1024], got[409600], ref[409600];
    if (stat("/mnt/e4rw/w1.bin", &st) != 0) R(SKIP, "no /mnt/e4rw scratch volume");
    int fd = open("/mnt/e4rw/sp6", O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0, "create sp6: %s", strerror(errno));
    CHECK(ftruncate(fd, 409600) == 0, "ftruncate 400K: %s", strerror(errno));
    memset(ref, 0, sizeof(ref)); int bad = 0;
    for (int i = 0; i < 134; i++) {
        long blk = (long)((i * 37) % 134) * 3, o = blk * 1024;
        for (int k = 0; k < 1024; k++) b[k] = e4s_pat(o + k);
        ssize_t w = pwrite(fd, b, 1024, o);
        if (w != 1024) { if (!bad++) CHECK(0, "pwrite block %ld -> %zd (%s)", blk, w, strerror(errno)); }
        else memcpy(ref + o, b, 1024);
    }
    CHECK(bad == 0, "%d hole writes failed", bad);
    for (int k = 0; k < 300; k++) b[k] = e4s_pat(1124 + k);
    CHECK(pwrite(fd, b, 300, 1124) == 300, "partial hole write: %s", strerror(errno)); memcpy(ref + 1124, b, 300);
    for (int k = 0; k < 2048; k++) ref[200 * 1024 + 500 + k] = e4s_pat(200 * 1024 + 500 + k);
    { static unsigned char bb[2048]; memcpy(bb, ref + 200 * 1024 + 500, 2048); CHECK(pwrite(fd, bb, 2048, 200 * 1024 + 500) == 2048, "straddling write: %s", strerror(errno)); }
    CHECK(fstat(fd, &st) == 0 && st.st_size == 409600, "size %lld", (long long)st.st_size);
    CHECK(pread(fd, got, sizeof(got), 0) == (ssize_t)sizeof(got), "pread all");
    long first = -1; for (long k = 0; k < (long)sizeof(got); k++) if (got[k] != ref[k]) { first = k; break; }
    CHECK(first < 0, "readback differs at %ld (got %d want %d)", first, first >= 0 ? got[first] : 0, first >= 0 ? ref[first] : 0);
    close(fd);
    fd = open("/mnt/e4rw/sp6", O_RDONLY); CHECK(fd >= 0 && pread(fd, got, sizeof(got), 0) == (ssize_t)sizeof(got) && !memcmp(got, ref, sizeof(got)), "reopen readback"); close(fd);
    sync();
    snprintf(d, n, "134 scattered hole writes + partial + straddling into a 400K sparse file, readback ok");
    return PASS;
}

/* ext4 write step 7 on /mnt/e4rw (after vol.ext_sparse): 1500 scattered 4K
 * writes into a sparse file push the extent tree to depth 2 (index node
 * splits on the way), readback; truncate back to 3 blocks (tree collapses),
 * readback, unlink.  Host: e2fsck -fn rc=0 + block ownership check. */
static unsigned char e4d_pat(long o) { return (unsigned char)(o * 7 + (o >> 12) * 13 + 3); }
static int t_vol_extdeep(char *d, size_t n) {
    struct stat st; static unsigned char b[4096], g[4096]; static unsigned char wr[3200];
    if (stat("/mnt/e4rw/w1.bin", &st) != 0) R(SKIP, "no /mnt/e4rw scratch volume");
    int fd = open("/mnt/e4rw/dp7", O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0, "create dp7: %s", strerror(errno));
    CHECK(ftruncate(fd, 3200L * 4096) == 0, "ftruncate: %s", strerror(errno));
    memset(wr, 0, sizeof(wr)); int bad = 0, nw = 0;
    for (int i = 0; i < 1500; i++) {
        long blk = (long)((i * 641L) % 1600) * 2, o = blk * 4096;
        for (int k = 0; k < 4096; k++) b[k] = e4d_pat(o + k);
        ssize_t w = pwrite(fd, b, 4096, o);
        if (w != 4096) { if (!bad++) CHECK(0, "pwrite block %ld -> %zd (%s)", blk, w, strerror(errno)); } else { wr[blk] = 1; nw++; }
    }
    CHECK(bad == 0, "%d scattered writes failed", bad);
    long first = -1;
    for (long blk = 0; blk < 3200 && first < 0; blk++) {
        CHECK(pread(fd, g, 4096, blk * 4096) == 4096, "pread block %ld", blk);
        for (int k = 0; k < 4096; k++) if (g[k] != (wr[blk] ? e4d_pat(blk * 4096 + k) : 0)) { first = blk * 4096 + k; break; }
    }
    CHECK(first < 0, "readback differs at %ld", first);
    CHECK(ftruncate(fd, 3L * 4096 + 5) == 0, "truncate down: %s", strerror(errno));
    CHECK(fstat(fd, &st) == 0 && st.st_size == 3L * 4096 + 5, "size after truncate %lld", (long long)st.st_size);
    for (long blk = 0; blk < 4; blk++) {
        int want = blk < 3 ? 4096 : 5;
        CHECK(pread(fd, g, 4096, blk * 4096) == want, "pread after truncate block %ld", blk);
        for (int k = 0; k < want; k++) CHECK(g[k] == (wr[blk] ? e4d_pat(blk * 4096 + k) : 0), "after truncate: byte %ld", blk * 4096 + k);
    }
    close(fd);
    CHECK(unlink("/mnt/e4rw/dp7") == 0, "unlink dp7: %s", strerror(errno));
    sync();
    snprintf(d, n, "%d scattered 4K writes (extent tree depth 2), readback, truncate to 3 blocks, unlink", nw);
    return PASS;
}
/* journaled writes (TEST ONLY fstype ext4wj-test) on /mnt/e4j: overwrite, append,
 * create/unlink, mkdir/rename/rmdir, truncate, htree inserts/unlinks, then sync.
 * The host checks the image with e2fsck and e4j_check.py. */
static unsigned char e4j_pat(long o, int s) { return (unsigned char)(s * 37 + o * 13 + (o >> 10)); }
static int e4j_put(const char *p, long off, long len, int s, int flags) {
    static unsigned char b[65536];
    int fd = open(p, flags, 0644); if (fd < 0) return -1;
    for (long k = 0; k < len; k++) b[k] = e4j_pat(off + k, s);
    long w = pwrite(fd, b, len, off); close(fd); return w == len ? 0 : -1;
}
static int t_vol_extjournal(char *d, size_t n) {
    struct stat st; char nm[96]; static unsigned char g[65536];
    if (stat("/mnt/e4j/jw.bin", &st) != 0) R(SKIP, "no /mnt/e4j journaled scratch volume");
    CHECK(st.st_size == 100000, "jw.bin size %lld", (long long)st.st_size);
    CHECK(e4j_put("/mnt/e4j/jw.bin", 3000, 20000, 1, O_WRONLY) == 0, "overwrite: %s", strerror(errno));
    CHECK(e4j_put("/mnt/e4j/jw.bin", 100000, 40000, 2, O_WRONLY) == 0, "append: %s", strerror(errno));
    int fd = open("/mnt/e4j/jw.bin", O_RDONLY); CHECK(fd >= 0, "reopen");
    CHECK(pread(fd, g, 40000, 100000) == 40000, "read append"); close(fd);
    for (long k = 0; k < 40000; k++) CHECK(g[k] == e4j_pat(100000 + k, 2), "append readback at %ld", k);
    for (int i = 0; i < 60; i++) { snprintf(nm, sizeof(nm), "/mnt/e4j/jf_%02d", i); CHECK(e4j_put(nm, 0, 1000 + i, 3 + i, O_WRONLY | O_CREAT | O_TRUNC) == 0, "create %s: %s", nm, strerror(errno)); }
    for (int i = 0; i < 60; i += 2) { snprintf(nm, sizeof(nm), "/mnt/e4j/jf_%02d", i); CHECK(unlink(nm) == 0, "unlink %s: %s", nm, strerror(errno)); }
    CHECK(mkdir("/mnt/e4j/jd", 0755) == 0, "mkdir: %s", strerror(errno));
    CHECK(e4j_put("/mnt/e4j/jd/a", 0, 5000, 9, O_WRONLY | O_CREAT) == 0, "jd/a");
    CHECK(rename("/mnt/e4j/jd", "/mnt/e4j/jd2") == 0, "rename dir: %s", strerror(errno));
    CHECK(mkdir("/mnt/e4j/jd2/sub", 0755) == 0 && rmdir("/mnt/e4j/jd2/sub") == 0, "mkdir/rmdir sub: %s", strerror(errno));
    CHECK(truncate("/mnt/e4j/jt.bin", 12345) == 0, "truncate: %s", strerror(errno));
    for (int i = 0; i < 200; i++) { snprintf(nm, sizeof(nm), "/mnt/e4j/jh/n_%d", i); CHECK(e4j_put(nm, 0, 100 + i, 50, O_WRONLY | O_CREAT) == 0, "htree create %s: %s", nm, strerror(errno)); }
    for (int i = 0; i < 100; i++) { snprintf(nm, sizeof(nm), "/mnt/e4j/jh/e_%d", i); CHECK(unlink(nm) == 0, "htree unlink %s: %s", nm, strerror(errno)); }
    DIR *D = opendir("/mnt/e4j/jh"); int nh = 0; struct dirent *e; CHECK(D, "opendir jh");
    while ((e = readdir(D))) if (e->d_name[0] != '.') nh++; closedir(D);
    CHECK(nh == 700, "jh lists %d names (want 700)", nh);
    CHECK(stat("/mnt/e4j/jd2/a", &st) == 0 && st.st_size == 5000 && access("/mnt/e4j/jd", F_OK) != 0, "rename result");
    sync();
    snprintf(d, n, "overwrite+append, 60 creates/30 unlinks, mkdir/rename/rmdir, truncate, 200 htree inserts/100 unlinks, sync - all journaled");
    return PASS;
}
static int t_vol_exte4dw(char *d, size_t n) {
    struct stat st; static unsigned char g[8192];
    if (stat("/mnt/e4dw/frag.bin", &st) != 0) R(SKIP, "no /mnt/e4dw scratch copy of ext4d");
    CHECK(e4j_put("/mnt/e4dw/e4dw_probe.bin", 0, 60000, 11, O_WRONLY | O_CREAT | O_TRUNC) == 0, "write probe: %s", strerror(errno));
    CHECK(mkdir("/mnt/e4dw/e4dw_dir", 0755) == 0, "mkdir: %s", strerror(errno));
    CHECK(unlink("/mnt/e4dw/frag.bin") == 0, "unlink frag.bin: %s", strerror(errno));
    CHECK(e4j_put("/mnt/e4dw/orph.bin", 0, 5000, 12, O_WRONLY | O_CREAT | O_TRUNC) == 0, "write orph: %s", strerror(errno));
    int fd = open("/mnt/e4dw/orph.bin", O_RDONLY); CHECK(fd >= 0, "open orph: %s", strerror(errno));
    CHECK(unlink("/mnt/e4dw/orph.bin") == 0, "unlink of an open file: %s", strerror(errno));
    CHECK(access("/mnt/e4dw/orph.bin", F_OK) != 0, "orph.bin still listed after unlink");
    CHECK(pread(fd, g, 5000, 0) == 5000, "read through the open fd after unlink: %s", strerror(errno));
    for (int k = 0; k < 5000; k++) CHECK(g[k] == e4j_pat(k, 12), "orphan readback at %d", k);
    close(fd);
    sync();
    snprintf(d, n, "plain ext4w on the ext4d copy (journaled): write, mkdir, unlink, unlink-while-open (orphan) read via fd, close frees it");
    return PASS;
}
/* vol.ext_links: symlinks / hard links / chmod+chown on the journaled
 * ext4d copy; the lk_keep/ entries stay for the host check (e4dw_check.py:
 * debugfs must see the fast + slow symlink, nlink 2 and the stored mode/owner). */
static int t_vol_extlinks(char *d, size_t n) {
    struct stat st; const char *b = "/mnt/e4dw/lk_t";
    if (stat("/mnt/e4dw/big.bin", &st) != 0) R(SKIP, "no /mnt/e4dw scratch copy of ext4d");
    CHECK(mkdir(b, 0755) == 0, "mkdir %s: %s", b, strerror(errno));
    char sub[700]; int r;
    if ((r = lk_sym(b, sub, sizeof sub)) != PASS) R(r, "symlinks: %s", sub);
    if ((r = lk_hard(b, sub, sizeof sub)) != PASS) R(r, "hard links: %s", sub);
    if ((r = lk_perm(b, 1, sub, sizeof sub)) != PASS) R(r, "perms: %s", sub);
    CHECK(rmdir(b) == 0, "rmdir %s: %s", b, strerror(errno));
    /* persistent fixture for the host check */
    const char *k = "/mnt/e4dw/lk_keep";
    CHECK(mkdir(k, 0755) == 0, "mkdir lk_keep: %s", strerror(errno));
    CHECK(put_file("/mnt/e4dw/lk_keep/data.bin", "e4dw-link-data\n", 15) == 0, "data.bin: %s", strerror(errno));
    CHECK(symlink("data.bin", "/mnt/e4dw/lk_keep/fast") == 0, "fast symlink: %s", strerror(errno));
    char lt[200]; int o = snprintf(lt, sizeof lt, "../lk_keep");
    while (o < 120) o += snprintf(lt + o, sizeof lt - o, "/.");
    snprintf(lt + o, sizeof lt - o, "/data.bin");
    CHECK(symlink(lt, "/mnt/e4dw/lk_keep/slow") == 0, "slow symlink: %s", strerror(errno));
    CHECK(link("/mnt/e4dw/lk_keep/data.bin", "/mnt/e4dw/lk_keep/hard") == 0, "hard link: %s", strerror(errno));
    CHECK(put_file("/mnt/e4dw/lk_keep/perm.bin", "p", 1) == 0, "perm.bin");
    CHECK(chmod("/mnt/e4dw/lk_keep/perm.bin", 0640) == 0 && chown("/mnt/e4dw/lk_keep/perm.bin", 1234, 4321) == 0, "chmod/chown perm.bin: %s", strerror(errno));
    char buf[32]; CHECK(get_file("/mnt/e4dw/lk_keep/slow", buf, sizeof buf) == 15, "read via slow link");
    CHECK(stat("/mnt/e4dw/lk_keep/hard", &st) == 0 && st.st_nlink == 2, "hard nlink %lu", (unsigned long)st.st_nlink);
    sync();
    R(PASS, "e4dw: %s; hard links + perms ok; lk_keep/ left for the host check (fast, slow %zu B, hard, perm.bin 0640 1234:4321)", sub, strlen(lt));
}
static int t_vol_mountslots(char *d, size_t n) {
    struct stat st; const char *want[] = { "ext4d", "e4rw", "e4j", "e4dw" }; int found = 0, total = 0;
    if (stat("/mnt/e4j", &st) != 0) R(SKIP, "no ext4 fixture mounts");
    DIR *D = opendir("/mnt"); CHECKE(D, "opendir /mnt");
    struct dirent *e;
    while ((e = readdir(D))) {
        if (e->d_name[0] == '.') continue;
        total++;
        for (int i = 0; i < 4; i++) if (!strcmp(e->d_name, want[i])) found |= 1 << i;
    }
    closedir(D);
    CHECK(found == 15, "mount points missing from /mnt listing (mask 0x%x of 0xf)", found);
    for (int i = 0; i < 4; i++) {
        char p[64]; snprintf(p, sizeof p, "/mnt/%s", want[i]);
        CHECK(stat(p, &st) == 0 && S_ISDIR(st.st_mode), "%s not a mounted directory", p);
    }
    R(PASS, "%d entries in /mnt; ext4d, e4rw, e4j, e4dw all mounted and resolvable (more than the old 8-slot limit in total)", total);
}
static int t_vol_extmeta(char *d, size_t n) {
    FILE *f = fopen("/home/user/ext_manifest.txt", "r");
    if (!f) R(SKIP, "no /home/user/ext_manifest.txt");
    char line[700], err[300] = ""; int nf = 0, nd = 0, nc = 0, nx = 0, nl = 0, bad = 0;
#define EXTBAD(...) do { if (!bad++) snprintf(err, sizeof(err), __VA_ARGS__); } while (0)
    while (fgets(line, sizeof(line), f)) {
        char t, rel[500], p[600]; unsigned long mode = 0; unsigned long uid = 0, gid = 0; long long size = 0, mt = 0, cnt = 0; unsigned long crc = 0;
        if (sscanf(line, "%c %499s", &t, rel) != 2) continue;
        snprintf(p, sizeof(p), "/mnt/%s", rel);
        struct stat st; uint32_t c; long long len;
        if (t == 'F' && sscanf(line, "%*c %*s %lo %lu %lu %lld %lld %lx", &mode, &uid, &gid, &size, &mt, &crc) == 6) {
            nf++;
            if (stat(p, &st) != 0) { EXTBAD("stat %s: %s", p, strerror(errno)); continue; }
            if (st.st_mode != mode || st.st_uid != uid || st.st_gid != gid || st.st_size != size || (long long)st.st_mtime != mt)
                { EXTBAD("%s: mode %o uid %u gid %u size %lld mtime %lld, want %lo %lu %lu %lld %lld", p, st.st_mode, st.st_uid, st.st_gid, (long long)st.st_size, (long long)st.st_mtime, mode, uid, gid, size, mt); continue; }
            if (file_crc(p, &c, &len) || c != crc || len != size) EXTBAD("%s: content crc %08x len %lld, want %08lx %lld", p, c, len, crc, size);
        } else if (t == 'D' && sscanf(line, "%*c %*s %lo %lu %lu %lld", &mode, &uid, &gid, &cnt) == 4) {
            nd++;
            if (stat(p, &st) != 0) { EXTBAD("stat %s: %s", p, strerror(errno)); continue; }
            if (st.st_mode != mode || st.st_uid != uid || st.st_gid != gid) { EXTBAD("%s: dir mode %o uid %u gid %u, want %lo %lu %lu", p, st.st_mode, st.st_uid, st.st_gid, mode, uid, gid); continue; }
            DIR *D = opendir(p); long long k = 0; struct dirent *e;
            if (!D) { EXTBAD("opendir %s: %s", p, strerror(errno)); continue; }
            while ((e = readdir(D))) if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) k++;
            closedir(D);
            if (k != cnt) EXTBAD("%s: %lld entries, want %lld", p, k, cnt);
        } else if (t == 'C' && sscanf(line, "%*c %*s %lld %lx", &size, &crc) == 2) {
            nc++;
            if (file_crc(p, &c, &len) || c != crc || len != size) EXTBAD("%s: content crc %08x len %lld, want %08lx %lld", p, c, len, crc, size);
        } else if (t == 'L') {
            char want[500], got[600]; nl++;
            if (sscanf(line, "%*c %*s %499s", want) != 1) continue;
            ssize_t r = readlink(p, got, sizeof(got) - 1); struct stat ls;
            if (r < 0) { EXTBAD("readlink %s: %s", p, strerror(errno)); continue; }
            got[r] = 0;
            if (strcmp(got, want)) EXTBAD("%s -> '%s', want '%s'", p, got, want);
            else if (lstat(p, &ls) != 0 || !S_ISLNK(ls.st_mode) || ls.st_size != r) EXTBAD("%s: lstat mode %o size %lld", p, ls.st_mode, (long long)ls.st_size);
        } else if (t == 'X') {
            nx++;
            int fd = open(p, O_RDONLY); int e1 = errno;
            if (fd >= 0) { char b[16]; ssize_t r = read(fd, b, sizeof(b)); close(fd); if (r >= 0) EXTBAD("%s: corrupted inode readable (%zd bytes)", p, r); }
            else if (e1 != EIO && e1 != ENOENT) EXTBAD("%s: open errno %d", p, e1);
            if (stat(p, &st) == 0) EXTBAD("%s: stat of corrupted inode succeeded", p);
        }
    }
    fclose(f);
    if (!nf && !nd) R(SKIP, "manifest empty");
    CHECK(!bad, "%d mismatches; first: %s", bad, err);
    R(PASS, "%d files, %d dirs, %d content, %d symlinks, %d bad-csum rejected", nf, nd, nc, nl, nx);
}

/* ===================== dev ===================== */
static int t_dev_null_zero(char *d, size_t n) {
    int fd = open("/dev/null", O_RDWR); CHECKE(fd >= 0, "open /dev/null");
    CHECK(write(fd, "abc", 3) == 3, "write null"); char b[16]; CHECK(read(fd, b, 16) == 0, "read null not EOF"); close(fd);
    fd = open("/dev/zero", O_RDONLY);
    if (fd < 0) R(PASS, "/dev/null ok; /dev/zero absent (%s)", strerror(errno));
    memset(b, 1, 16); CHECK(read(fd, b, 16) == 16, "read zero"); for (int i = 0; i < 16; i++) CHECK(b[i] == 0, "zero not zero"); close(fd);
    R(PASS, "/dev/null + /dev/zero");
}
static int t_dev_random(char *d, size_t n) {
    const char *ps[] = { "/dev/urandom", "/dev/random" }; int found = 0; char res[100] = "";
    for (int i = 0; i < 2; i++) {
        int fd = open(ps[i], O_RDONLY); if (fd < 0) continue; found++;
        uint8_t a[32] = {0}, b[32] = {0}; CHECK(read(fd, a, 32) == 32 && read(fd, b, 32) == 32, "%s short read", ps[i]);
        CHECK(memcmp(a, b, 32), "%s returns identical blocks", ps[i]); close(fd);
        strcat(res, ps[i]); strcat(res, " ");
    }
    if (!found) R(SKIP, "no /dev/urandom or /dev/random");
    R(PASS, "%sok", res);
}
static int t_dev_tty(char *d, size_t n) {
    int fd = isatty(0) ? 0 : isatty(1) ? 1 : -1;
    if (fd < 0) { fd = open("/dev/tty", O_RDWR); if (fd < 0) R(SKIP, "no tty on stdin/stdout"); }
    struct termios t; CHECKE(tcgetattr(fd, &t) == 0, "TCGETS");
    CHECKE(tcsetattr(fd, TCSANOW, &t) == 0, "TCSETS (same settings)");
    struct winsize ws; CHECKE(ioctl(fd, TIOCGWINSZ, &ws) == 0, "TIOCGWINSZ");
    pid_t pg = tcgetpgrp(fd);
    R(PASS, "termios lflag=0x%x %dx%d fg pgrp=%d", t.c_lflag, ws.ws_col, ws.ws_row, pg);
}
static int t_dev_fb(char *d, size_t n) {
    int fd = open("/dev/fb0", O_RDWR); if (fd < 0) R(SKIP, "/dev/fb0: %s", strerror(errno));
    struct fb_var_screeninfo v; struct fb_fix_screeninfo f;
    CHECKE(ioctl(fd, FBIOGET_VSCREENINFO, &v) == 0, "FBIOGET_VSCREENINFO");
    CHECKE(ioctl(fd, FBIOGET_FSCREENINFO, &f) == 0, "FBIOGET_FSCREENINFO");
    CHECK(v.xres && v.yres && v.bits_per_pixel && f.line_length >= v.xres * v.bits_per_pixel / 8, "bad geometry %ux%u bpp %u line %u", v.xres, v.yres, v.bits_per_pixel, f.line_length);
    size_t len = f.smem_len ? f.smem_len : (size_t)f.line_length * v.yres;
    uint32_t *m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0); CHECKE(m != MAP_FAILED, "mmap fb");
    volatile uint32_t x = m[0]; (void)x;
    if (draw && v.bits_per_pixel == 32) for (unsigned y = 0; y < 64 && y < v.yres; y++) for (unsigned i = 0; i < 64; i++) m[y * (f.line_length / 4) + i] = 0x00FF00FF;
    munmap(m, len); close(fd);
    R(PASS, "%ux%u %ubpp line=%u smem=%u mmap ok%s", v.xres, v.yres, v.bits_per_pixel, f.line_length, f.smem_len, draw ? " (drew 64x64)" : "");
}
static int t_dev_evdev(char *d, size_t n) {
    char res[400] = ""; int o = 0, cnt = 0;
    for (int i = 0; i < 32; i++) {
        char p[32]; snprintf(p, sizeof(p), "/dev/input/event%d", i);
        int fd = open(p, O_RDONLY | O_NONBLOCK); if (fd < 0) continue;
        char name[80] = ""; int r = ioctl(fd, EVIOCGNAME(sizeof(name)), name);
        unsigned long ev = 0; ioctl(fd, EVIOCGBIT(0, sizeof(ev)), &ev);
        struct input_event ie; errno = 0; ssize_t rr = read(fd, &ie, sizeof(ie)); int e = errno;
        close(fd); cnt++;
        CHECK(r >= 0, "%s EVIOCGNAME failed", p);
        CHECK(rr == (ssize_t)sizeof(ie) || (rr < 0 && e == EAGAIN), "%s nonblock read: %zd errno %d", p, rr, e);
        o += snprintf(res + o, sizeof(res) - o, "event%d='%s' ", i, name);
    }
    if (!cnt) R(SKIP, "no /dev/input/event*");
    R(PASS, "%s", res);
}
static int t_dev_uinput(char *d, size_t n) {
    int u = open("/dev/uinput", O_WRONLY | O_NONBLOCK); if (u < 0) R(SKIP, "/dev/uinput: %s", strerror(errno));
    int r = 0; r |= ioctl(u, UI_SET_EVBIT, EV_KEY); r |= ioctl(u, UI_SET_EVBIT, EV_SYN); r |= ioctl(u, UI_SET_KEYBIT, KEY_F24);
    struct uinput_setup us; memset(&us, 0, sizeof(us)); us.id.bustype = BUS_VIRTUAL; us.id.vendor = 0x0575; us.id.product = 0x7e57;
    snprintf(us.name, sizeof(us.name), "ostest uinput %d", getpid());
    r |= ioctl(u, UI_DEV_SETUP, &us); CHECK(!r, "UI_SET*/UI_DEV_SETUP failed");
    CHECKE(ioctl(u, UI_DEV_CREATE) == 0, "UI_DEV_CREATE");
    msleep(50);
    int rfd = -1;
    for (int i = 0; i < 32 && rfd < 0; i++) { char p[32]; snprintf(p, sizeof(p), "/dev/input/event%d", i); int fd = open(p, O_RDONLY | O_NONBLOCK); if (fd < 0) continue; char nm[80] = ""; ioctl(fd, EVIOCGNAME(sizeof(nm)), nm); if (!strcmp(nm, us.name)) rfd = fd; else close(fd); }
    if (rfd < 0) { ioctl(u, UI_DEV_DESTROY); R(FAIL, "created device not found under /dev/input by name"); }
    struct input_event ev[4]; memset(ev, 0, sizeof(ev));
    ev[0].type = EV_KEY; ev[0].code = KEY_F24; ev[0].value = 1; ev[1].type = EV_SYN;
    ev[2].type = EV_KEY; ev[2].code = KEY_F24; ev[2].value = 0; ev[3].type = EV_SYN;
    CHECK(write(u, ev, sizeof(ev)) == sizeof(ev), "write events: %s", strerror(errno));
    int keys = 0; long long t0 = mono_ms();
    while (mono_ms() - t0 < 1000 && keys < 2) { struct input_event in[8]; ssize_t k = read(rfd, in, sizeof(in)); if (k > 0) { for (int i = 0; i < k / (ssize_t)sizeof(in[0]); i++) if (in[i].type == EV_KEY && in[i].code == KEY_F24) keys++; } else msleep(10); }
    close(rfd); ioctl(u, UI_DEV_DESTROY); close(u);
    CHECK(keys == 2, "read back %d of 2 KEY_F24 events", keys);
    R(PASS, "created, events read back via evdev, destroyed");
}
static int t_dev_wm(char *d, size_t n) {
    wm_conn_t c; int r = wm_connect(&c);
    if (r < 0) R(SKIP, "/dev/wm: %s (desktop not running?)", strerror(-r));
    wm_window_t w; r = wm_window_create(&c, &w, 120, 80, "ostest", WM_TYPE_NORMAL, 0);
    if (r < 0) { wm_disconnect(&c); R(FAIL, "create window: %s", strerror(-r)); }
    wm_fill_rect(&w, 0, 0, 120, 80, wm_rgb(30, 120, 200));
    r = wm_window_present(&w, 0, 0, 0, 0, 0); CHECK(r >= 0, "present: %s", strerror(-r));
    r = wm_window_set_title(&w, "ostest 2"); CHECK(r >= 0, "set title: %s", strerror(-r));
    struct wm_event ev; int e = wm_next_event(&c, &ev, 100);
    r = wm_window_destroy(&w); CHECK(r >= 0, "destroy: %s", strerror(-r));
    wm_disconnect(&c);
    R(PASS, "window create/present/title/destroy (event poll -> %d)", e);
}

/* ===================== net ===================== */
static int t_net_socket(char *d, size_t n) {
    int t = socket(AF_INET, SOCK_STREAM, 0), u = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(t >= 0 && u >= 0, "socket tcp=%d udp=%d: %s", t, u, strerror(errno));
    close(t); close(u);
    int sp[2]; if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == 0) { write(sp[0], "hi", 2); char b[4] = ""; read(sp[1], b, 2); close(sp[0]); close(sp[1]); CHECK(!memcmp(b, "hi", 2), "socketpair data"); R(PASS, "tcp/udp sockets, socketpair ok"); }
    R(PASS, "tcp/udp sockets (socketpair: %s)", strerror(errno));
}
static int t_net_udp_lo(char *d, size_t n) {
    int a = socket(AF_INET, SOCK_DGRAM, 0), b = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(47011), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    CHECK(bind(a, (void *)&sa, sizeof(sa)) == 0, "bind 127.0.0.1:47011: %s", strerror(errno));
    CHECK(sendto(b, "ping", 4, 0, (void *)&sa, sizeof(sa)) == 4, "sendto loopback: %s", strerror(errno));
    struct pollfd p = { a, POLLIN, 0 };
    CHECK(poll(&p, 1, 1000) == 1, "no datagram within 1s (loopback UDP unsupported?)");
    char buf[8] = ""; CHECK(recv(a, buf, 8, 0) == 4 && !memcmp(buf, "ping", 4), "recv");
    close(a); close(b);
    R(PASS, "127.0.0.1 datagram");
}
static int t_net_tcp_lo(char *d, size_t n) {
    int l = socket(AF_INET, SOCK_STREAM, 0); int one = 1; setsockopt(l, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(47012), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    CHECK(bind(l, (void *)&sa, sizeof(sa)) == 0, "bind: %s", strerror(errno));
    CHECK(listen(l, 4) == 0, "listen: %s", strerror(errno));
    pid_t c = fork();
    if (c == 0) { int s = socket(AF_INET, SOCK_STREAM, 0); if (connect(s, (void *)&sa, sizeof(sa))) _exit(2); static char b[100000]; memset(b, 'T', sizeof(b)); if (writeall(s, b, sizeof(b)) != (ssize_t)sizeof(b)) _exit(3); shutdown(s, SHUT_WR); char r[4]; if (read(s, r, 2) != 2) _exit(4); _exit(0); }
    struct pollfd p = { l, POLLIN, 0 };
    if (poll(&p, 1, 2000) != 1) { kill(c, SIGKILL); waitpid(c, NULL, 0); R(FAIL, "no connection within 2s (loopback TCP unsupported?)"); }
    int s = accept(l, NULL, NULL); CHECK(s >= 0, "accept: %s", strerror(errno));
    long got = 0; char b[8192]; ssize_t r; while ((r = read(s, b, sizeof(b))) > 0) got += r;
    write(s, "ok", 2); int st; waitpid(c, &st, 0); close(s); close(l);
    CHECK(got == 100000 && WIFEXITED(st) && WEXITSTATUS(st) == 0, "got %ld bytes, client status 0x%x", got, st);
    R(PASS, "connect/accept/100KB/shutdown");
}
static int t_net_dns(char *d, size_t n) {
    struct addrinfo hints, *res = NULL; memset(&hints, 0, sizeof(hints)); hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    int r = getaddrinfo("example.com", "80", &hints, &res);
    CHECK(r == 0 && res, "getaddrinfo example.com: %s", gai_strerror(r));
    char ip[32]; inet_ntop(AF_INET, &((struct sockaddr_in *)res->ai_addr)->sin_addr, ip, sizeof(ip));
    int s = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(connect(s, res->ai_addr, res->ai_addrlen) == 0, "connect %s:80: %s", ip, strerror(errno));
    const char *q = "HEAD / HTTP/1.0\r\nHost: example.com\r\n\r\n"; write(s, q, strlen(q));
    char b[64] = ""; readall(s, b, 12); close(s); freeaddrinfo(res);
    CHECK(!strncmp(b, "HTTP/1.", 7), "reply '%.12s'", b);
    R(PASS, "example.com=%s, reply '%.12s'", ip, b);
}

/* ===================== misc ===================== */
static int t_cwd(char *d, size_t n) {
    char old[256], cur[256]; CHECKE(getcwd(old, sizeof(old)), "getcwd");
    CHECKE(chdir(tmpdir) == 0, "chdir");
    CHECKE(getcwd(cur, sizeof(cur)), "getcwd 2"); CHECK(!strcmp(cur, tmpdir), "cwd '%s' != '%s'", cur, tmpdir);
    put_file("rel.txt", "r", 1); char p[160]; tpath(p, sizeof(p), "rel.txt"); CHECK(access(p, F_OK) == 0, "relative create landed elsewhere");
    mkdir("sub", 0755); CHECKE(chdir("sub") == 0, "chdir relative"); CHECKE(chdir("..") == 0, "chdir ..");
    getcwd(cur, sizeof(cur)); CHECK(!strcmp(cur, tmpdir), "after .. cwd '%s'", cur);
    int fd = open(".", O_RDONLY | O_DIRECTORY); chdir("/");
    if (fd >= 0) { CHECKE(fchdir(fd) == 0, "fchdir"); getcwd(cur, sizeof(cur)); CHECK(!strcmp(cur, tmpdir), "fchdir -> '%s'", cur); close(fd); }
    chdir(old);
    R(PASS, "getcwd/chdir/relative/../fchdir");
}
static int t_dup(char *d, size_t n) {
    int pf[2]; pipe(pf);
    int a = dup(pf[1]); CHECKE(a >= 0, "dup");
    CHECKE(dup2(pf[1], 60) == 60, "dup2 to 60");
    write(60, "1", 1); write(a, "2", 1); close(60); close(a); close(pf[1]);
    char b[4] = ""; CHECK(read(pf[0], b, 4) == 2 && !memcmp(b, "12", 2), "data via dups '%.4s'", b);
    CHECK(read(pf[0], b, 1) == 0, "EOF only after all dups closed");
    CHECK(dup2(pf[0], pf[0]) == pf[0], "dup2 same fd");
    int e = dup3(pf[0], 61, O_CLOEXEC); CHECK(e == 61 && (fcntl(61, F_GETFD) & FD_CLOEXEC), "dup3 O_CLOEXEC (%d)", e);
    close(61); close(pf[0]);
    R(PASS, "dup/dup2/dup3 + EOF after last close");
}
static int t_fcntl(char *d, size_t n) {
    int pf[2]; pipe(pf);
    int fl = fcntl(pf[0], F_GETFL); CHECK(fl >= 0, "F_GETFL");
    CHECKE(fcntl(pf[0], F_SETFL, fl | O_NONBLOCK) == 0, "F_SETFL");
    CHECK(fcntl(pf[0], F_GETFL) & O_NONBLOCK, "O_NONBLOCK not reported");
    char b; errno = 0; CHECK(read(pf[0], &b, 1) == -1 && errno == EAGAIN, "nonblock read errno %d", errno);
    CHECK(fcntl(pf[0], F_SETFD, FD_CLOEXEC) == 0 && fcntl(pf[0], F_GETFD) == FD_CLOEXEC, "FD_CLOEXEC");
    int x = fcntl(pf[0], F_DUPFD, 40); CHECK(x >= 40, "F_DUPFD >= 40 -> %d", x);
    int bytes = -1; write(pf[1], "abc", 3); CHECK(ioctl(pf[0], FIONREAD, &bytes) == 0 && bytes == 3, "FIONREAD %d", bytes);
    close(x); close(pf[0]); close(pf[1]);
    R(PASS, "GETFL/SETFL O_NONBLOCK/CLOEXEC/DUPFD/FIONREAD");
}
static int t_poll(char *d, size_t n) {
    int pf[2]; pipe(pf);
    struct pollfd p = { pf[0], POLLIN, 0 };
    long long t0 = mono_ms(); int r = poll(&p, 1, 200); long long el = mono_ms() - t0;
    CHECK(r == 0 && el >= 150 && el < 1000, "empty poll r=%d %lldms", r, el);
    write(pf[1], "x", 1); r = poll(&p, 1, 0);
    CHECK(r == 1 && (p.revents & POLLIN), "readable poll r=%d revents=0x%x", r, p.revents);
    struct pollfd w = { pf[1], POLLOUT, 0 }; CHECK(poll(&w, 1, 0) == 1 && (w.revents & POLLOUT), "POLLOUT");
    char c; read(pf[0], &c, 1);
    close(pf[1]); p.revents = 0; poll(&p, 1, 0);
    CHECK(p.revents & (POLLHUP | POLLIN), "no POLLHUP/POLLIN after writer close (0x%x)", p.revents);
    int q[2]; pipe(q);
    pid_t k = fork(); if (k == 0) { msleep(100); write(q[1], "y", 1); _exit(0); }
    struct pollfd pq = { q[0], POLLIN, 0 }; t0 = mono_ms(); r = poll(&pq, 1, 2000); long long el2 = mono_ms() - t0; waitpid(k, NULL, 0);
    CHECK(r == 1 && el2 >= 50 && el2 < 1500, "wakeup by writer r=%d after %lldms", r, el2);
    R(PASS, "timeout %lldms, POLLIN/POLLOUT/HUP, wakeup %lldms", el, el2);
}
static int t_select(char *d, size_t n) {
    int pf[2]; pipe(pf); fd_set rs; struct timeval tv = { 0, 150000 };
    FD_ZERO(&rs); FD_SET(pf[0], &rs); long long t0 = mono_ms();
    int r = select(pf[0] + 1, &rs, NULL, NULL, &tv); long long el = mono_ms() - t0;
    if (r < 0 && errno == ENOSYS) R(SKIP, "select ENOSYS");
    CHECK(r == 0 && el >= 100, "empty select r=%d %lldms", r, el);
    write(pf[1], "x", 1); FD_ZERO(&rs); FD_SET(pf[0], &rs); tv.tv_sec = 1; tv.tv_usec = 0;
    r = select(pf[0] + 1, &rs, NULL, NULL, &tv); CHECK(r == 1 && FD_ISSET(pf[0], &rs), "readable select r=%d", r);
    close(pf[0]); close(pf[1]);
    R(PASS, "timeout %lldms + readable", el);
}
static int t_eventfd(char *d, size_t n) {
    extern int eventfd(unsigned int, int);
    int e = eventfd(0, 0); if (e < 0) R(SKIP, "eventfd: %s", strerror(errno));
    uint64_t v = 5; write(e, &v, 8); v = 2; write(e, &v, 8); v = 0;
    CHECK(read(e, &v, 8) == 8 && v == 7, "counter %llu", (unsigned long long)v);
    close(e); R(PASS, "counter 5+2=7");
}

/* ===================== epoll ===================== */
/* epoll.pipe: level-triggered EPOLLIN/EPOLLOUT on a pipe, data passthrough,
 * timeout, wakeup by a forked writer, EPOLLHUP when the writer goes away. */
static int t_ep_pipe(char *d, size_t n) {
    int ep = epoll_create1(EPOLL_CLOEXEC); if (ep < 0) R(SKIP, "epoll_create1: %s", strerror(errno));
    CHECK(fcntl(ep, F_GETFD) & FD_CLOEXEC, "EPOLL_CLOEXEC not set");
    int pf[2]; CHECKE(pipe(pf) == 0, "pipe");
    struct epoll_event ev = { .events = EPOLLIN, .data.u64 = 0x1122334455667788ULL }, out[4];
    CHECKE(epoll_ctl(ep, EPOLL_CTL_ADD, pf[0], &ev) == 0, "ctl ADD read end");
    long long t0 = mono_ms(); int r = epoll_wait(ep, out, 4, 200); long long el = mono_ms() - t0;
    CHECK(r == 0 && el >= 150 && el < 1000, "empty wait r=%d %lldms", r, el);
    write(pf[1], "x", 1);
    r = epoll_wait(ep, out, 4, 0);
    CHECK(r == 1 && (out[0].events & EPOLLIN) && out[0].data.u64 == 0x1122334455667788ULL,
          "readable r=%d ev=0x%x data=%llx", r, r > 0 ? out[0].events : 0, r > 0 ? (unsigned long long)out[0].data.u64 : 0ULL);
    r = epoll_wait(ep, out, 4, 0);
    CHECK(r == 1, "level-triggered: second wait r=%d", r);
    char c; read(pf[0], &c, 1);
    CHECK(epoll_wait(ep, out, 4, 0) == 0, "still ready after drain");
    struct epoll_event wv = { .events = EPOLLOUT, .data.fd = pf[1] };
    CHECKE(epoll_ctl(ep, EPOLL_CTL_ADD, pf[1], &wv) == 0, "ctl ADD write end");
    r = epoll_wait(ep, out, 4, 0);
    CHECK(r == 1 && out[0].data.fd == pf[1] && (out[0].events & EPOLLOUT), "EPOLLOUT r=%d", r);
    CHECKE(epoll_ctl(ep, EPOLL_CTL_DEL, pf[1], NULL) == 0, "ctl DEL");
    pid_t k = fork(); if (k == 0) { msleep(100); write(pf[1], "y", 1); _exit(0); }
    t0 = mono_ms(); r = epoll_wait(ep, out, 4, 2000); long long el2 = mono_ms() - t0; waitpid(k, NULL, 0);
    CHECK(r == 1 && (out[0].events & EPOLLIN) && el2 >= 50 && el2 < 1500, "wakeup r=%d after %lldms", r, el2);
    read(pf[0], &c, 1);
    close(pf[1]);
    r = epoll_wait(ep, out, 4, 0);
    CHECK(r == 1 && (out[0].events & EPOLLHUP), "no EPOLLHUP after writer close r=%d ev=0x%x", r, r > 0 ? out[0].events : 0);
    close(pf[0]); close(ep);
    R(PASS, "timeout %lldms, LT IN/OUT, wakeup %lldms, HUP", el, el2);
}
/* epoll.unix_socket: socketpair readiness, EPOLLONESHOT + MOD re-arm,
 * EPOLLRDHUP/HUP after peer close, maxevents limit. */
static int t_ep_sock(char *d, size_t n) {
    int ep = epoll_create(4); if (ep < 0) R(SKIP, "epoll_create: %s", strerror(errno));
    int sv[2]; CHECKE(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
    struct epoll_event ev = { .events = EPOLLIN | EPOLLONESHOT, .data.u32 = 7 }, out[4];
    CHECKE(epoll_ctl(ep, EPOLL_CTL_ADD, sv[1], &ev) == 0, "ctl ADD");
    CHECK(epoll_wait(ep, out, 4, 0) == 0, "ready before data");
    write(sv[0], "hi", 2);
    int r = epoll_wait(ep, out, 4, 0);
    CHECK(r == 1 && out[0].data.u32 == 7 && (out[0].events & EPOLLIN), "data ready r=%d", r);
    CHECK(epoll_wait(ep, out, 4, 0) == 0, "ONESHOT reported twice");
    ev.events = EPOLLIN | EPOLLRDHUP; ev.data.u32 = 8;
    CHECKE(epoll_ctl(ep, EPOLL_CTL_MOD, sv[1], &ev) == 0, "ctl MOD re-arm");
    r = epoll_wait(ep, out, 4, 0);
    CHECK(r == 1 && out[0].data.u32 == 8, "re-armed r=%d", r);
    char b[4]; CHECK(read(sv[1], b, 2) == 2 && !memcmp(b, "hi", 2), "socket data");
    struct epoll_event ov = { .events = EPOLLOUT, .data.u32 = 9 };
    CHECKE(epoll_ctl(ep, EPOLL_CTL_ADD, sv[0], &ov) == 0, "ctl ADD peer");
    write(sv[0], "z", 1);                       /* both entries ready now */
    r = epoll_wait(ep, out, 1, 0);
    CHECK(r == 1, "maxevents=1 returned %d", r);
    r = epoll_wait(ep, out, 4, 0);
    CHECK(r == 2, "two ready entries, got %d", r);
    read(sv[1], b, 1);
    CHECKE(epoll_ctl(ep, EPOLL_CTL_DEL, sv[0], NULL) == 0, "ctl DEL peer");
    close(sv[0]);
    r = epoll_wait(ep, out, 4, 500);
    CHECK(r == 1 && (out[0].events & (EPOLLRDHUP | EPOLLHUP | EPOLLIN)), "peer close r=%d ev=0x%x", r, r > 0 ? out[0].events : 0);
    unsigned hev = out[0].events;
    close(sv[1]); close(ep);
    R(PASS, "readiness, ONESHOT/MOD, maxevents, close ev=0x%x", hev);
}
/* epoll.ctl_errors: errno contract + stale-fd cleanup + epoll_pwait/eventfd. */
static int t_ep_ctl(char *d, size_t n) {
    int ep = epoll_create1(0); if (ep < 0) R(SKIP, "epoll_create1: %s", strerror(errno));
    struct epoll_event ev = { .events = EPOLLIN }, out[2];
    int pf[2]; CHECKE(pipe(pf) == 0, "pipe");
    errno = 0; CHECK(epoll_create1(12345) < 0 && errno == EINVAL, "create1(bad flags) errno %d", errno);
    errno = 0; CHECK(epoll_create(0) < 0 && errno == EINVAL, "create(0) errno %d", errno);
    CHECKE(epoll_ctl(ep, EPOLL_CTL_ADD, pf[0], &ev) == 0, "ADD");
    errno = 0; CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, pf[0], &ev) < 0 && errno == EEXIST, "dup ADD errno %d", errno);
    errno = 0; CHECK(epoll_ctl(ep, EPOLL_CTL_MOD, pf[1], &ev) < 0 && errno == ENOENT, "MOD missing errno %d", errno);
    errno = 0; CHECK(epoll_ctl(ep, EPOLL_CTL_DEL, pf[1], NULL) < 0 && errno == ENOENT, "DEL missing errno %d", errno);
    errno = 0; CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, 999, &ev) < 0 && errno == EBADF, "bad fd errno %d", errno);
    errno = 0; CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, ep, &ev) < 0 && errno == EINVAL, "self ADD errno %d", errno);
    errno = 0; CHECK(epoll_ctl(pf[0], EPOLL_CTL_ADD, pf[1], &ev) < 0 && errno == EINVAL, "non-epoll epfd errno %d", errno);
    errno = 0; CHECK(epoll_wait(ep, out, 0, 0) < 0 && errno == EINVAL, "maxevents 0 errno %d", errno);
    errno = 0; CHECK(read(ep, out, 1) < 0 && errno == EINVAL, "read(epfd) errno %d", errno);
    char fp[256]; tpath(fp, sizeof(fp), "ep_reg.txt");
    int rf = open(fp, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (rf >= 0) {
        errno = 0; int er = epoll_ctl(ep, EPOLL_CTL_ADD, rf, &ev) < 0 ? errno : 0;
        close(rf); unlink(fp);
        CHECK(er == EPERM, "regular file ADD errno %d (want EPERM)", er);
    }
    /* closed fds drop out of the interest set */
    write(pf[1], "x", 1);
    CHECK(epoll_wait(ep, out, 2, 0) == 1, "ready before close");
    close(pf[0]);
    CHECK(epoll_wait(ep, out, 2, 0) == 0, "closed fd still reported");
    close(pf[1]);
    /* eventfd + epoll_pwait with an empty mask */
    extern int eventfd(unsigned int, int);
    int e = eventfd(0, 0);
    if (e >= 0) {
        struct epoll_event ee = { .events = EPOLLIN, .data.fd = e };
        CHECKE(epoll_ctl(ep, EPOLL_CTL_ADD, e, &ee) == 0, "ADD eventfd");
        sigset_t m; sigemptyset(&m);
        CHECK(epoll_pwait(ep, out, 2, 0, &m) == 0, "eventfd ready at 0");
        uint64_t v = 3; write(e, &v, 8);
        int r = epoll_pwait(ep, out, 2, 100, &m);
        CHECK(r == 1 && out[0].data.fd == e, "eventfd pwait r=%d", r);
        close(e);
    }
    /* empty set: plain timeout */
    long long t0 = mono_ms(); int r = epoll_wait(ep, out, 2, 150); long long el = mono_ms() - t0;
    CHECK(r == 0 && el >= 100 && el < 1000, "empty set wait r=%d %lldms", r, el);
    close(ep);
    R(PASS, "EINVAL/EEXIST/ENOENT/EBADF/EPERM, stale fd, pwait+eventfd, empty %lldms", el);
}
/* epoll.tty: terminal fds can be added; stdout reports EPOLLOUT. */
static int t_ep_tty(char *d, size_t n) {
    int fd = isatty(1) ? 1 : isatty(0) ? 0 : -1;
    if (fd < 0) R(SKIP, "no tty on stdin/stdout");
    int ep = epoll_create1(0); if (ep < 0) R(SKIP, "epoll_create1: %s", strerror(errno));
    struct epoll_event ev = { .events = fd == 1 ? EPOLLOUT : EPOLLIN, .data.fd = fd }, out[2];
    CHECKE(epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev) == 0, "ADD tty");
    int r = epoll_wait(ep, out, 2, 0);
    if (fd == 1) CHECK(r == 1 && (out[0].events & EPOLLOUT), "tty EPOLLOUT r=%d", r);
    else CHECK(r >= 0, "tty wait r=%d %s", r, strerror(errno));
    close(ep);
    R(PASS, "tty fd %d added, wait r=%d", fd, r);
}

/* ===================== futex / timerfd / signalfd / epoll ET ===================== */
static long fx(volatile uint32_t *u, int op, uint32_t v, const struct timespec *ts, volatile uint32_t *u2, uint32_t v3) {
    return syscall(SYS_futex, u, op, v, ts, u2, v3);
}
/* futex.wait_wake: EAGAIN on mismatch, relative timeout, and a real WAKE
 * (word unchanged) from another process on a MAP_SHARED word. */
static int t_fx_wait_wake(char *d, size_t n) {
    volatile uint32_t *sh = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECKE(sh != MAP_FAILED, "mmap shared");
    sh[0] = 5; sh[1] = 0; sh[2] = 0;
    errno = 0; CHECK(fx(&sh[0], FUTEX_WAIT, 4, NULL, NULL, 0) == -1 && errno == EAGAIN, "mismatch errno %d", errno);
    struct timespec to = { 0, 100 * 1000000L };
    long long t0 = mono_ms(); errno = 0; long r = fx(&sh[0], FUTEX_WAIT, 5, &to, NULL, 0); long long el = mono_ms() - t0;
    CHECK(r == -1 && errno == ETIMEDOUT && el >= 80 && el < 1000, "timeout r=%ld errno %d %lldms", r, errno, el);
    errno = 0; CHECK(fx(&sh[0], FUTEX_WAKE, 1, NULL, NULL, 0) == 0, "WAKE with no waiters != 0");
    pid_t c = fork(); CHECKE(c >= 0, "fork");
    if (c == 0) {
        /* wake until the parent is really queued (WAKE returns 1); word never changes */
        for (int i = 0; i < 200; i++) { msleep(10); long w = fx(&sh[0], FUTEX_WAKE, 1, NULL, NULL, 0); if (w) { sh[1] = (uint32_t)w; _exit(0); } }
        _exit(3);
    }
    struct timespec to2 = { 3, 0 };
    t0 = mono_ms(); errno = 0; r = fx(&sh[0], FUTEX_WAIT, 5, &to2, NULL, 0); el = mono_ms() - t0;
    int st = 0; waitpid(c, &st, 0);
    CHECK(r == 0, "shared WAIT r=%ld errno %d after %lldms (child st %d)", r, errno, el, st);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0 && sh[1] == 1, "child WAKE count %u st %d", sh[1], st);
    CHECK(el < 2500, "woke only after %lldms", el);
    munmap((void *)sh, 4096);
    R(PASS, "EAGAIN, ETIMEDOUT %lldms, cross-process WAKE=1", el);
}
static volatile uint32_t fx_w1, fx_w2, fx_done;
static void *fx_waiter(void *arg) {
    volatile uint32_t *w = arg;
    long r = fx(w, FUTEX_WAIT_PRIVATE, 0, NULL, NULL, 0);
    __atomic_add_fetch(&fx_done, r == 0 ? 1 : 100, __ATOMIC_SEQ_CST);
    return NULL;
}
static int wake_until(volatile uint32_t *u, int op, int want, int ms) {
    int got = 0;
    for (int i = 0; i < ms / 10 && got < want; i++) { long w = fx(u, op, (uint32_t)(want - got), NULL, NULL, 0); if (w > 0) got += (int)w; else msleep(10); }
    return got;
}
/* futex.private_requeue: PRIVATE WAIT/WAKE between threads, CMP_REQUEUE
 * (EAGAIN on stale val3; wake 1 + move 1), then WAKE on the target word. */
static int t_fx_requeue(char *d, size_t n) {
    pthread_t a, b;
    fx_w1 = 0; fx_w2 = 0; fx_done = 0;
    if (pthread_create(&a, NULL, fx_waiter, (void *)&fx_w1)) R(SKIP, "pthread_create failed");
    int got = wake_until(&fx_w1, FUTEX_WAKE_PRIVATE, 1, 2000);
    pthread_join(a, NULL);
    CHECK(got == 1 && fx_done == 1, "private WAKE got %d done %u", got, fx_done);
    fx_done = 0;
    CHECK(pthread_create(&a, NULL, fx_waiter, (void *)&fx_w1) == 0, "pthread_create a");
    CHECK(pthread_create(&b, NULL, fx_waiter, (void *)&fx_w1) == 0, "pthread_create b");
    msleep(150);                                   /* both queued on w1 */
    errno = 0; long r = fx(&fx_w1, FUTEX_CMP_REQUEUE_PRIVATE, 1, (const struct timespec *)1L, &fx_w2, 7);
    CHECK(r == -1 && errno == EAGAIN, "stale CMP_REQUEUE r=%ld errno %d", r, errno);
    r = fx(&fx_w1, FUTEX_CMP_REQUEUE_PRIVATE, 1, (const struct timespec *)1L, &fx_w2, 0);
    CHECK(r == 2, "CMP_REQUEUE woke+moved %ld (want 2)", r);
    for (int i = 0; i < 100 && fx_done < 1; i++) msleep(10);
    msleep(50);
    CHECK(fx_done == 1, "after requeue done=%u (want 1)", fx_done);
    long w1 = fx(&fx_w1, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
    long w2 = fx(&fx_w2, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
    pthread_join(a, NULL); pthread_join(b, NULL);
    CHECK(w1 == 0 && w2 == 1 && fx_done == 2, "after requeue WAKE w1=%ld w2=%ld done=%u", w1, w2, fx_done);
    R(PASS, "private wake, CMP_REQUEUE EAGAIN + 1 woken/1 moved, target WAKE=1");
}
/* futex.bitset_abs: WAIT_BITSET absolute deadlines (MONOTONIC and
 * CLOCK_REALTIME), EINVAL on bitset 0, non-matching WAKE_BITSET wakes none. */
static int t_fx_bitset(char *d, size_t n) {
    static volatile uint32_t w = 3;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_nsec += 100000000L; if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    long long t0 = mono_ms(); errno = 0; long r = fx(&w, FUTEX_WAIT_BITSET_PRIVATE, 3, &ts, NULL, FUTEX_BITSET_MATCH_ANY); long long el = mono_ms() - t0;
    CHECK(r == -1 && errno == ETIMEDOUT && el >= 70 && el < 1000, "abs mono r=%ld errno %d %lldms", r, errno, el);
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 100000000L; if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    t0 = mono_ms(); errno = 0; r = fx(&w, FUTEX_WAIT_BITSET_PRIVATE | FUTEX_CLOCK_REALTIME, 3, &ts, NULL, FUTEX_BITSET_MATCH_ANY); long long el2 = mono_ms() - t0;
    CHECK(r == -1 && errno == ETIMEDOUT && el2 >= 50 && el2 < 1500, "abs realtime r=%ld errno %d %lldms", r, errno, el2);
    errno = 0; CHECK(fx(&w, FUTEX_WAIT_BITSET_PRIVATE, 3, NULL, NULL, 0) == -1 && errno == EINVAL, "bitset 0 errno %d", errno);
    CHECK(fx(&w, FUTEX_WAKE_BITSET_PRIVATE, 1, NULL, NULL, 2) == 0, "WAKE_BITSET no waiters");
    R(PASS, "abs mono %lldms, abs realtime %lldms, EINVAL", el, el2);
}
/* timerfd.basic: one-shot + periodic expirations, gettime/settime old value,
 * nonblocking EAGAIN, ABSTIME, poll and epoll readiness, disarm. */
static int t_tfd_basic(char *d, size_t n) {
    int t = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC); if (t < 0) R(SKIP, "timerfd_create: %s", strerror(errno));
    struct itimerspec cur, its = { { 0, 0 }, { 0, 100 * 1000000L } }, old;
    CHECKE(timerfd_gettime(t, &cur) == 0, "gettime");
    CHECK(!cur.it_value.tv_sec && !cur.it_value.tv_nsec, "fresh timer armed");
    CHECKE(timerfd_settime(t, 0, &its, NULL) == 0, "settime 100ms");
    uint64_t v = 0; long long t0 = mono_ms(); ssize_t rr = read(t, &v, 8); long long el = mono_ms() - t0;
    CHECK(rr == 8 && v == 1 && el >= 70 && el < 1000, "one-shot read %zd v=%llu %lldms", rr, (unsigned long long)v, el);
    CHECKE(timerfd_gettime(t, &cur) == 0, "gettime after");
    CHECK(!cur.it_value.tv_sec && !cur.it_value.tv_nsec, "one-shot still armed");
    errno = 0; CHECK(read(t, &v, 4) < 0 && errno == EINVAL, "short read errno %d", errno);
    int fl = fcntl(t, F_GETFL); fcntl(t, F_SETFL, fl | O_NONBLOCK);
    errno = 0; CHECK(read(t, &v, 8) < 0 && errno == EAGAIN, "nonblock read errno %d", errno);
    struct itimerspec per = { { 0, 20 * 1000000L }, { 0, 20 * 1000000L } };
    CHECKE(timerfd_settime(t, 0, &per, NULL) == 0, "settime periodic");
    msleep(130);
    v = 0; CHECK(read(t, &v, 8) == 8 && v >= 4 && v <= 8, "periodic expirations %llu (want ~6)", (unsigned long long)v);
    CHECKE(timerfd_gettime(t, &cur) == 0, "gettime periodic");
    CHECK(cur.it_interval.tv_sec == 0 && cur.it_interval.tv_nsec == 20 * 1000000L, "interval %ld", cur.it_interval.tv_nsec);
    CHECK(cur.it_value.tv_sec == 0 && cur.it_value.tv_nsec > 0 && cur.it_value.tv_nsec <= 20 * 1000000L, "remaining %ld", cur.it_value.tv_nsec);
    struct itimerspec off = { { 0, 0 }, { 0, 0 } };
    CHECKE(timerfd_settime(t, 0, &off, &old) == 0, "disarm");
    CHECK(old.it_interval.tv_nsec == 20 * 1000000L, "old interval %ld", old.it_interval.tv_nsec);
    struct pollfd pf = { t, POLLIN, 0 };
    CHECK(poll(&pf, 1, 60) == 0, "disarmed timer readable");
    struct timespec now; clock_gettime(CLOCK_REALTIME, &now);
    struct itimerspec ab = { { 0, 0 }, { now.tv_sec, now.tv_nsec } };
    ab.it_value.tv_nsec += 80 * 1000000L; if (ab.it_value.tv_nsec >= 1000000000L) { ab.it_value.tv_sec++; ab.it_value.tv_nsec -= 1000000000L; }
    int tr = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK); CHECKE(tr >= 0, "timerfd REALTIME");
    CHECKE(timerfd_settime(tr, TFD_TIMER_ABSTIME, &ab, NULL) == 0, "settime abstime");
    struct pollfd pr = { tr, POLLIN, 0 }; t0 = mono_ms(); int pn = poll(&pr, 1, 2000); long long el2 = mono_ms() - t0;
    CHECK(pn == 1 && (pr.revents & POLLIN) && el2 >= 40 && el2 < 1500, "abstime poll %d after %lldms", pn, el2);
    v = 0; CHECK(read(tr, &v, 8) == 8 && v == 1, "abstime read %llu", (unsigned long long)v);
    int ep = epoll_create1(0); CHECKE(ep >= 0, "epoll_create1");
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = t }, out[2];
    CHECKE(epoll_ctl(ep, EPOLL_CTL_ADD, t, &ev) == 0, "epoll ADD timerfd");
    its.it_value.tv_nsec = 50 * 1000000L; timerfd_settime(t, 0, &its, NULL);
    t0 = mono_ms(); int en = epoll_wait(ep, out, 2, 2000); long long el3 = mono_ms() - t0;
    CHECK(en == 1 && out[0].data.fd == t && el3 >= 30 && el3 < 1500, "epoll timerfd %d after %lldms", en, el3);
    errno = 0; CHECK(timerfd_create(12345, 0) < 0 && errno == EINVAL, "bad clock errno %d", errno);
    close(ep); close(tr); close(t);
    R(PASS, "one-shot %lldms, periodic, abstime %lldms, poll/epoll %lldms", el, el2, el3);
}
/* signalfd.basic: blocked signals are read from a signalfd (nonblocking
 * EAGAIN, poll/epoll readiness, mask update, blocking read woken by a
 * signal from a child), and rt_sigtimedwait dequeues / times out. */
static int t_sfd_basic(char *d, size_t n) {
    sigset_t m, om; sigemptyset(&m); sigaddset(&m, SIGUSR1); sigaddset(&m, SIGUSR2);
    sigprocmask(SIG_BLOCK, &m, &om);
    sigset_t m1; sigemptyset(&m1); sigaddset(&m1, SIGUSR1);
    int s = signalfd(-1, &m1, SFD_NONBLOCK | SFD_CLOEXEC); if (s < 0) R(SKIP, "signalfd: %s", strerror(errno));
    struct signalfd_siginfo si;
    errno = 0; CHECK(read(s, &si, sizeof(si)) < 0 && errno == EAGAIN, "empty read errno %d", errno);
    kill(getpid(), SIGUSR1);
    struct pollfd pf = { s, POLLIN, 0 };
    CHECK(poll(&pf, 1, 500) == 1 && (pf.revents & POLLIN), "poll after kill 0x%x", pf.revents);
    memset(&si, 0, sizeof(si));
    CHECK(read(s, &si, sizeof(si)) == sizeof(si) && si.ssi_signo == SIGUSR1, "read signo %u", si.ssi_signo);
    errno = 0; CHECK(read(s, &si, sizeof(si)) < 0 && errno == EAGAIN, "second read errno %d", errno);
    errno = 0; CHECK(read(s, &si, 16) < 0 && errno == EINVAL, "short read errno %d", errno);
    kill(getpid(), SIGUSR2);                       /* not in the signalfd mask yet */
    CHECK(poll(&pf, 1, 50) == 0, "SIGUSR2 visible through USR1-only signalfd");
    CHECK(signalfd(s, &m, 0) == s, "mask update");
    int ep = epoll_create1(0); CHECKE(ep >= 0, "epoll_create1");
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = s }, out[2];
    CHECKE(epoll_ctl(ep, EPOLL_CTL_ADD, s, &ev) == 0, "epoll ADD signalfd");
    CHECK(epoll_wait(ep, out, 2, 500) == 1, "epoll ready for pending SIGUSR2");
    CHECK(read(s, &si, sizeof(si)) == sizeof(si) && si.ssi_signo == SIGUSR2, "USR2 signo %u", si.ssi_signo);
    int fl = fcntl(s, F_GETFL); fcntl(s, F_SETFL, fl & ~O_NONBLOCK);
    pid_t par = getpid(), c = fork(); CHECKE(c >= 0, "fork");
    if (c == 0) { msleep(100); kill(par, SIGUSR1); _exit(0); }
    long long t0 = mono_ms(); ssize_t rr = read(s, &si, sizeof(si)); long long el = mono_ms() - t0;
    waitpid(c, NULL, 0);
    CHECK(rr == sizeof(si) && si.ssi_signo == SIGUSR1 && el >= 50 && el < 2000, "blocking read %zd signo %u %lldms", rr, si.ssi_signo, el);
    close(ep); close(s);
    kill(getpid(), SIGUSR2);
    siginfo_t inf; struct timespec z = { 1, 0 };
    int g = sigtimedwait(&m, &inf, &z);
    CHECK(g == SIGUSR2 && inf.si_signo == SIGUSR2, "sigtimedwait got %d", g);
    struct timespec sh = { 0, 60 * 1000000L };
    t0 = mono_ms(); errno = 0; g = sigtimedwait(&m, &inf, &sh); long long el2 = mono_ms() - t0;
    CHECK(g < 0 && errno == EAGAIN && el2 >= 40 && el2 < 1000, "sigtimedwait timeout %d errno %d %lldms", g, errno, el2);
    sigprocmask(SIG_SETMASK, &om, NULL);
    R(PASS, "nonblock/poll/epoll/mask update, blocking read %lldms, sigtimedwait", el);
}
/* epoll.edge: EPOLLET reports once per edge: no repeat while nothing
 * changes, a new edge after more data is written, and after drain+refill. */
static int t_ep_edge(char *d, size_t n) {
    int ep = epoll_create1(0); if (ep < 0) R(SKIP, "epoll_create1: %s", strerror(errno));
    int pf[2]; CHECKE(pipe(pf) == 0, "pipe");
    struct epoll_event ev = { .events = EPOLLIN | EPOLLET, .data.u32 = 1 }, out[2];
    CHECKE(epoll_ctl(ep, EPOLL_CTL_ADD, pf[0], &ev) == 0, "ADD ET");
    CHECK(epoll_wait(ep, out, 2, 0) == 0, "ready before data");
    write(pf[1], "a", 1);
    CHECK(epoll_wait(ep, out, 2, 0) == 1, "first edge");
    long long t0 = mono_ms(); int r = epoll_wait(ep, out, 2, 150); long long el = mono_ms() - t0;
    CHECK(r == 0 && el >= 100, "ET repeated without new data (r=%d after %lldms)", r, el);
    write(pf[1], "b", 1);
    CHECK(epoll_wait(ep, out, 2, 0) == 1, "no edge for new data while still readable");
    char b[8]; CHECK(read(pf[0], b, sizeof(b)) == 2, "drain");
    CHECK(epoll_wait(ep, out, 2, 0) == 0, "ready after drain");
    pid_t c = fork(); if (c == 0) { msleep(100); write(pf[1], "c", 1); _exit(0); }
    t0 = mono_ms(); r = epoll_wait(ep, out, 2, 2000); long long el2 = mono_ms() - t0; waitpid(c, NULL, 0);
    CHECK(r == 1 && el2 >= 50 && el2 < 1500, "edge from writer r=%d %lldms", r, el2);
    /* level-triggered entry on the same pipe still repeats */
    struct epoll_event lv = { .events = EPOLLOUT, .data.u32 = 2 };
    CHECKE(epoll_ctl(ep, EPOLL_CTL_ADD, pf[1], &lv) == 0, "ADD LT");
    r = epoll_wait(ep, out, 2, 0);
    CHECK(r == 1 && out[0].data.u32 == 2, "LT entry r=%d", r);
    CHECK(epoll_wait(ep, out, 2, 0) == 1, "LT entry repeat");
    close(pf[0]); close(pf[1]); close(ep);
    R(PASS, "single edge, no repeat (%lldms), new-data edge, writer edge %lldms", el, el2);
}

/* epoll.edge_strict: EPOLLET semantics verified against Linux: a partial read
 * makes no new EPOLLIN edge, new data does; EPOLLOUT edges when a full pipe
 * gains room; eventfd and socketpair edges; EPOLLET|EPOLLONESHOT re-arm. */
static int ep_n(int ep, struct epoll_event *o, int m) { return epoll_wait(ep, o, m, 0); }
static int t_ep_edge2(char *d, size_t n) {
    int ep = epoll_create1(0); if (ep < 0) R(SKIP, "epoll_create1: %s", strerror(errno));
    struct epoll_event o[4]; char b[256]; int r;
    /* pipe read end */
    int pf[2]; CHECKE(pipe2(pf, O_NONBLOCK) == 0, "pipe2");
    struct epoll_event ev = { .events = EPOLLIN | EPOLLET, .data.u32 = 1 };
    CHECKE(epoll_ctl(ep, EPOLL_CTL_ADD, pf[0], &ev) == 0, "ADD pipe ET");
    CHECK(ep_n(ep, o, 4) == 0, "pipe: ready when empty");
    CHECK(write(pf[1], "0123456789", 10) == 10, "write");
    r = ep_n(ep, o, 4); CHECK(r == 1 && o[0].events == EPOLLIN, "pipe: first edge r=%d ev=0x%x", r, r > 0 ? o[0].events : 0);
    CHECK(ep_n(ep, o, 4) == 0, "pipe: repeated edge");
    CHECK(read(pf[0], b, 4) == 4, "partial read");
    r = ep_n(ep, o, 4); CHECK(r == 0, "pipe: edge after partial read (r=%d)", r);
    CHECK(write(pf[1], "x", 1) == 1, "write2");
    r = ep_n(ep, o, 4); CHECK(r == 1, "pipe: no edge for new data while readable (r=%d)", r);
    CHECK(ep_n(ep, o, 4) == 0, "pipe: repeated edge 2");
    CHECK(read(pf[0], b, sizeof(b)) == 7, "drain");
    CHECK(read(pf[0], b, 1) < 0 && errno == EAGAIN, "EAGAIN after drain");
    CHECK(ep_n(ep, o, 4) == 0, "pipe: ready after drain");
    CHECK(write(pf[1], "y", 1) == 1, "refill");
    CHECK(ep_n(ep, o, 4) == 1, "pipe: no edge after refill");
    read(pf[0], b, sizeof(b));
    /* pipe write end: EPOLLOUT edges */
    struct epoll_event wv = { .events = EPOLLOUT | EPOLLET, .data.u32 = 2 };
    CHECKE(epoll_ctl(ep, EPOLL_CTL_DEL, pf[0], NULL) == 0, "DEL pipe");
    CHECKE(epoll_ctl(ep, EPOLL_CTL_ADD, pf[1], &wv) == 0, "ADD wr ET");
    r = ep_n(ep, o, 4); CHECK(r == 1 && o[0].events == EPOLLOUT, "wr: initial OUT r=%d ev=0x%x", r, r > 0 ? o[0].events : 0);
    CHECK(ep_n(ep, o, 4) == 0, "wr: repeated OUT");
    long tot = 0; for (;;) { long w = write(pf[1], b, sizeof(b)); if (w <= 0) break; tot += w; }
    CHECK(errno == EAGAIN && tot > 0, "fill pipe (tot=%ld errno=%d)", tot, errno);
    CHECK(ep_n(ep, o, 4) == 0, "wr: OUT while full");
    long g = 0; for (;;) { long k = read(pf[0], b, sizeof(b)); if (k <= 0) break; g += k; }
    CHECK(g == tot, "drain full pipe %ld/%ld", g, tot);
    r = ep_n(ep, o, 4); CHECK(r == 1 && (o[0].events & EPOLLOUT), "wr: no OUT edge after room (r=%d)", r);
    CHECK(ep_n(ep, o, 4) == 0, "wr: repeated OUT 2");
    CHECKE(epoll_ctl(ep, EPOLL_CTL_DEL, pf[1], NULL) == 0, "DEL wr");
    /* eventfd */
    int efd = eventfd(0, EFD_NONBLOCK); CHECKE(efd >= 0, "eventfd");
    struct epoll_event fv = { .events = EPOLLIN | EPOLLET, .data.u32 = 3 };
    CHECKE(epoll_ctl(ep, EPOLL_CTL_ADD, efd, &fv) == 0, "ADD efd");
    CHECK(ep_n(ep, o, 4) == 0, "efd: ready at 0");
    unsigned long long one = 1, v;
    write(efd, &one, 8);
    CHECK(ep_n(ep, o, 4) == 1, "efd: first edge");
    CHECK(ep_n(ep, o, 4) == 0, "efd: repeated edge");
    write(efd, &one, 8);
    CHECK(ep_n(ep, o, 4) == 1, "efd: no edge on second write");
    CHECK(read(efd, &v, 8) == 8 && v == 2, "efd read v=%llu", v);
    CHECK(ep_n(ep, o, 4) == 0, "efd: edge after read");
    close(efd);
    /* socketpair: partial recv, then new data */
    int sv[2]; CHECKE(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0, "socketpair");
    struct epoll_event sev = { .events = EPOLLIN | EPOLLET, .data.u32 = 4 };
    CHECKE(epoll_ctl(ep, EPOLL_CTL_ADD, sv[1], &sev) == 0, "ADD sock");
    CHECK(ep_n(ep, o, 4) == 0, "sock: ready when empty");
    write(sv[0], "abcdef", 6);
    CHECK(ep_n(ep, o, 4) == 1, "sock: first edge");
    CHECK(read(sv[1], b, 2) == 2, "sock partial read");
    CHECK(ep_n(ep, o, 4) == 0, "sock: edge after partial read");
    write(sv[0], "g", 1);
    CHECK(ep_n(ep, o, 4) == 1, "sock: no edge for new data");
    CHECK(ep_n(ep, o, 4) == 0, "sock: repeated edge");
    /* ET|ONESHOT */
    sev.events = EPOLLIN | EPOLLET | EPOLLONESHOT; sev.data.u32 = 5;
    CHECKE(epoll_ctl(ep, EPOLL_CTL_MOD, sv[1], &sev) == 0, "MOD oneshot");
    r = ep_n(ep, o, 4); CHECK(r == 1 && o[0].data.u32 == 5, "oneshot: MOD on ready fd r=%d", r);
    write(sv[0], "h", 1);
    CHECK(ep_n(ep, o, 4) == 0, "oneshot: reported while disarmed");
    CHECKE(epoll_ctl(ep, EPOLL_CTL_MOD, sv[1], &sev) == 0, "MOD re-arm");
    CHECK(ep_n(ep, o, 4) == 1, "oneshot: re-arm not reported");
    CHECK(ep_n(ep, o, 4) == 0, "oneshot: twice");
    close(sv[0]); close(sv[1]); close(pf[0]); close(pf[1]); close(ep);
    R(PASS, "pipe IN/OUT, eventfd, socketpair, ET|ONESHOT edges as on Linux (fill %ld)", tot);
}

/* compat.erofs: on a read-only root (boot CD) every modification fails with
 * EROFS (EEXIST/ENOENT where Linux checks those first); skipped if / is writable. */
/* inotify (verified against Linux: tmpfs, ext4/overlay, /dev/shm) */
/* inotify: read all pending events as "MASK:name" tokens ('.' = no name),
 * masks in hex; cookies of MOVED_FROM/TO must pair up. */
static int ino_drain(int fd, char *out, size_t cap, int *wd_seen) {
    char buf[4096] __attribute__((aligned(8))); size_t o = 0; out[0] = 0;
    uint32_t cookie = 0;
    for (;;) {
        ssize_t k = read(fd, buf, sizeof(buf));
        if (k < 0) { if (errno == EAGAIN) break; return -errno; }
        if (k == 0) break;
        for (char *p = buf; p < buf + k; ) {
            struct inotify_event *e = (struct inotify_event *)p;
            if (wd_seen && (e->mask & IN_IGNORED)) *wd_seen = e->wd;
            if (e->mask & IN_MOVED_FROM) cookie = e->cookie;
            if ((e->mask & IN_MOVED_TO) && (!e->cookie || e->cookie != cookie)) return -1000;
            o += snprintf(out + o, cap - o, "%s%x:%s", o ? " " : "", e->mask, e->len ? e->name : ".");
            if (o >= cap) return -1001;
            p += sizeof(*e) + e->len;
        }
    }
    return (int)o;
}
static int ino_dir(const char *dir, char *d, size_t n) {
    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC); CHECKE(fd >= 0, "inotify_init1");
    CHECK(fcntl(fd, F_GETFD) & FD_CLOEXEC, "IN_CLOEXEC not set");
    char b[64], ev[600], p1[256], p2[256], p3[256];
    errno = 0; CHECK(read(fd, b, sizeof(b)) < 0 && errno == EAGAIN, "empty read errno %d", errno);
    int wd = inotify_add_watch(fd, dir, IN_CREATE | IN_DELETE | IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_FROM | IN_MOVED_TO);
    CHECKE(wd >= 1, "add_watch");
    CHECK(inotify_add_watch(fd, dir, IN_CREATE | IN_DELETE | IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_FROM | IN_MOVED_TO) == wd, "re-add gave a new wd");
    int ep = epoll_create1(0); CHECKE(ep >= 0, "epoll_create1");
    struct epoll_event ee = { .events = EPOLLIN, .data.u32 = 77 }, eo[2];
    CHECKE(epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ee) == 0, "epoll ADD inotify fd");
    struct pollfd pf = { fd, POLLIN, 0 };
    CHECK(poll(&pf, 1, 0) == 0 && epoll_wait(ep, eo, 2, 0) == 0, "ready with no events");
    snprintf(p1, sizeof(p1), "%s/a.txt", dir); snprintf(p2, sizeof(p2), "%s/b.txt", dir); snprintf(p3, sizeof(p3), "%s/sub", dir);
    int f = open(p1, O_WRONLY | O_CREAT | O_TRUNC, 0644); CHECKE(f >= 0, "create a.txt");
    CHECK(poll(&pf, 1, 0) == 1 && (pf.revents & POLLIN), "poll after create revents 0x%x", pf.revents);
    CHECK(epoll_wait(ep, eo, 2, 0) == 1 && eo[0].data.u32 == 77, "epoll after create");
    CHECK(write(f, "hi", 2) == 2, "write"); close(f);
    int r = ino_drain(fd, ev, sizeof(ev), NULL); CHECK(r >= 0, "drain1 %d", r);
    CHECK(!strcmp(ev, "100:a.txt 2:a.txt 8:a.txt"), "create/write/close events '%s'", ev);
    CHECK(poll(&pf, 1, 0) == 0, "still readable after drain");
    CHECKE(mkdir(p3, 0755) == 0, "mkdir");
    CHECKE(rename(p1, p2) == 0, "rename");
    CHECKE(unlink(p2) == 0, "unlink");
    CHECKE(rmdir(p3) == 0, "rmdir");
    r = ino_drain(fd, ev, sizeof(ev), NULL); CHECK(r >= 0, "drain2 %d (cookie pairing)", r);
    CHECK(!strcmp(ev, "40000100:sub 40:a.txt 80:b.txt 200:b.txt 40000200:sub"), "mkdir/rename/unlink/rmdir events '%s'", ev);
    /* wakeup from another process */
    pid_t c = fork(); if (c == 0) { msleep(100); int g = open(p1, O_WRONLY | O_CREAT, 0644); if (g >= 0) close(g); _exit(0); }
    long long t0 = mono_ms(); r = epoll_wait(ep, eo, 2, 2000); long long el = mono_ms() - t0; waitpid(c, NULL, 0);
    CHECK(r == 1 && el >= 50 && el < 1500, "epoll wakeup r=%d after %lldms", r, el);
    errno = 0; CHECK(read(fd, b, 1) < 0 && errno == EINVAL, "short buffer errno %d", errno);
    r = ino_drain(fd, ev, sizeof(ev), NULL);
    CHECK(r >= 0 && !strcmp(ev, "100:a.txt 8:a.txt"), "child create events '%s'", ev);
    /* watch on the file itself */
    int wf = inotify_add_watch(fd, p1, IN_MODIFY | IN_CLOSE_WRITE | IN_DELETE_SELF); CHECKE(wf > wd, "add_watch file");
    f = open(p1, O_WRONLY | O_APPEND); CHECKE(f >= 0, "reopen"); write(f, "x", 1); close(f);
    unlink(p1);
    int lw = 0; r = ino_drain(fd, ev, sizeof(ev), &lw);
    CHECK(r >= 0 && !strcmp(ev, "2:a.txt 2:. 8:a.txt 8:. 400:. 8000:. 200:a.txt"), "file watch events '%s'", ev);
    CHECK(lw == wf, "IN_IGNORED wd %d want %d", lw, wf);
    errno = 0; CHECK(inotify_rm_watch(fd, wf) < 0 && errno == EINVAL, "rm_watch auto-removed wd errno %d", errno);
    CHECKE(inotify_rm_watch(fd, wd) == 0, "rm_watch");
    r = ino_drain(fd, ev, sizeof(ev), &lw);
    CHECK(r >= 0 && !strcmp(ev, "8000:.") && lw == wd, "IN_IGNORED after rm_watch '%s'", ev);
    errno = 0; CHECK(inotify_rm_watch(fd, wd) < 0 && errno == EINVAL, "rm_watch twice errno %d", errno);
    snprintf(p3, sizeof(p3), "%s/no_such_dir_x", dir);
    errno = 0; CHECK(inotify_add_watch(fd, p3, IN_CREATE) < 0 && errno == ENOENT, "watch missing errno %d", errno);
    errno = 0; CHECK(inotify_add_watch(ep, dir, IN_CREATE) < 0 && errno == EINVAL, "watch on non-inotify fd errno %d", errno);
    close(ep); close(fd);
    R(PASS, "create/modify/close_write/mkdir/move/delete/self, poll+epoll, wakeup %lldms", el);
}
static int t_inotify_tmp(char *d, size_t n) {
    char dir[64]; snprintf(dir, sizeof(dir), "/tmp/ostest_ino.%d", getpid());
    if (mkdir(dir, 0755) != 0) R(FAIL, "mkdir %s: %s", dir, strerror(errno));
    int r = ino_dir(dir, d, n);
    rmdir(dir);
    return r;
}
/* inotify.volumes: the same sequence on every writable /mnt volume
 * (FAT32 / exFAT go through the same VFS hooks as tmpfs). */
static int t_inotify_vols(char *d, size_t n) {
    char names[8][64]; int k = vol_list(names, 8); if (!k) R(SKIP, "nothing mounted under /mnt");
    char res[300] = ""; int o = 0, tested = 0;
    for (int i = 0; i < k; i++) {
        if (!strcmp(names[i], "badcs")) continue;
        char dir[160]; snprintf(dir, sizeof(dir), "/mnt/%s/ostest_ino.%d", names[i], getpid());
        if (mkdir(dir, 0755) != 0) continue;
        tested++;
        char sub[700]; int r = ino_dir(dir, sub, sizeof(sub));
        rmdir(dir);
        if (r != PASS) R(FAIL, "%s: %s", names[i], sub);
        o += snprintf(res + o, sizeof(res) - o, "%s ok; ", names[i]);
    }
    if (!tested) R(SKIP, "no writable volume under /mnt");
    R(PASS, "%s", res);
}
static int t_cx_erofs(char *d, size_t n) {
    const char *probe = "/ostest_erofs_probe";
    int fd = open(probe, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { close(fd); unlink(probe); R(SKIP, "root is writable"); }
    CHECK(errno == EROFS, "creat on ro root errno %d (%s)", errno, strerror(errno));
    errno = 0; CHECK(mkdir("/ostest_erofs_dir", 0755) < 0 && errno == EROFS, "mkdir errno %d", errno);
    errno = 0; CHECK(mkdir("/bin", 0755) < 0 && errno == EEXIST, "mkdir existing errno %d", errno);
    errno = 0; CHECK(open("/bin/busybox.exe", O_WRONLY) < 0 && errno == EROFS, "open O_WRONLY errno %d", errno);
    errno = 0; CHECK(open("/bin/busybox.exe", O_RDONLY | O_APPEND | O_WRONLY) < 0 && errno == EROFS, "open O_APPEND errno %d", errno);
    errno = 0; CHECK(unlink("/bin/busybox.exe") < 0 && errno == EROFS, "unlink errno %d", errno);
    errno = 0; CHECK(unlink("/ostest_no_such") < 0 && errno == ENOENT, "unlink missing errno %d", errno);
    errno = 0; CHECK(rename("/bin/busybox.exe", "/bin/bb2") < 0 && errno == EROFS, "rename errno %d", errno);
    errno = 0; CHECK(truncate("/bin/busybox.exe", 0) < 0 && errno == EROFS, "truncate errno %d", errno);
    int r = open("/bin/busybox.exe", O_RDONLY); CHECKE(r >= 0, "read-only open still works"); close(r);
    R(PASS, "creat/mkdir/open-w/unlink/rename/truncate -> EROFS, mkdir existing EEXIST");
}

/* ===================== registry ===================== */
/* tty.pty_basic: stdin of the desktop terminal is a pty-like device:
 * isatty, raw-mode termios round-trip, TIOCGWINSZ/TIOCSWINSZ round-trip. */
static int t_pty_basic(char *d, size_t n) {
    int fd = 0;
    if (!isatty(0)) { if (isatty(1)) fd = 1; else R(SKIP, "stdin/stdout not a tty"); }
    struct termios o, r, c, b;
    CHECKE(tcgetattr(fd, &o) == 0, "tcgetattr");
    r = o;
    r.c_lflag &= ~(ICANON | ECHO);
    r.c_cc[VMIN] = 0; r.c_cc[VTIME] = 1;
    CHECKE(tcsetattr(fd, TCSANOW, &r) == 0, "tcsetattr raw");
    int g = tcgetattr(fd, &c);
    tcsetattr(fd, TCSANOW, &o);
    CHECKE(g == 0, "tcgetattr raw");
    CHECK(!(c.c_lflag & (ICANON | ECHO)), "raw lflag 0x%x kept ICANON/ECHO", (unsigned)c.c_lflag);
    CHECK(c.c_cc[VMIN] == 0 && c.c_cc[VTIME] == 1, "VMIN %d VTIME %d", c.c_cc[VMIN], c.c_cc[VTIME]);
    CHECKE(tcgetattr(fd, &b) == 0, "tcgetattr restored");
    CHECK((b.c_lflag & (ICANON | ECHO)) == (o.c_lflag & (ICANON | ECHO)), "restore lflag 0x%x vs 0x%x", (unsigned)b.c_lflag, (unsigned)o.c_lflag);
    struct winsize w0, w1, ws;
    CHECKE(ioctl(fd, TIOCGWINSZ, &w0) == 0, "TIOCGWINSZ");
    CHECK(w0.ws_row > 0 && w0.ws_col > 0, "winsize %dx%d", w0.ws_col, w0.ws_row);
    ws = w0; ws.ws_row = 33; ws.ws_col = 77;
    CHECKE(ioctl(fd, TIOCSWINSZ, &ws) == 0, "TIOCSWINSZ");
    int g2 = ioctl(fd, TIOCGWINSZ, &w1);
    ioctl(fd, TIOCSWINSZ, &w0);
    CHECK(g2 == 0 && w1.ws_row == 33 && w1.ws_col == 77, "winsize round-trip got %dx%d", w1.ws_col, w1.ws_row);
    const char *te = getenv("TERM"), *li = getenv("LINES"), *co = getenv("COLUMNS");
    R(PASS, "fd %d %dx%d TERM=%s LINES=%s COLUMNS=%s", fd, w0.ws_col, w0.ws_row,
      te ? te : "-", li ? li : "-", co ? co : "-");
}

/* ===================== compat (exec/shebang/modes/stdio/proc/tmp) ===================== */
#define CB_NARG 60
#define CB_NENV 90
static void cb_str(char *b, int i, int len, char tag) { int o = snprintf(b, 16, "%c%03d=", tag, i); for (int k = o; k < len; k++) b[k] = 'a' + (i + k) % 26; b[len] = 0; }
static int cb_child(int argc, char **argv) {
    char b[260]; int bad = 0, envs = 0;
    if (argc != CB_NARG + 2) return 20;
    for (int i = 0; i < CB_NARG; i++) { cb_str(b, i, 200, 'A'); if (strcmp(argv[i + 2], b)) bad++; }
    for (int i = 0; i < CB_NENV; i++) { cb_str(b, i, 150, 'E'); char k[8]; snprintf(k, sizeof(k), "E%03d", i); const char *v = getenv(k); if (v && !strcmp(v, b + 5)) envs++; }
    return bad ? 21 : (envs == CB_NENV ? 9 : 22);
}
static int t_cx_bigexec(char *d, size_t n) {
    static char ab[CB_NARG][201], eb[CB_NENV][151];
    char *av[CB_NARG + 3], *ev[CB_NENV + 2];
    av[0] = (char *)self_path; av[1] = "--child-big";
    for (int i = 0; i < CB_NARG; i++) { cb_str(ab[i], i, 200, 'A'); av[i + 2] = ab[i]; } av[CB_NARG + 2] = NULL;
    for (int i = 0; i < CB_NENV; i++) { cb_str(eb[i], i, 150, 'E'); ev[i] = eb[i]; } ev[CB_NENV] = "PATH=/bin"; ev[CB_NENV + 1] = NULL;
    int st; pid_t c = fork(); CHECKE(c >= 0, "fork");
    if (!c) { execve(self_path, av, ev); _exit(127); }
    waitpid(c, &st, 0); CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 9, "fork+execve child status 0x%x", st);
    c = vfork(); CHECKE(c >= 0, "vfork");
    if (!c) { execve(self_path, av, ev); _exit(127); }
    waitpid(c, &st, 0); CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 9, "vfork+execve child status 0x%x", st);
    int pf[2]; CHECKE(pipe2(pf, O_CLOEXEC) == 0, "pipe2");
    c = fork(); CHECKE(c >= 0, "fork");
    if (!c) { char *a2[] = { "x", NULL }; execve("/nonexistent_ostest", a2, ev); _exit(write(pf[1], "k", 1) == 1 ? 0 : 5); }
    close(pf[1]); waitpid(c, &st, 0); close(pf[0]);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "failed execve closed CLOEXEC fd (0x%x)", st);
    R(PASS, "%d args x200B + %d env x150B via fork/vfork execve; failed execve keeps CLOEXEC", CB_NARG, CB_NENV);
}
static int t_cx_shebang(char *d, size_t n) {
    const char *body[3] = { "#!/bin/sh\necho \"SB1:$1\"\nexit 11\n", "#!/bin/sh -e\necho \"SB2:$1\"\nexit 12\n", "#!/usr/bin/env sh\necho \"SB3:$1\"\nexit 13\n" };
    for (int i = 0; i < 3; i++) {
        char p[200], out[256]; snprintf(p, sizeof(p), "%s/sb%d.sh", tmpdir, i);
        CHECKE(put_file(p, body[i], strlen(body[i])) == 0, "write script"); chmod(p, 0755);
        char *av[] = { p, "zz", NULL };
        int r = run_capture(av, out, sizeof(out), 5000);
        char want[16]; snprintf(want, sizeof(want), "SB%d:zz", i + 1);
        CHECK(r == 11 + i && strstr(out, want), "script %d rc=%d out='%.60s'", i + 1, r, out);
    }
    R(PASS, "#!/bin/sh, #!/bin/sh -e, #!/usr/bin/env sh");
}
static int t_cx_modes(char *d, size_t n) {
    char p[200]; tpath(p, sizeof(p), "modef"); CHECKE(put_file(p, "x", 1) == 0, "create");
    struct stat st; CHECKE(chmod(p, 0751) == 0, "chmod 0751"); stat(p, &st); int m1 = st.st_mode & 07777, a1 = access(p, X_OK);
    CHECKE(chmod(p, 0640) == 0, "chmod 0640"); stat(p, &st); int m2 = st.st_mode & 07777, a2 = access(p, X_OK);
    CHECK(m1 == 0751 && m2 == 0640 && a1 == 0 && a2 == -1, "modes %o/%o access %d/%d", m1, m2, a1, a2);
    CHECK(stat("/bin/busybox.exe", &st) == 0 && (st.st_mode & 0111) && access("/bin/busybox.exe", X_OK) == 0, "/bin/busybox.exe not executable (mode %o)", st.st_mode & 0777);
    R(PASS, "chmod sticks (0751/0640), X_OK honest, /bin exec bits");
}
static int t_cx_close_stdio(char *d, size_t n) {
    int st; pid_t c = fork(); CHECKE(c >= 0, "fork");
    if (!c) { close(0); int f = open("/dev/null", O_RDONLY); int c2 = close(2); int g = open("/dev/null", O_WRONLY); int h = dup(1); _exit((f == 0 && c2 == 0 && g == 2 && h >= 3) ? 0 : 10 + (f & 7)); }
    waitpid(c, &st, 0); CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "child status 0x%x", st);
    R(PASS, "close(0)/open->0, close(2)/open->2");
}
static int t_cx_proc(char *d, size_t n) {
    const char *pf[3] = { "/proc/cpuinfo", "/proc/meminfo", "/proc/self/stat" }, *key[3] = { "processor", "MemTotal:", " (" };
    char b[4096]; char res[160] = "";
    for (int i = 0; i < 3; i++) {
        ssize_t r = get_file(pf[i], b, sizeof(b) - 1); CHECK(r > 0, "%s: %s", pf[i], strerror(errno)); b[r] = 0;
        CHECK(strstr(b, key[i]) != NULL, "%s lacks '%s'", pf[i], key[i]);
        snprintf(res + strlen(res), sizeof(res) - strlen(res), "%s=%zdB ", pf[i] + 6, r);
    }
    CHECK(atoi(b) == getpid(), "/proc/self/stat pid %d != %d", atoi(b), getpid());
    R(PASS, "%s", res);
}
static int t_cx_tmp_big(char *d, size_t n) {
    char p[200], q[200]; snprintf(p, sizeof(p), "/tmp/ostest_big.%d", getpid()); snprintf(q, sizeof(q), "%s.r", p);
    static uint8_t blk[65536]; int fd = open(p, O_CREAT | O_TRUNC | O_WRONLY, 0644); CHECKE(fd >= 0, "open /tmp");
    uint32_t h1 = 2166136261u, h2 = 2166136261u;
    for (int i = 0; i < 64; i++) { fill(blk, sizeof(blk), i); h1 = fnv(blk, sizeof(blk), h1); if (writeall(fd, blk, sizeof(blk)) != (ssize_t)sizeof(blk)) { close(fd); unlink(p); R(FAIL, "write chunk %d: %s", i, strerror(errno)); } }
    close(fd);
    CHECKE(rename(p, q) == 0, "rename in /tmp");
    fd = open(q, O_RDONLY); CHECKE(fd >= 0, "reopen");
    for (int i = 0; i < 64; i++) { if (readall(fd, blk, sizeof(blk)) != (ssize_t)sizeof(blk)) break; h2 = fnv(blk, sizeof(blk), h2); }
    close(fd); unlink(q);
    CHECK(h1 == h2, "4 MB /tmp readback hash mismatch");
    R(PASS, "4 MB file in /tmp written, renamed, read back");
}

/* ===================== AF_UNIX ===================== */
static int t_unix_pair_stream(char *d, size_t n) {
    int sv[2]; CHECKE(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");
    pid_t c = fork(); CHECKE(c >= 0, "fork");
    if (c == 0) {
        close(sv[1]); static uint8_t b[200000]; fill(b, sizeof(b), 7);
        _exit(writeall(sv[0], b, sizeof(b)) == (ssize_t)sizeof(b) ? 0 : 1);
    }
    close(sv[0]);
    static uint8_t got[200000], want[200000]; fill(want, sizeof(want), 7);
    ssize_t r = readall(sv[1], got, sizeof(got)); char x; ssize_t e = read(sv[1], &x, 1);
    int st = 0; waitpid(c, &st, 0); close(sv[1]);
    CHECK(r == (ssize_t)sizeof(got) && !memcmp(got, want, sizeof(got)), "200 KB mismatch (r=%zd)", r);
    CHECK(e == 0, "no EOF after writer exit (%zd)", e);
    CHECK(WIFEXITED(st) && !WEXITSTATUS(st), "writer status %x", st);
    R(PASS, "200 KB through socketpair + EOF");
}
static int t_unix_pair_dgram(char *d, size_t n) {
    int sv[2]; CHECKE(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0, "socketpair dgram");
    send(sv[0], "one", 3, 0); send(sv[0], "second", 6, 0);
    char b[16]; ssize_t a1 = recv(sv[1], b, sizeof(b), 0), a2 = recv(sv[1], b, 3, 0);
    errno = 0; ssize_t a3 = recv(sv[1], b, sizeof(b), MSG_DONTWAIT); int e3 = errno;
    close(sv[0]); close(sv[1]);
    CHECK(a1 == 3 && a2 == 3 && !memcmp(b, "sec", 3), "boundaries %zd %zd", a1, a2);
    CHECK(a3 == -1 && e3 == EAGAIN, "empty recv %zd errno %d", a3, e3);
    R(PASS, "datagram boundaries, truncation, EAGAIN");
}
static int unix_srv(char *d, size_t n, struct sockaddr_un *a, socklen_t al, const char *what) {
    int l = socket(AF_UNIX, SOCK_STREAM, 0); CHECKE(l >= 0, "socket");
    CHECKE(bind(l, (void *)a, al) == 0, "bind");
    int l2 = socket(AF_UNIX, SOCK_STREAM, 0); errno = 0;
    int b2 = bind(l2, (void *)a, al); int e2 = errno; close(l2);
    CHECK(b2 == -1 && e2 == EADDRINUSE, "second bind %d errno %d", b2, e2);
    CHECKE(listen(l, 4) == 0, "listen");
    pid_t c = fork(); CHECKE(c >= 0, "fork");
    if (c == 0) {
        int s = socket(AF_UNIX, SOCK_STREAM, 0);
        if (connect(s, (void *)a, al)) _exit(3);
        write(s, "ping", 4); char b[8]; int k = read(s, b, sizeof(b));
        _exit(k == 4 && !memcmp(b, "pong", 4) ? 0 : 4);
    }
    struct pollfd p = { l, POLLIN, 0 }; int pr = poll(&p, 1, 3000);
    int s = pr == 1 ? accept(l, NULL, NULL) : -1;
    char b[8] = ""; ssize_t k = s >= 0 ? read(s, b, sizeof(b)) : -1;
    if (s >= 0) write(s, "pong", 4);
    int st = 0; waitpid(c, &st, 0); if (s >= 0) close(s); close(l);
    CHECK(pr == 1 && s >= 0, "poll/accept %d %d", pr, s);
    CHECK(k == 4 && !memcmp(b, "ping", 4), "server read %zd", k);
    CHECK(WIFEXITED(st) && !WEXITSTATUS(st), "client status %x", st);
    int x = socket(AF_UNIX, SOCK_STREAM, 0); errno = 0;
    int cr = connect(x, (void *)a, al); int ce = errno; close(x);
    CHECK(cr == -1 && (ce == ECONNREFUSED || ce == ENOENT), "connect after close %d errno %d", cr, ce);
    R(PASS, "%s: bind/listen/accept/connect, EADDRINUSE, refused after close", what);
}
static int t_unix_path(char *d, size_t n) {
    struct sockaddr_un a; memset(&a, 0, sizeof(a)); a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s/ux.sock", tmpdir); unlink(a.sun_path);
    int r = unix_srv(d, n, &a, sizeof(a), "path"); unlink(a.sun_path); return r;
}
static int t_unix_abstract(char *d, size_t n) {
    struct sockaddr_un a; memset(&a, 0, sizeof(a)); a.sun_family = AF_UNIX;
    memcpy(a.sun_path + 1, "ostest_abs", 10);
    return unix_srv(d, n, &a, offsetof(struct sockaddr_un, sun_path) + 11, "abstract");
}
static int t_unix_scm_rights(char *d, size_t n) {
    int sv[2], pp[2]; CHECKE(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair"); CHECKE(pipe(pp) == 0, "pipe");
    char cb[CMSG_SPACE(sizeof(int))]; memset(cb, 0, sizeof(cb));
    struct iovec iov = { "F", 1 }; struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = cb, .msg_controllen = sizeof(cb) };
    struct cmsghdr *cm = CMSG_FIRSTHDR(&m); cm->cmsg_level = SOL_SOCKET; cm->cmsg_type = SCM_RIGHTS; cm->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cm), &pp[1], sizeof(int));
    CHECKE(sendmsg(sv[0], &m, 0) == 1, "sendmsg"); close(pp[1]);
    char rb[4], rc[CMSG_SPACE(sizeof(int))]; struct iovec ri = { rb, sizeof(rb) };
    struct msghdr r = { .msg_iov = &ri, .msg_iovlen = 1, .msg_control = rc, .msg_controllen = sizeof(rc) };
    ssize_t k = recvmsg(sv[1], &r, 0); struct cmsghdr *rcm = CMSG_FIRSTHDR(&r); int nfd = -1;
    if (k == 1 && rcm && rcm->cmsg_level == SOL_SOCKET && rcm->cmsg_type == SCM_RIGHTS) memcpy(&nfd, CMSG_DATA(rcm), sizeof(int));
    CHECK(nfd >= 0, "no fd received (k=%zd)", k);
    write(nfd, "via", 3); close(nfd);
    char pb[8] = ""; ssize_t a = read(pp[0], pb, sizeof(pb)), b = read(pp[0], pb + 3, 1);
    close(pp[0]); close(sv[0]); close(sv[1]);
    CHECK(a == 3 && !memcmp(pb, "via", 3) && b == 0, "pipe via passed fd: %zd %zd", a, b);
    R(PASS, "pipe write end passed with SCM_RIGHTS");
}

static const struct test tests[] = {
    { "sys", "uname", t_uname, 5, 0 }, { "sys", "pids", t_pids, 5, 0 }, { "sys", "ids", t_ids, 5, 0 },
    { "sys", "clock", t_clock, 10, 0 }, { "sys", "nanosleep", t_nanosleep, 10, 0 },
    { "sys", "sysinfo", t_sysinfo, 5, 0 }, { "sys", "getrandom", t_getrandom, 5, 0 },
    { "time", "rtc", t_time, 5, 0 },
    { "mem", "malloc_big", t_malloc_big, 20, 0 }, { "mem", "mmap_anon", t_mmap_anon, 15, 0 }, { "mem", "mmap_file", t_mmap_file, 10, 0 },
    { "mem", "mmap_shared_anon", t_mmap_shared, 10, 0 }, { "mem", "mmap_shared_file", t_mmap_shared_file, 10, 0 }, { "mem", "mmap_shared_coherent", t_mmap_shared_coh, 10, 0 },
    { "mem", "munmap", t_munmap, 10, 0 }, { "mem", "mprotect", t_mprotect, 10, 0 }, { "mem", "brk", t_brk, 10, 0 },
    { "mem", "cow_fork", t_cow, 10, 0 }, { "mem", "exhaust", t_mem_exhaust, 60, TF_DESTRUCT },
    { "proc", "exit_codes", t_exit_codes, 10, 0 }, { "proc", "exec_busybox", t_exec_busybox, 20, 0 },
    { "proc", "argv_env", t_argv_env, 10, 0 }, { "proc", "many_children", t_many_children, 30, 0 },
    { "proc", "zombies", t_zombies, 15, 0 }, { "proc", "vfork", t_vfork, 10, 0 },
    { "sig", "kill_handler", t_sig_kill, 10, 0 }, { "sig", "sigchld", t_sigchld, 10, 0 }, { "sig", "sigpipe", t_sigpipe, 10, 0 },
    { "sig", "sigalrm", t_sigalrm, 10, 0 }, { "sig", "sigprocmask", t_sigmask, 10, 0 }, { "sig", "sigint_pgrp", t_sigint_pgrp, 10, 0 },
    { "sig", "eintr", t_eintr, 10, 0 },
    { "pipe", "basic", t_pipe_basic, 5, 0 }, { "pipe", "blocking", t_pipe_block, 10, 0 }, { "pipe", "large_8mb", t_pipe_large, 60, 0 },
    { "pipe", "concurrent", t_pipe_concurrent, 30, 0 }, { "pipe", "nonblock", t_pipe_nonblock, 15, 0 },
    { "fs", "rw_seek", t_fs_rw, 10, 0 }, { "fs", "append", t_fs_append, 10, 0 }, { "fs", "truncate", t_fs_truncate, 15, 0 },
    { "fs", "rename", t_fs_rename, 10, 0 }, { "fs", "dirs", t_fs_dirs, 15, 0 }, { "fs", "readdir_stat", t_fs_readdir_stat, 10, 0 }, { "fs", "readdir_many", t_fs_readdir_many, 120, 0 },
    { "fs", "many_files", t_fs_many, 120, 0 }, { "fs", "large_24mb", t_fs_large, 180, 0 },
    { "fs", "readback_busybox", t_fs_readback_busybox, 15, 0 }, { "fs", "fd_offsets", t_fs_fd_offsets, 10, 0 }, { "fs", "seek_far_read", t_fs_seek_far_read, 30, 0 },
    { "vol", "read_all", t_vol_read, 120, 0 }, { "vol", "rw_if_writable", t_vol_rw, 60, 0 }, { "vol", "ext_write", t_vol_extwrite, 60, 0 }, { "vol", "ext_ns", t_vol_extns, 120, 0 }, { "vol", "ext_dir", t_vol_extdir, 60, 0 }, { "vol", "ext_htree", t_vol_exthtree, 120, 0 }, { "vol", "ext_sparse", t_vol_extsparse, 60, 0 }, { "vol", "ext_deep", t_vol_extdeep, 120, 0 }, { "vol", "ext_journal", t_vol_extjournal, 120, 0 }, { "vol", "ext_e4dw", t_vol_exte4dw, 60, 0 }, { "vol", "ext_links", t_vol_extlinks, 120, 0 }, { "vol", "mount_slots", t_vol_mountslots, 30, 0 },
    { "vol", "ext_meta", t_vol_extmeta, 180, 0 },
    { "dev", "null_zero", t_dev_null_zero, 5, 0 }, { "dev", "random", t_dev_random, 5, 0 }, { "dev", "tty", t_dev_tty, 5, 0 },
    { "dev", "fb0", t_dev_fb, 10, 0 }, { "dev", "evdev", t_dev_evdev, 10, 0 }, { "dev", "uinput", t_dev_uinput, 10, 0 },
    { "dev", "wm", t_dev_wm, 10, 0 },
    { "net", "sockets", t_net_socket, 5, 0 }, { "net", "udp_loopback", t_net_udp_lo, 5, 0 }, { "net", "tcp_loopback", t_net_tcp_lo, 10, 0 }, { "net", "links_http", t_net_links_http, 60, 0 },
    { "net", "dns_connect", t_net_dns, 20, TF_EXPLICIT },
    { "unix", "pair_stream", t_unix_pair_stream, 15, 0 }, { "unix", "pair_dgram", t_unix_pair_dgram, 5, 0 },
    { "unix", "path_listen", t_unix_path, 15, 0 }, { "unix", "abstract_listen", t_unix_abstract, 15, 0 },
    { "unix", "scm_rights", t_unix_scm_rights, 10, 0 },
    { "sc", "links", t_sc_links, 10, 0 }, { "sc", "fifo", t_sc_fifo, 15, 0 }, { "sc", "flock", t_sc_flock, 15, 0 },
    { "sc", "fallocate_pv", t_sc_fallocate, 15, 0 }, { "sc", "utimes", t_sc_times, 10, 0 },
    { "sc", "waitid", t_sc_waitid, 15, 0 }, { "sc", "prio_affinity", t_sc_sched, 10, 0 },
    { "lnk", "symlinks", t_lk_sym, 30, 0 }, { "lnk", "hardlinks", t_lk_hard, 20, 0 }, { "lnk", "perms", t_lk_perm, 20, 0 },
    { "tty", "pty_basic", t_pty_basic, 10, 0 },
    { "misc", "cwd", t_cwd, 5, 0 }, { "misc", "dup", t_dup, 5, 0 }, { "misc", "fcntl", t_fcntl, 5, 0 },
    { "misc", "poll", t_poll, 10, 0 }, { "misc", "select", t_select, 10, 0 }, { "misc", "eventfd", t_eventfd, 5, 0 },
    { "epoll", "pipe", t_ep_pipe, 15, 0 }, { "epoll", "unix_socket", t_ep_sock, 10, 0 },
    { "epoll", "ctl_errors", t_ep_ctl, 10, 0 }, { "epoll", "tty", t_ep_tty, 5, 0 },
    { "epoll", "edge", t_ep_edge, 10, 0 }, { "epoll", "edge_strict", t_ep_edge2, 10, 0 },
    { "futex", "wait_wake", t_fx_wait_wake, 15, 0 }, { "futex", "private_requeue", t_fx_requeue, 15, 0 },
    { "futex", "bitset_abs", t_fx_bitset, 10, 0 },
    { "timerfd", "basic", t_tfd_basic, 15, 0 }, { "signalfd", "basic", t_sfd_basic, 15, 0 },
    { "compat", "big_exec", t_cx_bigexec, 15, 0 }, { "compat", "shebang", t_cx_shebang, 15, 0 },
    { "compat", "modes", t_cx_modes, 5, 0 }, { "compat", "close_stdio", t_cx_close_stdio, 5, 0 },
    { "compat", "proc_files", t_cx_proc, 5, 0 }, { "compat", "erofs", t_cx_erofs, 5, 0 }, { "inotify", "tmpfs", t_inotify_tmp, 10, 0 }, { "inotify", "volumes", t_inotify_vols, 30, 0 }, { "compat", "tmp_big", t_cx_tmp_big, 60, 0 },
};
#define NTESTS (int)(sizeof(tests) / sizeof(tests[0]))

/* ===================== runner ===================== */
static char report[262144]; static size_t rlen;
static void out(const char *fmt, ...) {
    char b[1200]; va_list ap; va_start(ap, fmt); int k = vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
    if (k < 0) return;
    if (k >= (int)sizeof(b)) k = sizeof(b) - 1;
    fwrite(b, 1, k, stdout); fflush(stdout);
    if (rlen + k < sizeof(report)) { memcpy(report + rlen, b, k); rlen += k; }
}
static void rm_tree(const char *p) {
    struct stat st; if (lstat(p, &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        DIR *D = opendir(p); struct dirent *e; char c[1024];
        while (D && (e = readdir(D))) { if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue; snprintf(c, sizeof(c), "%s/%s", p, e->d_name); rm_tree(c); }
        if (D) closedir(D);
        rmdir(p);
    } else unlink(p);
}
static int in_list(const char *sel, const char *g) {
    if (!sel) return 0;
    char b[256], k[64]; snprintf(b, sizeof(b), ",%s,", sel); snprintf(k, sizeof(k), ",%s,", g); return strstr(b, k) != NULL;
}
static int boot_number(void) {
    const char *ps[] = { "/BOOTLOG.TXT", "/mnt/ventoy/INDEVOS_BOOTLOG.TXT" };
    for (int i = 0; i < 2; i++) { char b[200] = ""; if (get_file(ps[i], b, sizeof(b) - 1) > 0) { char *h = strstr(b, "boot #"); if (h) return atoi(h + 6); } }
    return -1;
}
static int run_one(const struct test *t, char *detail, size_t dn, long long *ms) {
    char rf[160]; snprintf(rf, sizeof(rf), "%s/result.%s.%s", tmpdir, t->group, t->name);
    unlink(rf);
    long long t0 = mono_ms();
    pid_t c = fork();
    if (c < 0) { snprintf(detail, dn, "runner fork: %s", strerror(errno)); return FAIL; }
    if (c == 0) {
        setpgid(0, 0); signal(SIGPIPE, SIG_DFL);
        char d[500] = ""; alarm(t->timeout + 2);
        int r = t->fn(d, sizeof(d));
        put_file(rf, d, strlen(d)); _exit(r);
    }
    setpgid(c, c);
    if (verbose > 1) out("     [runner] forked pid %d\n", c);
    /* Watchdog process: sleeps for the timeout, then SIGKILLs the test's
     * process group. The runner just blocks in waitpid(-1), so a hung test
     * cannot stall the run even if signals do not interrupt blocking calls. */
    int st = 0, done = 0, timed_out = 0;
    pid_t wd = fork();
    if (wd == 0) { msleep(t->timeout * 1000); kill(-c, SIGKILL); kill(c, SIGKILL); _exit(0); }
    while (!done) {
        int s2; pid_t w = waitpid(-1, &s2, 0);
        if (w == c) { st = s2; done = 1; break; }
        if (w == wd && wd > 0) { timed_out = 1; wd = -1; if (waitpid(c, &st, 0) == c) done = 1; break; }
        if (w < 0 && errno != EINTR) break;
    }
    if (wd > 0) { kill(wd, SIGKILL); waitpid(wd, NULL, 0); }
    if (timed_out) done = 0;
    *ms = mono_ms() - t0;
    if (verbose > 1) out("     [runner] %s after %lldms\n", done ? "reaped" : "timed out", *ms);
    if (!done) {
        kill(-c, SIGKILL); kill(c, SIGKILL);
        long long t1 = mono_ms(); int gone = 0;
        while (mono_ms() - t1 < 2000) { pid_t w2 = waitpid(c, &st, WNOHANG); if (w2 == c || (w2 < 0 && errno == ECHILD)) { gone = 1; break; } msleep(5); }
        snprintf(detail, dn, "timeout after %ds%s", t->timeout, gone ? "" : ", process could not be killed");
        kill(-c, SIGKILL);
        return FAIL;
    }
    kill(-c, SIGKILL);                       /* leftovers in the test's process group */
    while (waitpid(-c, NULL, WNOHANG) > 0) {}
    ssize_t r = get_file(rf, detail, dn - 1); detail[r > 0 ? r : 0] = 0; unlink(rf);
    if (WIFSIGNALED(st)) {
        int s = WTERMSIG(st);
        snprintf(detail + strlen(detail), dn - strlen(detail), "%s%s (signal %d %s)", detail[0] ? " " : "", s == SIGALRM ? "timeout" : "crashed", s, strsignal(s));
        return FAIL;
    }
    int rc = WEXITSTATUS(st);
    if (rc > SKIP) { snprintf(detail + strlen(detail), dn - strlen(detail), " (exit %d)", rc); return FAIL; }
    return rc;
}

int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "--child-big")) return cb_child(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "--child-argv")) {
        printf("argc=%d", argc); for (int i = 2; i < argc; i++) printf("|%s", argv[i]);
        const char *e = getenv("OSTEST_ENV"); printf("|env=%s\n", e ? e : "(null)"); return 7;
    }
    const char *sel = NULL; int list = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-g") && i + 1 < argc) sel = argv[++i];
        else if (!strncmp(argv[i], "-g", 2) && argv[i][2]) sel = argv[i] + 2;
        else if (!strcmp(argv[i], "-l")) list = 1;
        else if (!strcmp(argv[i], "-vv")) verbose = 2;
        else if (!strcmp(argv[i], "-v")) { verbose++; if (i + 1 < argc && !strcmp(argv[i + 1], "draw")) { draw = 1; i++; } }
        else if (!strcmp(argv[i], "--destructive")) destructive = 1;
        else if (!strcmp(argv[i], "--http-port") && i + 1 < argc) http_port = atoi(argv[++i]);
        else { fprintf(stderr, "usage: ostest [-g group,group] [-l] [-v [draw]] [--destructive] [--http-port N]\n"); return 255; }
    }
    if (argv[0] && strchr(argv[0], '/') && access(argv[0], F_OK) == 0) self_path = argv[0];
    if (list) {
        for (int i = 0; i < NTESTS; i++) printf("%-5s %-18s timeout %3ds%s%s\n", tests[i].group, tests[i].name, tests[i].timeout, tests[i].flags & TF_EXPLICIT ? "  [only with -g net]" : "", tests[i].flags & TF_DESTRUCT ? "  [--destructive]" : "");
        printf("%d tests\n", NTESTS);
        return 0;
    }
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/ostest.%d", getpid());
    mkdir("/tmp", 0777);
    if (mkdir(tmpdir, 0755) != 0) { fprintf(stderr, "ostest: cannot create %s: %s\n", tmpdir, strerror(errno)); return 255; }
    signal(SIGPIPE, SIG_IGN);
    struct utsname u; uname(&u); time_t now = time(NULL); struct tm tm; gmtime_r(&now, &tm);
    out("ostest: %s %s %s %s | boot #%d | %04d-%02d-%02d %02d:%02d:%02d UTC | groups=%s%s\n", u.sysname, u.release, u.version, u.machine, boot_number(),
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, sel ? sel : "all", destructive ? " +destructive" : "");
    int cnt[3] = {0}, gc[3] = {0}; char gsum[1200] = ""; int go = 0; const char *lastg = NULL;
    long long t_all = mono_ms();
    for (int i = 0; i < NTESTS; i++) {
        const struct test *t = &tests[i];
        if (sel && !in_list(sel, t->group)) continue;
        if (lastg && strcmp(lastg, t->group)) { go += snprintf(gsum + go, sizeof(gsum) - go, "  %-5s pass %d fail %d skip %d\n", lastg, gc[0], gc[1], gc[2]); memset(gc, 0, sizeof(gc)); }
        lastg = t->group;
        char detail[700]; int res; long long ms = 0;
        if ((t->flags & TF_EXPLICIT) && !in_list(sel, t->group)) { res = SKIP; snprintf(detail, sizeof(detail), "needs network: run with -g %s", t->group); }
        else if ((t->flags & TF_DESTRUCT) && !destructive) { res = SKIP; snprintf(detail, sizeof(detail), "needs --destructive"); }
        else { if (verbose) out("RUN  %s.%s\n", t->group, t->name); res = run_one(t, detail, sizeof(detail), &ms); }
        for (char *q = detail; *q; q++) if (*q == '\n' || *q == '\r') *q = ' ';
        if (verbose && ms) out("%s %s.%s (%s) [%lldms]\n", res == PASS ? "PASS" : res == FAIL ? "FAIL" : "SKIP", t->group, t->name, detail, ms);
        else out("%s %s.%s (%s)\n", res == PASS ? "PASS" : res == FAIL ? "FAIL" : "SKIP", t->group, t->name, detail);
        cnt[res]++; gc[res]++;
    }
    if (lastg) go += snprintf(gsum + go, sizeof(gsum) - go, "  %-5s pass %d fail %d skip %d\n", lastg, gc[0], gc[1], gc[2]);
    out("---- per group ----\n%s", gsum);
    out("SUMMARY: %d passed, %d failed, %d skipped (%d tests, %llds)\n", cnt[0], cnt[1], cnt[2], cnt[0] + cnt[1] + cnt[2], (mono_ms() - t_all) / 1000);
    rm_tree(tmpdir);
    const char *rp = "/home/user/ostest_report.txt";
    if (put_file(rp, report, rlen) != 0) { rp = "/ostest_report.txt"; if (put_file(rp, report, rlen) != 0) rp = NULL; }
    printf("report: %s\n", rp ? rp : "(could not write report)");
    sync();
    return cnt[1] > 254 ? 254 : cnt[1];
}
