/*
 * linux_syscall.c - Linux x86-64 syscall ABI for inDevOS.
 *
 * Entry: the SYSCALL instruction -> syscall_entry (syscallasm.asm) ->
 * linux_syscall_dispatch(frame).  rax = number; rdi, rsi, rdx, r10, r8, r9 =
 * arguments; the result (or -errno) goes back in rax; rcx/r11 are clobbered
 * by SYSCALL/SYSRET as on Linux.
 *
 * Goal (phase 1): run statically linked musl binaries unmodified.  Most calls
 * are thin adapters over the existing sys_* implementations in syscall.c;
 * the adapters translate flag values (O_*, F_*), structure layouts (struct
 * stat, dirent64, termios, utsname, timespec) and path handling (dirfd,
 * "." / ".." normalisation) between Linux and inDevOS.
 *
 * Anything not implemented returns -ENOSYS and is logged to serial once per
 * syscall number ("linux: unimplemented syscall ..."), so running a new
 * program shows exactly what it still needs.
 */
#include "linux_syscall.h"
#include "syscall.h"
#include "syscall_internal.h"
#include "exec.h"
#include "vfs.h"
#include "mem.h"
#include "string.h"
#include "stdio.h"
#include "serial.h"
#include "uaccess.h"
#include "utils.h"
#include "thread.h"
#include "ktime.h"
#include "rtc.h"

bool linux_syscall_trace = false;
bool linux_stdout_mirror = false;  /* console output already reaches serial */

extern void fb_console_get_size(uint32_t *cols, uint32_t *rows);

/* ------------------------------------------------------------------------ */
/* Linux constants (x86-64)                                                 */
/* ------------------------------------------------------------------------ */
#define LX_O_ACCMODE   0x3
#define LX_O_CREAT     0x40
#define LX_O_EXCL      0x80
#define LX_O_NOCTTY    0x100
#define LX_O_TRUNC     0x200
#define LX_O_APPEND    0x400
#define LX_O_NONBLOCK  0x800
#define LX_O_DIRECTORY 0x10000
#define LX_O_CLOEXEC   0x80000

#define LX_AT_FDCWD           (-100)
#define LX_AT_SYMLINK_NOFOLLOW 0x100
#define LX_AT_REMOVEDIR       0x200
#define LX_AT_EMPTY_PATH      0x1000

#define LX_F_DUPFD          0
#define LX_F_GETFD          1
#define LX_F_SETFD          2
#define LX_F_GETFL          3
#define LX_F_SETFL          4
#define LX_F_GETLK          5
#define LX_F_SETLK          6
#define LX_F_SETLKW         7
#define LX_F_DUPFD_CLOEXEC  1030

#define LX_MAP_SHARED     0x01
#define LX_MAP_PRIVATE    0x02
#define LX_MAP_FIXED      0x10
#define LX_MAP_ANONYMOUS  0x20

#define LX_TCGETS     0x5401
#define LX_TCSETS     0x5402
#define LX_TCSETSW    0x5403
#define LX_TCSETSF    0x5404
#define LX_TIOCGPGRP  0x540F
#define LX_TIOCSPGRP  0x5410
#define LX_TIOCGWINSZ 0x5413
#define LX_TIOCSWINSZ 0x5414
#define LX_FIONREAD   0x541B
#define LX_FIONBIO    0x5421

#define LX_ARCH_SET_GS 0x1001
#define LX_ARCH_SET_FS 0x1002
#define LX_ARCH_GET_FS 0x1003
#define LX_ARCH_GET_GS 0x1004

#define LX_WNOHANG 1

#define LX_DT_UNKNOWN 0
#define LX_DT_FIFO    1
#define LX_DT_CHR     2
#define LX_DT_DIR     4
#define LX_DT_REG     8

#define LX_S_IFIFO 0010000

#ifndef ERANGE
#define ERANGE 34
#endif

/* clone flags (linux/sched.h) */
#define CLONE_VM             0x00000100ULL
#define CLONE_VFORK          0x00004000ULL
#define CLONE_SETTLS         0x00080000ULL
#define CLONE_PARENT_SETTID  0x00100000ULL
#define CLONE_CHILD_CLEARTID 0x00200000ULL
#define CLONE_CHILD_SETTID   0x01000000ULL

/* Linux x86-64 syscall numbers used below */
enum {
    LX_read = 0, LX_write = 1, LX_open = 2, LX_close = 3, LX_stat = 4,
    LX_fstat = 5, LX_lstat = 6, LX_poll = 7, LX_lseek = 8, LX_mmap = 9,
    LX_mprotect = 10, LX_munmap = 11, LX_brk = 12, LX_rt_sigaction = 13,
    LX_rt_sigprocmask = 14, LX_rt_sigreturn = 15, LX_ioctl = 16,
    LX_pread64 = 17, LX_pwrite64 = 18, LX_readv = 19, LX_writev = 20,
    LX_access = 21, LX_pipe = 22, LX_select = 23, LX_sched_yield = 24,
    LX_mremap = 25, LX_msync = 26, LX_mincore = 27, LX_madvise = 28,
    LX_dup = 32, LX_dup2 = 33, LX_pause = 34, LX_nanosleep = 35,
    LX_getitimer = 36, LX_alarm = 37, LX_setitimer = 38, LX_getpid = 39,
    LX_sendfile = 40, LX_socket = 41, LX_clone = 56, LX_fork = 57, LX_vfork = 58,
    LX_execve = 59, LX_exit = 60, LX_wait4 = 61, LX_kill = 62, LX_uname = 63,
    LX_fcntl = 72, LX_flock = 73, LX_fsync = 74, LX_fdatasync = 75,
    LX_truncate = 76, LX_ftruncate = 77, LX_getdents = 78, LX_getcwd = 79,
    LX_chdir = 80, LX_fchdir = 81, LX_rename = 82, LX_mkdir = 83,
    LX_rmdir = 84, LX_creat = 85, LX_link = 86, LX_unlink = 87,
    LX_symlink = 88, LX_readlink = 89, LX_chmod = 90, LX_fchmod = 91,
    LX_chown = 92, LX_fchown = 93, LX_lchown = 94, LX_umask = 95,
    LX_gettimeofday = 96, LX_getrlimit = 97, LX_getrusage = 98,
    LX_sysinfo = 99, LX_times = 100, LX_getuid = 102, LX_getgid = 104,
    LX_setuid = 105, LX_setgid = 106, LX_geteuid = 107, LX_getegid = 108,
    LX_setpgid = 109, LX_getppid = 110, LX_getpgrp = 111, LX_setsid = 112,
    LX_setreuid = 113, LX_setregid = 114, LX_getgroups = 115,
    LX_setgroups = 116, LX_setresuid = 117, LX_getresuid = 118,
    LX_setresgid = 119, LX_getresgid = 120, LX_getpgid = 121,
    LX_getsid = 124, LX_rt_sigpending = 127, LX_rt_sigsuspend = 130,
    LX_sigaltstack = 131, LX_statfs = 137, LX_fstatfs = 138,
    LX_prctl = 157, LX_arch_prctl = 158, LX_setrlimit = 160, LX_sync = 162,
    LX_gettid = 186, LX_tkill = 200, LX_time = 201, LX_futex = 202,
    LX_sched_getaffinity = 204, LX_getdents64 = 217,
    LX_set_tid_address = 218, LX_clock_gettime = 228, LX_clock_getres = 229,
    LX_clock_nanosleep = 230, LX_exit_group = 231, LX_tgkill = 234,
    LX_openat = 257, LX_mkdirat = 258, LX_fchownat = 260,
    LX_newfstatat = 262, LX_unlinkat = 263, LX_renameat = 264,
    LX_readlinkat = 267, LX_fchmodat = 268, LX_faccessat = 269,
    LX_pselect6 = 270, LX_ppoll = 271, LX_set_robust_list = 273,
    LX_get_robust_list = 274, LX_utimensat = 280, LX_dup3 = 292,
    LX_pipe2 = 293, LX_prlimit64 = 302, LX_getcpu = 309,
    LX_renameat2 = 316, LX_copy_file_range = 326, LX_getrandom = 318, LX_membarrier = 324,
    LX_statx = 332, LX_rseq = 334, LX_faccessat2 = 439,
};

/* ------------------------------------------------------------------------ */
/* Linux structures                                                         */
/* ------------------------------------------------------------------------ */
typedef struct {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t __pad0;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;
    uint64_t st_atime_sec,  st_atime_nsec;
    uint64_t st_mtime_sec,  st_mtime_nsec;
    uint64_t st_ctime_sec,  st_ctime_nsec;
    int64_t  __unused[3];
} lx_stat_t;                                    /* 144 bytes */

typedef struct { int64_t tv_sec; int64_t tv_nsec; } lx_timespec_t;
typedef struct { int64_t tv_sec; int64_t tv_usec; } lx_timeval_t;
typedef struct { uint64_t iov_base; uint64_t iov_len; } lx_iovec_t;

typedef struct {
    uint32_t c_iflag, c_oflag, c_cflag, c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[19];
} lx_termios_t;                                 /* kernel struct termios, 36 bytes */

typedef struct { uint16_t ws_row, ws_col, ws_xpixel, ws_ypixel; } lx_winsize_t;

typedef struct {
    char sysname[65], nodename[65], release[65], version[65], machine[65],
         domainname[65];
} lx_utsname_t;

typedef struct { uint64_t rlim_cur, rlim_max; } lx_rlimit_t;

/* ------------------------------------------------------------------------ */
/* Helpers                                                                  */
/* ------------------------------------------------------------------------ */
static inline process_t *lx_proc(void) { return get_current_process(); }

static bool lx_fd_ok(process_t *p, long fd) {
    return p && fd >= 0 && fd < MAX_OPEN_FILES_SYSCALL && p->fds[fd].in_use;
}

/* fds 0..2 that were never redirected are the console. */
static bool lx_fd_is_tty(process_t *p, long fd) {
    if (!lx_fd_ok(p, fd)) return false;
    if (p->fds[fd].fd_type == FD_TYPE_DEV_TTY) return true;
    /* Console stdio: process_alloc marks 0/1/2 as FD_TYPE_FILE with NULL handle. */
    return fd <= 2 && p->fds[fd].fd_type == FD_TYPE_FILE &&
           p->fds[fd].file_handle == NULL;
}

/* Default cooked termios (matches prior hard-coded TCGETS values). */
static void lx_ti_default(process_t *p) {
    if (!p || p->linux_ti_valid) return;
    p->linux_ti_iflag = 0x100 | 0x400;               /* ICRNL | IXON */
    p->linux_ti_oflag = 0x1 | 0x4;                   /* OPOST | ONLCR */
    p->linux_ti_cflag = 0xF | 0x30 | 0x80;           /* B38400|CS8|CREAD */
    p->linux_ti_lflag = 0x1 | 0x2 | 0x8 | 0x10 | 0x20 | 0x8000; /* ISIG ICANON ECHO... */
    static const uint8_t cc[19] = { 3, 28, 127, 21, 4, 0, 1, 0, 17, 19, 26,
                                    0, 18, 15, 23, 22, 0, 0, 0 };
    for (int i = 0; i < 19; i++) p->linux_ti_cc[i] = cc[i];
    p->linux_ti_valid = true;
}

#define LX_ICANON 0x0002
#define LX_ECHO   0x0008

/* Read from a tty respecting ICANON: cooked = line until \n; raw = one byte. */
static long lx_read_tty(process_t *p, long fd, void *ubuf, size_t count) {
    (void)fd;
    if (count == 0) return 0;
    lx_ti_default(p);
    if (p->linux_ti_lflag & LX_ICANON)
        return (long)sys_read(STDIN_FILENO, ubuf, count);

    /* Non-canonical: return as soon as one character is available (VMIN=1). */
    size_t want = count;
    if (want > 64) want = 64;   /* bound latency for large reads */
    char kbuf[64];
    size_t got = 0;
    while (got < want) {
        extern int sys_getchar_intr(process_t *);
        int ch = sys_getchar_intr(p);
        if (ch < 0) {
            if (got == 0) return -EINTR;
            break;
        }
        kbuf[got++] = (char)ch;
        /* If more chars are already buffered, keep filling without blocking. */
        extern uint32_t keyboard_get_buffer_length(void);
        if (keyboard_get_buffer_length() == 0)
            break;
    }
    if (copy_to_user(ubuf, kbuf, got) != 0)
        return -EFAULT;
    return (long)got;
}

static long lx_read(process_t *p, long fd, void *ubuf, size_t count) {
    if (!lx_fd_ok(p, fd)) return -EBADF;
    uint8_t ty = p->fds[fd].fd_type;
    if (ty == FD_TYPE_DEV_NULL)
        return 0;
    if (ty == FD_TYPE_DEV_TTY || lx_fd_is_tty(p, fd))
        return lx_read_tty(p, fd, ubuf, count);
    return (long)sys_read((int)fd, ubuf, count);
}

static long lx_put(void *uptr, const void *k, size_t n) {
    if (!uptr) return -EFAULT;
    return copy_to_user(uptr, k, n) ? -EFAULT : 0;
}

static long lx_get(void *k, const void *uptr, size_t n) {
    if (!uptr) return -EFAULT;
    return copy_from_user(k, uptr, n) ? -EFAULT : 0;
}

/* Time: monotonic = microseconds since boot, realtime = RTC epoch at boot
 * plus the same counter (the CMOS clock is read once). */
static uint64_t lx_epoch_base_us;
static bool     lx_epoch_valid;

static uint64_t lx_mono_us(void) { return ktime_now_us(); }
static uint64_t lx_real_us(void) {
    if (!lx_epoch_valid) {
        uint64_t now = ktime_now_us();
        uint64_t epoch = rtc_unix_epoch();
        lx_epoch_base_us = epoch * 1000000ULL - now;
        lx_epoch_valid = true;
    }
    return lx_epoch_base_us + ktime_now_us();
}

static uint64_t lx_rand_state;
static uint64_t lx_random_u64(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    lx_rand_state += (((uint64_t)hi << 32) | lo) + 0x9E3779B97F4A7C15ULL;
    uint64_t z = lx_rand_state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

/*
 * Resolve a user path (optionally relative to dirfd) into a normalised
 * absolute path: no "." / ".." / empty components, no trailing slash.
 */
static long lx_resolve(process_t *p, long dirfd, const char *upath,
                       char *out, size_t outsz, bool allow_empty)
{
    char kpath[256];
    if (!upath || !strncpy_from_user(kpath, upath, sizeof(kpath)))
        return -EFAULT;
    if (kpath[0] == '\0' && !allow_empty)
        return -ENOENT;

    const char *base = "";
    if (kpath[0] != '/') {
        if ((int)dirfd == LX_AT_FDCWD) {
            base = p->cwd[0] ? p->cwd : "/";
        } else {
            if (!lx_fd_ok(p, dirfd))
                return -EBADF;
            if (p->fds[dirfd].fd_type != FD_TYPE_VFS_DIR)
                return -ENOTDIR;
            base = (const char *)p->fds[dirfd].file_handle;
            if (!base) return -EBADF;
        }
    }

    char tmp[512];
    size_t bl = strlen(base), kl = strlen(kpath);
    if (bl + 1 + kl + 1 > sizeof(tmp))
        return -ENAMETOOLONG;
    memcpy(tmp, base, bl);
    tmp[bl] = '/';
    memcpy(tmp + bl + 1, kpath, kl + 1);

    /* Normalise into out */
    size_t o = 0;
    const char *s = tmp;
    while (*s) {
        while (*s == '/') s++;
        if (!*s) break;
        const char *e = s;
        while (*e && *e != '/') e++;
        size_t cl = (size_t)(e - s);
        if (cl == 1 && s[0] == '.') {
            /* skip */
        } else if (cl == 2 && s[0] == '.' && s[1] == '.') {
            while (o > 0 && out[o - 1] != '/') o--;
            if (o > 0) o--;                 /* drop the slash too */
        } else {
            if (o + 1 + cl + 1 > outsz)
                return -ENAMETOOLONG;
            out[o++] = '/';
            memcpy(out + o, s, cl);
            o += cl;
        }
        s = e;
    }
    if (o == 0)
        out[o++] = '/';
    out[o] = '\0';
    return 0;
}

static int lx_path_kind(const char *abspath, stat_t *st_out) {
    stat_t st;
    if (syscall_stat_abspath(abspath, &st) != 0)
        return 0;                               /* missing */
    if (st_out) *st_out = st;
    return ((st.st_mode & S_IFMT) == S_IFDIR) ? 2 : 1;
}

static void lx_fill_stat(lx_stat_t *ls, const stat_t *st) {
    memset(ls, 0, sizeof(*ls));
    uint64_t now = lx_real_us() / 1000000ULL;
    ls->st_dev = st->st_dev;
    ls->st_ino = st->st_ino;
    ls->st_nlink = st->st_nlink ? st->st_nlink : 1;
    ls->st_mode = st->st_mode;
    ls->st_uid = st->st_uid;
    ls->st_gid = st->st_gid;
    ls->st_rdev = st->st_rdev;
    ls->st_size = (int64_t)st->st_size;
    ls->st_blksize = st->st_blksize ? (int64_t)st->st_blksize : 4096;
    ls->st_blocks = (int64_t)st->st_blocks;
    /* The VFS has no timestamps yet; report "now" rather than boot-relative
     * seconds so tools that compare times see something plausible. */
    ls->st_atime_sec = ls->st_mtime_sec = ls->st_ctime_sec = now;
}

static long lx_stat_path(const char *abspath, void *ubuf) {
    stat_t st;
    if (!lx_path_kind(abspath, &st))
        return -ENOENT;
    lx_stat_t ls;
    lx_fill_stat(&ls, &st);
    return lx_put(ubuf, &ls, sizeof(ls));
}

static long lx_fstat(process_t *p, long fd, void *ubuf) {
    if (!lx_fd_ok(p, fd))
        return -EBADF;
    file_descriptor_t *f = &p->fds[fd];
    stat_t st;
    memset(&st, 0, sizeof(st));
    if (f->fd_type == FD_TYPE_DEV_NULL || f->fd_type == FD_TYPE_DEV_TTY) {
        st.st_mode = (f->fd_type == FD_TYPE_DEV_NULL ? 0666 : 0622) | S_IFCHR;
        st.st_nlink = 1;
        st.st_blksize = 4096;
        st.st_rdev = f->fd_type == FD_TYPE_DEV_NULL ? 0x103 : 0x500; /* 1,3 / 5,0 */
        lx_stat_t ls;
        lx_fill_stat(&ls, &st);
        return lx_put(ubuf, &ls, sizeof(ls));
    }
    if (f->fd_type == FD_TYPE_VFS && f->vfs_fd >= 0) {
        char path[VFS_MAX_PATH];
        if (vfs_fd_get_path(f->vfs_fd, path, sizeof(path)) != 0)
            return -EIO;
        if (syscall_stat_abspath(path, &st) != 0)
            return -EIO;
    } else if (f->fd_type == FD_TYPE_VFS_DIR && f->file_handle) {
        if (syscall_stat_abspath((const char *)f->file_handle, &st) != 0) {
            st.st_mode = S_IFDIR | 0755;
        }
    } else if (f->fd_type == FD_TYPE_PIPE_READ || f->fd_type == FD_TYPE_PIPE_WRITE) {
        st.st_mode = LX_S_IFIFO | 0600;
        st.st_blksize = 4096;
    } else {
        st.st_mode = S_IFCHR | 0620;            /* console */
        st.st_rdev = (4u << 8) | 1;             /* tty1 */
        st.st_blksize = 1024;
        st.st_ino = 1000 + (uint64_t)fd;
    }
    st.st_nlink = 1;
    lx_stat_t ls;
    lx_fill_stat(&ls, &st);
    return lx_put(ubuf, &ls, sizeof(ls));
}

/* Mirror Linux stdout/stderr to serial (first few KB per process). */
static uint32_t lx_mirror_pid, lx_mirror_bytes;
static void lx_mirror(process_t *p, long fd, const void *ubuf, size_t len) {
    if (!linux_stdout_mirror || !p || (fd != 1 && fd != 2) || !lx_fd_is_tty(p, fd))
        return;
    if (lx_mirror_pid != p->pid) { lx_mirror_pid = p->pid; lx_mirror_bytes = 0; }
    if (lx_mirror_bytes >= LINUX_STDOUT_MIRROR_MAX)
        return;
    size_t n = len;
    if (n > LINUX_STDOUT_MIRROR_MAX - lx_mirror_bytes)
        n = LINUX_STDOUT_MIRROR_MAX - lx_mirror_bytes;
    char buf[257];
    size_t done = 0;
    while (done < n) {
        size_t c = n - done;
        if (c > 256) c = 256;
        if (copy_from_user(buf, (const uint8_t *)ubuf + done, c) != 0)
            return;
        buf[c] = '\0';
        serial_printf("%s", buf);
        done += c;
    }
    lx_mirror_bytes += (uint32_t)n;
    if (lx_mirror_bytes >= LINUX_STDOUT_MIRROR_MAX)
        serial_printf("\n[linux: PID %u stdout mirror limit reached]\n", p->pid);
}

static long lx_write(process_t *p, long fd, const void *ubuf, size_t len) {
    if (!lx_fd_ok(p, fd)) return -EBADF;
    if (p->fds[fd].fd_type == FD_TYPE_DEV_NULL)
        return (long)len;
    if (p->fds[fd].fd_type == FD_TYPE_DEV_TTY) {
        /* Write through stdout. */
        long r = sys_write(STDOUT_FILENO, ubuf, len);
        if (r > 0) lx_mirror(p, 1, ubuf, (size_t)r);
        return r;
    }
    long r = sys_write((int)fd, ubuf, len);
    if (r > 0)
        lx_mirror(p, fd, ubuf, (size_t)r);
    return r;
}

static long lx_rw_vec(process_t *p, long fd, const void *uiov, long iovcnt, bool wr) {
    if (iovcnt < 0 || iovcnt > 1024)
        return -EINVAL;
    long total = 0;
    for (long i = 0; i < iovcnt; i++) {
        lx_iovec_t v;
        if (lx_get(&v, (const lx_iovec_t *)uiov + i, sizeof(v)))
            return total ? total : -EFAULT;
        if (v.iov_len == 0)
            continue;
        long r = wr ? lx_write(p, fd, (const void *)v.iov_base, (size_t)v.iov_len)
                    : lx_read(p, fd, (void *)v.iov_base, (size_t)v.iov_len);
        if (r < 0)
            return total ? total : r;
        total += r;
        if ((uint64_t)r < v.iov_len)
            break;                              /* short transfer */
    }
    return total;
}

/* Positional I/O: seek, transfer, seek back. */
static long lx_pio(process_t *p, long fd, void *ubuf, size_t len, int64_t off, bool wr) {
    if (!lx_fd_ok(p, fd)) return -EBADF;
    if (p->fds[fd].fd_type != FD_TYPE_VFS) return -ESPIPE;
    if (off < 0) return -EINVAL;
    long cur = sys_seek((int)fd, 0, SEEK_CUR);
    if (cur < 0) return cur;
    long r = sys_seek((int)fd, (off_t)off, SEEK_SET);
    if (r < 0) return r;
    r = wr ? (long)sys_write((int)fd, ubuf, len) : (long)sys_read((int)fd, ubuf, len);
    sys_seek((int)fd, (off_t)cur, SEEK_SET);
    return r;
}

static long lx_open_special(process_t *p, const char *path, long lflags) {
    /* Synthetic devices used by musl/busybox (isatty, ash, redirects). */
    if (kstrcmp(path, "/dev/null") == 0) {
        int fd = allocate_fd(p);
        if (fd < 0) return -EMFILE;
        p->fds[fd].fd_type = FD_TYPE_DEV_NULL;
        p->fds[fd].file_handle = NULL;
        p->fds[fd].flags = (uint32_t)(lflags & 3);
        p->fds[fd].position = 0;
        p->fds[fd].cloexec = (lflags & LX_O_CLOEXEC) != 0;
        p->fds[fd].in_use = true;
        return fd;
    }
    if (kstrcmp(path, "/dev/tty") == 0 || kstrcmp(path, "/dev/console") == 0) {
        int fd = allocate_fd(p);
        if (fd < 0) return -EMFILE;
        p->fds[fd].fd_type = FD_TYPE_DEV_TTY;
        p->fds[fd].file_handle = NULL;
        p->fds[fd].flags = (uint32_t)(lflags & 3);
        p->fds[fd].position = 0;
        p->fds[fd].cloexec = (lflags & LX_O_CLOEXEC) != 0;
        p->fds[fd].in_use = true;
        return fd;
    }
    return -ENOENT; /* not special */
}

static long lx_openat(process_t *p, long dirfd, const char *upath, long lflags, long mode) {
    char path[256];
    long r = lx_resolve(p, dirfd, upath, path, sizeof(path), false);
    if (r) return r;

    r = lx_open_special(p, path, lflags);
    if (r != -ENOENT) return r;

    int kind = lx_path_kind(path, NULL);
    if ((lflags & LX_O_CREAT) && (lflags & LX_O_EXCL) && kind)
        return -EEXIST;
    if ((lflags & LX_O_DIRECTORY) && kind != 2)
        return kind ? -ENOTDIR : -ENOENT;
    if (!kind && !(lflags & LX_O_CREAT))
        return -ENOENT;
    if (kind == 2 && (lflags & LX_O_ACCMODE))
        return -EISDIR;

    int f = 0;
    switch (lflags & LX_O_ACCMODE) {
        case 1: f |= O_WRONLY; break;
        case 2: f |= O_RDWR;   break;
        default: break;
    }
    if (kind != 2) {
        if (lflags & LX_O_CREAT)  f |= O_CREAT;
        if (lflags & LX_O_TRUNC)  f |= O_TRUNC;
        if (lflags & LX_O_APPEND) f |= O_APPEND;
    }
    if (lflags & LX_O_NONBLOCK) f |= O_NONBLOCK;

    long fd = sys_open_kpath(path, f, (mode_t)mode);
    if (fd >= 0 && (lflags & LX_O_CLOEXEC))
        p->fds[fd].cloexec = true;
    return fd;
}

static long lx_getdents64(process_t *p, long fd, void *udirp, size_t count) {
    if (!lx_fd_ok(p, fd)) return -EBADF;
    file_descriptor_t *f = &p->fds[fd];
    if (f->fd_type != FD_TYPE_VFS_DIR || !f->file_handle) return -ENOTDIR;
    if (count > 65536) count = 65536;

    enum { MAXENT = 256 };
    vfs_dirent_t *ents = (vfs_dirent_t *)kalloc(sizeof(vfs_dirent_t) * MAXENT);
    uint8_t *kbuf = (uint8_t *)kalloc(count ? count : 1);
    if (!ents || !kbuf) {
        if (ents) kfree(ents);
        if (kbuf) kfree(kbuf);
        return -ENOMEM;
    }
    int total = vfs_readdir((const char *)f->file_handle, ents, MAXENT);
    if (total < 0) { kfree(ents); kfree(kbuf); return -EIO; }

    uint32_t pos = f->position;
    size_t off = 0;
    while (pos < (uint32_t)total) {
        vfs_dirent_t *e = &ents[pos];
        size_t nl = strlen(e->name);
        size_t reclen = (19 + nl + 1 + 7) & ~(size_t)7;
        if (off + reclen > count) break;
        uint8_t *rec = kbuf + off;
        memset(rec, 0, reclen);
        uint64_t ino = e->inode ? e->inode : (uint64_t)pos + 1;
        int64_t  doff = (int64_t)pos + 1;
        uint16_t rl = (uint16_t)reclen;
        memcpy(rec + 0, &ino, 8);
        memcpy(rec + 8, &doff, 8);
        memcpy(rec + 16, &rl, 2);
        rec[18] = (e->type & VFS_DIRECTORY) ? LX_DT_DIR :
                  (e->type & VFS_FILE) ? LX_DT_REG : LX_DT_UNKNOWN;
        memcpy(rec + 19, e->name, nl + 1);
        off += reclen;
        pos++;
    }
    long r;
    if (off == 0 && pos < (uint32_t)total) {
        r = -EINVAL;                            /* buffer too small */
    } else if (off && copy_to_user(udirp, kbuf, off) != 0) {
        r = -EFAULT;
    } else {
        f->position = pos;
        r = (long)off;
    }
    kfree(ents);
    kfree(kbuf);
    return r;
}

static long lx_lseek(process_t *p, long fd, int64_t off, long whence) {
    if (!lx_fd_ok(p, fd)) return -EBADF;
    file_descriptor_t *f = &p->fds[fd];
    if (f->fd_type == FD_TYPE_VFS_DIR) {         /* rewinddir / seekdir */
        if (whence == SEEK_SET && off >= 0) { f->position = (uint32_t)off; return off; }
        if (whence == SEEK_CUR && off == 0) return f->position;
        return -EINVAL;
    }
    if (f->fd_type != FD_TYPE_VFS) return -ESPIPE;
    if (whence < 0 || whence > 2) return -EINVAL;
    return sys_seek((int)fd, (off_t)off, (int)whence);
}

static long lx_ioctl(process_t *p, long fd, unsigned long req, void *arg) {
    if (!lx_fd_ok(p, fd)) return -EBADF;
    bool tty = lx_fd_is_tty(p, fd);
    switch (req) {
        case LX_TCGETS: {
            if (!tty) return -ENOTTY;
            lx_ti_default(p);
            lx_termios_t ti;
            memset(&ti, 0, sizeof(ti));
            ti.c_iflag = p->linux_ti_iflag;
            ti.c_oflag = p->linux_ti_oflag;
            ti.c_cflag = p->linux_ti_cflag;
            ti.c_lflag = p->linux_ti_lflag;
            memcpy(ti.c_cc, p->linux_ti_cc, sizeof(ti.c_cc));
            return lx_put(arg, &ti, sizeof(ti));
        }
        case LX_TCSETS: case LX_TCSETSW: case LX_TCSETSF: {
            if (!tty) return -ENOTTY;
            lx_termios_t ti;
            if (lx_get(&ti, arg, sizeof(ti))) return -EFAULT;
            p->linux_ti_iflag = ti.c_iflag;
            p->linux_ti_oflag = ti.c_oflag;
            p->linux_ti_cflag = ti.c_cflag;
            p->linux_ti_lflag = ti.c_lflag;
            memcpy(p->linux_ti_cc, ti.c_cc, sizeof(p->linux_ti_cc));
            p->linux_ti_valid = true;
            return 0;
        }
        case LX_TIOCGWINSZ: {
            if (!tty) return -ENOTTY;
            uint32_t cols = 80, rows = 25;
            fb_console_get_size(&cols, &rows);
            lx_winsize_t ws = { (uint16_t)rows, (uint16_t)cols, 0, 0 };
            return lx_put(arg, &ws, sizeof(ws));
        }
        case LX_TIOCSWINSZ:
            return tty ? 0 : -ENOTTY;
        case LX_TIOCGPGRP: {
            if (!tty) return -ENOTTY;
            int pg = (int)process_get_fg_pgid();
            if (!pg) pg = (int)p->pgid;
            return lx_put(arg, &pg, sizeof(pg));
        }
        case LX_TIOCSPGRP: {
            if (!tty) return -ENOTTY;
            int pg = 0;
            if (lx_get(&pg, arg, sizeof(pg))) return -EFAULT;
            if (pg < 0) return -EINVAL;
            process_set_fg_pgid((uint32_t)pg);
            return 0;
        }
        case 0x540E: /* TIOCSCTTY */
        case 0x5422: /* TIOCNOTTY */
            return tty ? 0 : -ENOTTY;
        case 0x5429: { /* TIOCGSID */
            if (!tty) return -ENOTTY;
            int sid = (int)p->sid;
            return lx_put(arg, &sid, sizeof(sid));
        }
        case LX_FIONREAD:
        case LX_FIONBIO:
            return sys_ioctl((int)fd, req, arg);
        default:
            if (tty) return -ENOTTY;
            return sys_ioctl((int)fd, req, arg);
    }
}

static long lx_fcntl(process_t *p, long fd, long cmd, long arg) {
    if (!lx_fd_ok(p, fd)) return -EBADF;
    switch (cmd) {
        case LX_F_DUPFD:
            return sys_fcntl((int)fd, F_DUPFD, (int)arg);
        case LX_F_DUPFD_CLOEXEC: {
            long nfd = sys_fcntl((int)fd, F_DUPFD, (int)arg);
            if (nfd >= 0) p->fds[nfd].cloexec = true;
            return nfd;
        }
        case LX_F_GETFD:
            return p->fds[fd].cloexec ? 1 : 0;
        case LX_F_SETFD:
            p->fds[fd].cloexec = (arg & 1) != 0;
            return 0;
        case LX_F_GETFL: {
            uint32_t k = p->fds[fd].flags;
            long l = (k & O_RDWR) ? 2 : (k & O_WRONLY) ? 1 : 0;
            if (fd <= 2 && lx_fd_is_tty(p, fd)) l = 2;
            if (k & O_APPEND)   l |= LX_O_APPEND;
            if (k & O_NONBLOCK) l |= LX_O_NONBLOCK;
            return l;
        }
        case LX_F_SETFL: {
            uint32_t k = p->fds[fd].flags & ~(uint32_t)(O_APPEND | O_NONBLOCK);
            if (arg & LX_O_APPEND)   k |= O_APPEND;
            if (arg & LX_O_NONBLOCK) k |= O_NONBLOCK;
            p->fds[fd].flags = k;
            return 0;
        }
        case LX_F_GETLK: case LX_F_SETLK: case LX_F_SETLKW:
            return 0;                           /* no locking: single user */
        default:
            return -EINVAL;
    }
}

static long lx_mmap(process_t *p, uint64_t addr, uint64_t len, long prot, long flags,
                    long fd, int64_t off) {
    if (len == 0) return -EINVAL;
    if (flags & LX_MAP_ANONYMOUS) {
        /* MAP_SHARED|MAP_ANONYMOUS is only "shared" across fork, which is
         * vfork-like here anyway: treat it as private. */
        int kf = MAP_ANONYMOUS | MAP_PRIVATE | ((flags & LX_MAP_FIXED) ? MAP_FIXED : 0);
        return (long)sys_mmap((void *)addr, (size_t)len, (int)prot, kf);
    }
    /* File mapping: private copy of the file contents (no write-back). */
    if (!lx_fd_ok(p, fd) || p->fds[fd].fd_type != FD_TYPE_VFS)
        return -EBADF;
    if (off < 0 || (off & (PAGE_SIZE - 1)))
        return -EINVAL;
    int kf = MAP_ANONYMOUS | MAP_PRIVATE | ((flags & LX_MAP_FIXED) ? MAP_FIXED : 0);
    long va = (long)sys_mmap((void *)addr, (size_t)len, PROT_READ | PROT_WRITE | (int)(prot & PROT_EXEC), kf);
    if (va < 0 && va > -4096) return va;
    long cur = sys_seek((int)fd, 0, SEEK_CUR);
    if (sys_seek((int)fd, (off_t)off, SEEK_SET) >= 0) {
        uint64_t done = 0;
        while (done < len) {
            uint64_t chunk = len - done;
            if (chunk > (1u << 20)) chunk = 1u << 20;
            long r = sys_read((int)fd, (void *)(va + done), (size_t)chunk);
            if (r <= 0) break;
            done += (uint64_t)r;
        }
    }
    if (cur >= 0) sys_seek((int)fd, (off_t)cur, SEEK_SET);
    return va;
}

static long lx_wait4(process_t *p, long pid, int *uwstatus, long options, void *urusage) {
    if (urusage) {
        uint8_t z[144];
        memset(z, 0, sizeof(z));
        lx_put(urusage, z, sizeof(z));
    }
    if (!p->child) return -ECHILD;
    if (options & LX_WNOHANG) {
        bool any = false;
        for (process_t *c = p->child; c; c = c->sibling)
            if (c->is_zombie && (pid <= 0 || (long)c->pid == pid)) { any = true; break; }
        if (!any)
            return 0; /* wait-only: empty WNOHANG must NOT release held kids */
    }
    /* Blocking wait (or WNOHANG with a zombie): wake any held newborns. */
    fork_release_pending(p);
    long r = sys_wait(uwstatus);
    if (r > 0 && uwstatus) {
        int raw = 0;
        if (lx_get(&raw, uwstatus, sizeof(raw)) == 0) {
            /* inDevOS stores 128+sig for signal deaths; encode like Linux. */
            int enc = (raw > 128 && raw < 128 + 32) ? (raw - 128) : ((raw & 0xff) << 8);
            lx_put(uwstatus, &enc, sizeof(enc));
        }
    }
    return r;
}

static long lx_rlimit(long res, lx_rlimit_t *out) {
    const uint64_t INF = ~0ULL;
    out->rlim_cur = INF;
    out->rlim_max = INF;
    switch (res) {
        case 3:  out->rlim_cur = 8ULL << 20; break;                 /* RLIMIT_STACK */
        case 7:  out->rlim_cur = out->rlim_max = MAX_OPEN_FILES_SYSCALL; break; /* NOFILE */
        case 4:  out->rlim_cur = 0; break;                           /* RLIMIT_CORE */
        default: break;
    }
    return 0;
}

/* Exit the calling process as if killed by `sig`. */
__attribute__((noreturn))
static void lx_die_by_signal(process_t *p, int sig) {
    serial_printf("linux: PID %u killed by signal %d\n", p ? p->pid : 0, sig);
    printf("Killed by signal %d\n", sig);
    sys_exit(128 + sig);
    __builtin_unreachable();
}

/* ------------------------------------------------------------------------ */
/* -ENOSYS logging                                                          */
/* ------------------------------------------------------------------------ */
static uint8_t lx_enosys_seen[512 / 8];

static long lx_enosys(process_t *p, uint64_t nr) {
    if (nr < 512) {
        if (lx_enosys_seen[nr / 8] & (1u << (nr % 8)))
            return -ENOSYS;
        lx_enosys_seen[nr / 8] |= (uint8_t)(1u << (nr % 8));
    }
    serial_printf("linux: unimplemented syscall %llu from PID %u -> -ENOSYS\n",
                  (unsigned long long)nr, p ? p->pid : 0);
    return -ENOSYS;
}

/* ------------------------------------------------------------------------ */
/* Dispatcher                                                               */
/* ------------------------------------------------------------------------ */

/* Refresh THIS thread's sc_* fork stash from the live syscall frame.
 * The per-thread stash is the only fork snapshot: fork_spawn_child() copies
 * it by value before doing any long work, so a sibling thread or a nested
 * fork can never clobber it. */
static void fork_snapshot_frame(process_t *p, linux_syscall_frame_t *fr) {
    thread_t *thr = scheduler_get_current_thread();
    if (thr) {
        thr->sc_user_rip = fr->user_rip;
        thr->sc_user_rsp = fr->user_rsp;
        thr->sc_user_rflags = fr->user_rflags;
        thr->sc_r15 = fr->r15; thr->sc_r14 = fr->r14;
        thr->sc_r13 = fr->r13; thr->sc_r12 = fr->r12;
        thr->sc_rbp = fr->rbp; thr->sc_rbx = fr->rbx;
        thr->sc_r9 = fr->a6; thr->sc_r8 = fr->a5;
        thr->sc_r10 = fr->a4; thr->sc_rdx = fr->a3;
        thr->sc_rsi = fr->a2; thr->sc_rdi = fr->a1;
        thr->sc_frame_valid = 1;
    }
    if (!fr->user_rip)
        serial_printf("fork: WARNING snapshot user_rip=0 pid=%d nr=%llu\n",
                      p ? (int)p->pid : -1, (unsigned long long)fr->nr);
}

static long lx_dispatch(linux_syscall_frame_t *fr) {
    process_t *p = lx_proc();
    const uint64_t nr = fr->nr;
    const uint64_t a1 = fr->a1, a2 = fr->a2, a3 = fr->a3,
                   a4 = fr->a4, a5 = fr->a5, a6 = fr->a6;
    char path[256], path2[256];
    long r;

    switch (nr) {
    /* ---- file I/O ---- */
    case LX_read:    return lx_read(p, (long)a1, (void *)a2, (size_t)a3);
    case LX_write:   return lx_write(p, (long)a1, (const void *)a2, (size_t)a3);
    case LX_readv:   return lx_rw_vec(p, (long)a1, (const void *)a2, (long)a3, false);
    case LX_writev:  return lx_rw_vec(p, (long)a1, (const void *)a2, (long)a3, true);
    case LX_pread64: return lx_pio(p, (long)a1, (void *)a2, (size_t)a3, (int64_t)a4, false);
    case LX_pwrite64:return lx_pio(p, (long)a1, (void *)a2, (size_t)a3, (int64_t)a4, true);
    case LX_open:    return lx_openat(p, LX_AT_FDCWD, (const char *)a1, (long)a2, (long)a3);
    case LX_creat:   return lx_openat(p, LX_AT_FDCWD, (const char *)a1,
                                      1 | LX_O_CREAT | LX_O_TRUNC, (long)a2);
    case LX_openat:  return lx_openat(p, (long)(int)a1, (const char *)a2, (long)a3, (long)a4);
    case LX_close:
        if (lx_fd_ok(p, (long)a1) && (long)a1 <= 2 && lx_fd_is_tty(p, (long)a1))
            return 0;                           /* keep the console fds */
        if (lx_fd_ok(p, (long)a1) &&
            (p->fds[a1].fd_type == FD_TYPE_DEV_NULL ||
             p->fds[a1].fd_type == FD_TYPE_DEV_TTY)) {
            p->fds[a1].in_use = false;
            p->fds[a1].fd_type = 0;
            return 0;
        }
        return sys_close((int)a1);
    case LX_lseek:   return lx_lseek(p, (long)a1, (int64_t)a2, (long)a3);
    case LX_getdents64: return lx_getdents64(p, (long)a1, (void *)a2, (size_t)a3);
    case LX_ioctl:   return lx_ioctl(p, (long)a1, (unsigned long)a2, (void *)a3);
    case LX_fcntl:   return lx_fcntl(p, (long)a1, (long)a2, (long)a3);
    case LX_dup:     return sys_dup((int)a1);
    case LX_dup2:    return sys_dup2((int)a1, (int)a2);
    case LX_dup3:
        if (a1 == a2) return -EINVAL;
        r = sys_dup2((int)a1, (int)a2);
        if (r >= 0 && (a3 & LX_O_CLOEXEC)) p->fds[r].cloexec = true;
        return r;
    case LX_pipe:    return sys_pipe((int *)a1);
    case LX_pipe2: {
        r = sys_pipe((int *)a1);
        if (r == 0 && (a2 & LX_O_CLOEXEC)) {
            int fds[2];
            if (lx_get(fds, (void *)a1, sizeof(fds)) == 0) {
                if (lx_fd_ok(p, fds[0])) p->fds[fds[0]].cloexec = true;
                if (lx_fd_ok(p, fds[1])) p->fds[fds[1]].cloexec = true;
            }
        }
        return r;
    }
    case LX_poll:    return sys_poll((pollfd_t *)a1, (unsigned)a2, (int)a3);
    case LX_select: case LX_pselect6: {
        /* Minimal select: only support reading readiness on stdin/pipes via poll. */
        /* args: nfds, readfds, writefds, exceptfds, timeout[, sig] */
        int nfds = (int)a1;
        if (nfds < 0) return -EINVAL;
        if (nfds > 64) nfds = 64;
        uint64_t rin = 0, win = 0, ein = 0;
        if (a2 && lx_get(&rin, (void *)a2, 8)) return -EFAULT;
        if (a3 && lx_get(&win, (void *)a3, 8)) return -EFAULT;
        if (a4 && lx_get(&ein, (void *)a4, 8)) return -EFAULT;
        (void)ein;
        int timeout_ms = -1;
        if (a5) {
            if (nr == LX_pselect6) {
                /* struct timespec */
                int64_t ts[2];
                if (lx_get(ts, (void *)a5, 16)) return -EFAULT;
                timeout_ms = (int)(ts[0] * 1000 + ts[1] / 1000000);
            } else {
                /* struct timeval */
                int64_t tv[2];
                if (lx_get(tv, (void *)a5, 16)) return -EFAULT;
                timeout_ms = (int)(tv[0] * 1000 + tv[1] / 1000);
            }
            if (timeout_ms < 0) timeout_ms = 0;
        }
        pollfd_t pf[64];
        int np = 0;
        for (int fd = 0; fd < nfds && np < 64; fd++) {
            short ev = 0;
            if (rin & (1ULL << fd)) ev |= POLLIN;
            if (win & (1ULL << fd)) ev |= POLLOUT;
            if (!ev) continue;
            pf[np].fd = fd;
            pf[np].events = ev;
            pf[np].revents = 0;
            np++;
        }
        int pr = np ? sys_poll(pf, (unsigned)np, timeout_ms) : 0;
        if (pr < 0) return pr;
        uint64_t rout = 0, wout = 0, eout = 0;
        int ready = 0;
        for (int i = 0; i < np; i++) {
            if (pf[i].revents & (POLLIN | POLLHUP | POLLERR)) {
                rout |= (1ULL << pf[i].fd); ready++;
            }
            if (pf[i].revents & POLLOUT) {
                wout |= (1ULL << pf[i].fd); ready++;
            }
        }
        if (a2 && lx_put((void *)a2, &rout, 8)) return -EFAULT;
        if (a3 && lx_put((void *)a3, &wout, 8)) return -EFAULT;
        if (a4 && lx_put((void *)a4, &eout, 8)) return -EFAULT;
        return ready;
    }
    case LX_ppoll: {
        int ms = -1;
        if (a3) {
            lx_timespec_t ts;
            if (lx_get(&ts, (void *)a3, sizeof(ts))) return -EFAULT;
            ms = (int)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
        }
        return sys_poll((pollfd_t *)a1, (unsigned)a2, ms);
    }
    case LX_fsync: case LX_fdatasync: case LX_sync:
        return 0;

    /* ---- metadata / paths ---- */
    case LX_stat: case LX_lstat:
        if ((r = lx_resolve(p, LX_AT_FDCWD, (const char *)a1, path, sizeof(path), false))) return r;
        return lx_stat_path(path, (void *)a2);
    case LX_fstat:
        return lx_fstat(p, (long)a1, (void *)a2);
    case LX_newfstatat: {
        if (a4 & LX_AT_EMPTY_PATH) {
            char c = 0;
            if (a2) copy_from_user(&c, (const void *)a2, 1);
            if (c == 0) {
                if ((int)a1 == LX_AT_FDCWD) {
                    return lx_stat_path(p->cwd[0] ? p->cwd : "/", (void *)a3);
                }
                return lx_fstat(p, (long)(int)a1, (void *)a3);
            }
        }
        if ((r = lx_resolve(p, (long)(int)a1, (const char *)a2, path, sizeof(path), false))) return r;
        return lx_stat_path(path, (void *)a3);
    }
    case LX_access:
        if ((r = lx_resolve(p, LX_AT_FDCWD, (const char *)a1, path, sizeof(path), false))) return r;
        return lx_path_kind(path, NULL) ? 0 : -ENOENT;
    case LX_faccessat: case LX_faccessat2:
        if ((r = lx_resolve(p, (long)(int)a1, (const char *)a2, path, sizeof(path), false))) return r;
        return lx_path_kind(path, NULL) ? 0 : -ENOENT;
    case LX_readlink: case LX_readlinkat: {
        const char *up = (const char *)(nr == LX_readlink ? a1 : a2);
        long dfd = nr == LX_readlink ? LX_AT_FDCWD : (long)(int)a1;
        void *ubuf = (void *)(nr == LX_readlink ? a2 : a3);
        size_t bufsiz = (size_t)(nr == LX_readlink ? a3 : a4);
        if ((r = lx_resolve(p, dfd, up, path, sizeof(path), false))) return r;
        /* /proc/self/exe and /proc/<pid>/exe */
        bool is_exe = false;
        if (kstrcmp(path, "/proc/self/exe") == 0)
            is_exe = true;
        else {
            /* /proc/<digits>/exe */
            const char *s = path;
            if (s[0]=='/' && s[1]=='p' && s[2]=='r' && s[3]=='o' && s[4]=='c' && s[5]=='/') {
                s += 6;
                while (*s >= '0' && *s <= '9') s++;
                if (kstrcmp(s, "/exe") == 0) is_exe = true;
            }
        }
        if (is_exe) {
            const char *exe = p->linux_exe_path[0] ? p->linux_exe_path : "/bin/busybox.exe";
            size_t n = strlen(exe);
            if (bufsiz == 0) return -EINVAL;
            if (n > bufsiz) n = bufsiz;
            if (lx_put(ubuf, exe, n)) return -EFAULT;
            return (long)n;
        }
        return lx_path_kind(path, NULL) ? -EINVAL : -ENOENT;   /* no real symlinks */
    }
    case LX_getcwd: {
        const char *cwd = p->cwd[0] ? p->cwd : "/";
        size_t n = strlen(cwd) + 1;
        if (a2 < n) return -ERANGE;
        if (lx_put((void *)a1, cwd, n)) return -EFAULT;
        return (long)n;
    }
    case LX_chdir:
        if ((r = lx_resolve(p, LX_AT_FDCWD, (const char *)a1, path, sizeof(path), false))) return r;
        r = lx_path_kind(path, NULL);
        if (r != 2) return r ? -ENOTDIR : -ENOENT;
        if (strlen(path) >= sizeof(p->cwd)) return -ENAMETOOLONG;
        strcpy(p->cwd, path);
        return 0;
    case LX_fchdir:
        if (!lx_fd_ok(p, (long)a1)) return -EBADF;
        if (p->fds[a1].fd_type != FD_TYPE_VFS_DIR || !p->fds[a1].file_handle) return -ENOTDIR;
        kstrncpy(p->cwd, (const char *)p->fds[a1].file_handle, sizeof(p->cwd));
        return 0;
    case LX_mkdir: case LX_mkdirat: {
        long dfd = nr == LX_mkdir ? LX_AT_FDCWD : (long)(int)a1;
        const char *up = (const char *)(nr == LX_mkdir ? a1 : a2);
        if ((r = lx_resolve(p, dfd, up, path, sizeof(path), false))) return r;
        if (lx_path_kind(path, NULL)) return -EEXIST;
        if (!vfs_has_root_mount()) return -ENODEV;
        r = vfs_mkdir(path);
        return r < 0 ? (r == -1 ? -EIO : r) : 0;
    }
    case LX_rmdir: case LX_unlink: case LX_unlinkat: {
        long dfd = nr == LX_unlinkat ? (long)(int)a1 : LX_AT_FDCWD;
        const char *up = (const char *)(nr == LX_unlinkat ? a2 : a1);
        if ((r = lx_resolve(p, dfd, up, path, sizeof(path), false))) return r;
        int kind = lx_path_kind(path, NULL);
        if (!kind) return -ENOENT;
        bool want_dir = nr == LX_rmdir || (nr == LX_unlinkat && (a3 & LX_AT_REMOVEDIR));
        if (want_dir && kind != 2) return -ENOTDIR;
        if (!want_dir && kind == 2) return -EISDIR;
        if (want_dir) {
            extern int fat32_remove_empty_directory(const char *path);
            r = fat32_remove_empty_directory(path);
        } else {
            r = vfs_unlink(path);
        }
        return r < 0 ? (r == -1 ? -EIO : r) : 0;
    }
    case LX_rename: case LX_renameat: case LX_renameat2: {
        long d1 = nr == LX_rename ? LX_AT_FDCWD : (long)(int)a1;
        const char *u1 = (const char *)(nr == LX_rename ? a1 : a2);
        long d2 = nr == LX_rename ? LX_AT_FDCWD : (long)(int)a3;
        const char *u2 = (const char *)(nr == LX_rename ? a2 : a4);
        if ((r = lx_resolve(p, d1, u1, path, sizeof(path), false))) return r;
        if ((r = lx_resolve(p, d2, u2, path2, sizeof(path2), false))) return r;
        if (!lx_path_kind(path, NULL)) return -ENOENT;
        r = vfs_rename(path, path2);
        return r < 0 ? (r == -1 ? -EIO : r) : 0;
    }
    case LX_truncate: case LX_ftruncate: {
        /* Best-effort: FAT32 truncate exists for paths; ftruncate via vfs. */
        if (nr == LX_truncate) {
            if ((r = lx_resolve(p, LX_AT_FDCWD, (const char *)a1, path, sizeof(path), false)))
                return r;
            extern int fat32_truncate_file(const char *path);
            if (!lx_path_kind(path, NULL)) return -ENOENT;
            return fat32_truncate_file(path) == 0 ? 0 : -EIO;
        }
        if (!lx_fd_ok(p, (long)a1)) return -EBADF;
        if (p->fds[a1].fd_type == FD_TYPE_DEV_NULL) return 0;
        if (p->fds[a1].fd_type != FD_TYPE_VFS) return -EINVAL;
        /* No reliable VFS ftruncate; accept and ignore length for ash scripts. */
        (void)a2;
        return 0;
    }
    case LX_umask:   return sys_umask((mode_t)a1);
    case LX_chmod: case LX_fchmod: case LX_fchmodat:
    case LX_chown: case LX_fchown: case LX_lchown: case LX_fchownat:
        return 0;                               /* accepted, not enforced */
    case LX_utimensat: {
        /* BusyBox touch: utimensat first; on ENOENT it open(O_CREAT).
         * Always returning 0 made touch exit success without creating. */
        long dfd = (long)(int)a1;
        const char *up = (const char *)a2;
        if (!up) return -EFAULT;
        if ((r = lx_resolve(p, dfd, up, path, sizeof(path), (a4 & 0x1000) != 0)))
            return r; /* AT_EMPTY_PATH */
        if (!lx_path_kind(path, NULL)) return -ENOENT;
        return 0; /* timestamps not stored on FAT32 yet */
    }
    case LX_statfs: case LX_fstatfs: {
        uint64_t sf[15];                        /* struct statfs, 120 bytes */
        memset(sf, 0, sizeof(sf));
        sf[0] = 0x4d44;                         /* MSDOS_SUPER_MAGIC */
        sf[1] = 4096;                           /* f_bsize */
        sf[11] = 255;                           /* f_namelen */
        sf[12] = 4096;                          /* f_frsize */
        return lx_put((void *)a2, sf, sizeof(sf));
    }

    /* ---- memory ---- */
    case LX_brk:     return (long)sys_brk((void *)a1);
    case LX_mmap:    return lx_mmap(p, a1, a2, (long)a3, (long)a4, (long)(int)a5, (int64_t)a6);
    case LX_munmap:  return sys_munmap((void *)a1, (size_t)a2);
    case LX_mprotect:
        return sys_mprotect((void *)a1, (size_t)a2, (int)a3);
    case LX_madvise: case LX_msync:
        return 0;
    case LX_mremap:  return -ENOMEM;            /* callers fall back to copy */

    /* ---- process ---- */
    case LX_exit: case LX_exit_group:
        sys_exit((int)(a1 & 0xff));
        return 0;
    case LX_getpid: case LX_gettid:
        return p->pid;
    case LX_getppid: return sys_getppid();
    case LX_getuid: case LX_geteuid: return p->uid;
    case LX_getgid: case LX_getegid: return p->gid;
    case LX_getresuid: case LX_getresgid: {
        uint32_t v = nr == LX_getresuid ? p->uid : p->gid;
        if (lx_put((void *)a1, &v, 4) || lx_put((void *)a2, &v, 4) || lx_put((void *)a3, &v, 4))
            return -EFAULT;
        return 0;
    }
    case LX_setuid:  return sys_setuid((uint32_t)a1);
    case LX_setgid:  return sys_setgid((uint32_t)a1);
    case LX_setreuid: case LX_setregid: case LX_setresuid: case LX_setresgid:
    case LX_setgroups:
        return p->uid == 0 ? 0 : -EPERM;
    case LX_getgroups: return 0;
    case LX_getpgrp:
        return (long)p->pgid;
    case LX_getpgid:
        return sys_getpgid((pid_t)(int)a1);
    case LX_getsid:
        { extern int sys_getsid(pid_t); return sys_getsid((pid_t)(int)a1); }
    case LX_setpgid:
        return sys_setpgid((pid_t)(int)a1, (pid_t)(int)a2);
    case LX_setsid:
        return sys_setsid();
    case LX_set_tid_address:
        p->linux_clear_child_tid = a1;
        return p->pid;
    case LX_set_robust_list: return 0;
    case LX_prctl:
        if (a1 == 16) return lx_put((void *)a2, "indevos", 8);   /* PR_GET_NAME */
        return 0;
    case LX_arch_prctl:
        switch (a1) {
            case LX_ARCH_SET_FS:
                if (!is_user_address(a2)) return -EPERM;
                thread_wrfsbase(a2);
                /* Keep thread_t.fs_base in sync — fork children copy it, and
                 * context switch restores from thread_t, not only the MSR. */
                {
                    thread_t *ct = scheduler_get_current_thread();
                    if (ct)
                        ct->fs_base = a2;
                }
                return 0;
            case LX_ARCH_GET_FS: {
                uint64_t v = thread_rdfsbase();
                return lx_put((void *)a2, &v, sizeof(v));
            }
            default:
                return -EINVAL;     /* GS base: not saved across switches */
        }
    case LX_fork: {
        fork_snapshot_frame(p, fr);
        p->fork_defer_release = 0;
        return sys_fork();
    }
    case LX_vfork: {
        /* vfork == clone(CLONE_VM|CLONE_VFORK|SIGCHLD) */
        fork_snapshot_frame(p, fr);
        p->fork_defer_release = 0;
        return sys_clone(CLONE_VM | CLONE_VFORK | 17 /* SIGCHLD */, 0, 0, 0, 0);
    }
    case LX_clone: {
        /* flags, newsp, parent_tid, child_tid, tls (x86-64 Linux ABI) */
        fork_snapshot_frame(p, fr);
        /* Child stack (newsp = a2) is applied by sys_clone. */
        p->fork_defer_release = 0;
        return sys_clone(a1, a2, a3, a4, a5);
    }
    case LX_execve: {
        r = sys_exec((const char *)a1, (char *const *)a2);
        if (r < 0) return r;
                /* In-place replace: SYSRET into the new entry (same pid). */
        if (p && p->exec_replaced) {
            p->exec_replaced = false;
            fr->user_rip = p->exec_entry_rip;
            fr->user_rsp = p->exec_entry_rsp;
            fr->user_rflags = 0x202; /* IF set */
            /* Clear argument regs; _start reads argc/argv from the stack. */
            fr->a1 = fr->a2 = fr->a3 = fr->a4 = fr->a5 = fr->a6 = 0;
            fr->rbx = fr->rbp = fr->r12 = fr->r13 = fr->r14 = fr->r15 = 0;
            /* CLONE_VFORK: release parent now that child has a private image. */
            if (p->fork_parent) {
                process_t *vp = p->fork_parent;
                p->fork_parent = NULL;
                if (vp->sched_thread)
                    thread_wake((thread_t *)vp->sched_thread);
            }
            p->clone_shared_vm = false;
            return 0;
        }
        /* Legacy nested path returned an exit status — terminate. */
        sys_exit((int)(r & 0xff));
        return 0;
    }
    case LX_wait4:   return lx_wait4(p, (long)(int)a1, (int *)a2, (long)a3, (void *)a4);
    case LX_kill: {
        /* Queue + deliver (custom handlers / SIG_IGN). Do NOT always
         * lx_die_by_signal on self — that broke sig_self (SIGUSR1 handler). */
        if (a2 == 0) {
            /* Existence / permission check only */
            if ((long)(int)a1 == (long)p->pid || (int)a1 == 0) return 0;
            return sys_kill((pid_t)a1, 0);
        }
        if ((int)a1 == 0) {
            /* kill(0, sig): send to caller's process group */
            process_kill_pgid(p->pgid ? p->pgid : p->pid, (int)a2);
            if (p && !p->is_running) lx_die_by_signal(p, (int)a2);
            return 0;
        }
        if ((int)a1 < -1) {
            /* kill(-pgid, sig) */
            process_kill_pgid((uint32_t)(-(int)a1), (int)a2);
            return 0;
        }
        long r = sys_kill((pid_t)a1, (int)a2);
        /* If we killed ourselves with a fatal default action, exit now. */
        if (r == 0 && p && !p->is_running)
            lx_die_by_signal(p, (int)a2);
        return r;
    }
    case LX_tkill: case LX_tgkill: {
        long tid = nr == LX_tkill ? (long)a1 : (long)a2;
        long sig = nr == LX_tkill ? (long)a2 : (long)a3;
        if (sig == 0) return 0;
        if (tid == (long)p->pid || tid == 0)
            return sys_kill((pid_t)p->pid, (int)sig);
        return sys_kill((pid_t)tid, (int)sig);
    }
    case LX_rt_sigaction: {
        /* Linux: act/oldact are struct sigaction (handler, flags, restorer, mask).
         * Map onto our sigaction_ind_t (handler + flags); stash restorer in
         * signal_sa_flags high bit unused — keep restorer in a per-signal slot
         * via sa_flags field's companion: we store restorer in pending unused
         * by using signal_sa_flags as flags only and a parallel restorer array
         * would be ideal; for now store restorer address in signal_sa_flags if
         * SA_RESTORER (0x04000000) is set... actually keep flags and use
         * a side table. Simpler: call sys_sigaction with ind layout built here. */
        if (a1 < 1 || a1 >= NSIG) return -EINVAL;
        if ((a1 == 9 || a1 == 19) && a2) return -EINVAL; /* KILL/STOP */
        typedef struct {
            uint64_t handler;
            uint64_t flags;
            uint64_t restorer;
            uint64_t mask;
        } lx_sa_t;
        /* a4 is sigsetsize (must be 8), NOT sizeof(sigaction). Always
         * copy the full kernel sigaction (handler,flags,restorer,mask). */
        if (a4 && a4 != 8) return -EINVAL;
        lx_sa_t old_lx;
        memset(&old_lx, 0, sizeof(old_lx));
        old_lx.handler = (uint64_t)(uintptr_t)p->signal_handlers[a1];
        old_lx.flags = p->signal_sa_flags[a1];
        old_lx.restorer = p->sig_restorer[a1];
        if (a3) {
            if (lx_put((void *)a3, &old_lx, sizeof(old_lx)))
                return -EFAULT;
        }
        if (a2) {
            lx_sa_t act;
            memset(&act, 0, sizeof(act));
            if (lx_get(&act, (void *)a2, sizeof(act))) return -EFAULT;
            p->signal_handlers[a1] = (sighandler_t)(uintptr_t)act.handler;
            p->signal_sa_flags[a1] = (uint32_t)act.flags;
            p->sig_restorer[a1] = act.restorer;
        }
        return 0;
    }
    case LX_rt_sigprocmask: {
        /* how, set, oldset, sigsetsize.
         * Linux sigset bit (N-1) == signal N; our blocked_signals uses bit N. */
        uint64_t old_lx = ((uint64_t)p->blocked_signals) >> 1;
        if (a3) {
            if (lx_put((void *)a3, &old_lx, a4 >= 8 ? 8 : (size_t)a4)) return -EFAULT;
        }
        if (a2) {
            uint64_t s = 0;
            if (lx_get(&s, (void *)a2, a4 >= 8 ? 8 : (size_t)a4)) return -EFAULT;
            uint32_t sm = (uint32_t)((s << 1) & 0xFFFFFFFEu); /* linux -> internal */
            if (a1 == 0) p->blocked_signals |= sm;       /* SIG_BLOCK */
            else if (a1 == 1) p->blocked_signals &= ~sm; /* SIG_UNBLOCK */
            else if (a1 == 2) p->blocked_signals = sm;   /* SIG_SETMASK */
            else return -EINVAL;
            p->blocked_signals &= ~((1u << 9) | (1u << 19)); /* cannot block KILL/STOP */
        }
        return 0;
    }
    case LX_rt_sigpending: {
        uint64_t z = ((uint64_t)p->pending_signals) >> 1; /* internal -> linux */
        return lx_put((void *)a1, &z, sizeof(z));
    }
    case LX_rt_sigreturn: {
        if (!p->sig_in_handler) return 0;
        p->sig_in_handler = false;
        if (p->sig_from_irq) {
            p->sig_from_irq = false;
            p->sig_iret_pending = true; /* handled right after lx_dispatch */
            return 0;
        }
        /* Restore FULL GPR set into this syscall's live frame. The SYSCALL
         * epilogue pops r15..rbx and a6..a1 from the frame before sysret/iretq,
         * so partial restore left handler/callee-saved regs in place and ash
         * later called sigsetjmp with rdi still holding clone flags (0x11). */
        fr->r15 = p->sig_saved_r15;
        fr->r14 = p->sig_saved_r14;
        fr->r13 = p->sig_saved_r13;
        fr->r12 = p->sig_saved_r12;
        fr->rbp = p->sig_saved_rbp;
        fr->rbx = p->sig_saved_rbx;
        fr->a6  = p->sig_saved_r9;
        fr->a5  = p->sig_saved_r8;
        fr->a4  = p->sig_saved_r10;
        fr->a3  = p->sig_saved_rdx;
        fr->a2  = p->sig_saved_rsi;
        fr->a1  = p->sig_saved_rdi;
        fr->user_rip = p->sig_saved_rip;
        fr->user_rsp = p->sig_saved_rsp;
        fr->user_rflags = p->sig_saved_rflags | 0x200ULL; /* IF */
        return (long)p->sig_saved_rax;
    }
    case LX_pause: {
        extern bool proc_signal_interrupts(process_t *);
        while (!proc_signal_interrupts(p))
            ktime_yield_until(ktime_deadline_ms(5));
        return -EINTR;
    }
    case LX_rt_sigsuspend: {
        /* Replace mask, wait for an unblocked pending signal, restore, EINTR.
         * Ash uses this after pipelines; leaving it -ENOSYS hung the suite. */
        uint32_t old = p->blocked_signals;
        if (a1) {
            uint64_t s = 0;
            size_t n = a2 >= 8 ? 8 : (size_t)a2;
            if (n == 0) n = 8;
            if (lx_get(&s, (void *)a1, n)) return -EFAULT;
            uint32_t sm = (uint32_t)((s << 1) & 0xFFFFFFFEu);
            p->blocked_signals = sm & ~((1u << 9) | (1u << 19));
        }
        for (int i = 0; i < 500; i++) {
            if (p->pending_signals & ~p->blocked_signals) break;
            ktime_yield_until(ktime_deadline_ms(5));
        }
        p->blocked_signals = old;
        return -EINTR;
    }
    case LX_sigaltstack:
        if (a2) {
            uint64_t ss[3] = { 0, 2 /* SS_DISABLE */, 0 };
            if (lx_put((void *)a2, ss, sizeof(ss))) return -EFAULT;
        }
        return 0;
    case LX_alarm: case LX_setitimer: case LX_getitimer:
        return 0;

    /* ---- time ---- */
    case LX_clock_gettime: {
        uint64_t us;
        switch (a1) {
            case 0: case 5: case 8: case 11: us = lx_real_us(); break; /* REALTIME* , TAI */
            default: us = lx_mono_us(); break;
        }
        lx_timespec_t ts = { (int64_t)(us / 1000000ULL), (int64_t)(us % 1000000ULL) * 1000 };
        return lx_put((void *)a2, &ts, sizeof(ts));
    }
    case LX_clock_getres: {
        if (!a2) return 0;
        lx_timespec_t ts = { 0, 1000 };
        return lx_put((void *)a2, &ts, sizeof(ts));
    }
    case LX_gettimeofday: {
        if (a1) {
            uint64_t us = lx_real_us();
            lx_timeval_t tv = { (int64_t)(us / 1000000ULL), (int64_t)(us % 1000000ULL) };
            if (lx_put((void *)a1, &tv, sizeof(tv))) return -EFAULT;
        }
        if (a2) {
            int tz[2] = { 0, 0 };
            if (lx_put((void *)a2, tz, sizeof(tz))) return -EFAULT;
        }
        return 0;
    }
    case LX_time: {
        int64_t t = (int64_t)(lx_real_us() / 1000000ULL);
        if (a1 && lx_put((void *)a1, &t, sizeof(t))) return -EFAULT;
        return t;
    }
    case LX_nanosleep:
        return sys_nanosleep((const timespec_ind_t *)a1, (timespec_ind_t *)a2);
    case LX_clock_nanosleep: {
        lx_timespec_t ts;
        if (lx_get(&ts, (void *)a3, sizeof(ts))) return -EFAULT;
        uint64_t want_us = (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
        if (a2 & 1) {                           /* TIMER_ABSTIME */
            uint64_t now = (a1 == 0) ? lx_real_us() : lx_mono_us();
            want_us = want_us > now ? want_us - now : 0;
        }
        {
            extern bool proc_signal_interrupts(process_t *);
            extern bool ktime_expired(uint64_t);
            uint64_t dl = ktime_deadline_ms((want_us + 999) / 1000);
            while (!ktime_expired(dl)) {
                if (proc_signal_interrupts(p)) return -EINTR;
                __asm__ volatile("sti; hlt" ::: "memory");
            }
        }
        return 0;
    }
    case LX_times: {
        if (a1) {
            uint64_t z[4] = { 0, 0, 0, 0 };
            if (lx_put((void *)a1, z, sizeof(z))) return -EFAULT;
        }
        return (long)(lx_mono_us() / 10000ULL);  /* clock ticks @ 100 Hz */
    }
    case LX_getrusage: {
        uint8_t z[144];
        memset(z, 0, sizeof(z));
        return lx_put((void *)a2, z, sizeof(z));
    }

    /* ---- system info ---- */
    case LX_uname: {
        lx_utsname_t u;
        memset(&u, 0, sizeof(u));
        kstrncpy(u.sysname, "Linux", sizeof(u.sysname));
        kstrncpy(u.nodename, "indevos", sizeof(u.nodename));
        kstrncpy(u.release, "6.1.0-indevos", sizeof(u.release));
        kstrncpy(u.version, "#1 inDevOS Linux ABI layer", sizeof(u.version));
        kstrncpy(u.machine, "x86_64", sizeof(u.machine));
        kstrncpy(u.domainname, "(none)", sizeof(u.domainname));
        return lx_put((void *)a1, &u, sizeof(u));
    }
    case LX_sysinfo: {
        uint8_t si[112];
        memset(si, 0, sizeof(si));
        int64_t up = (int64_t)(lx_mono_us() / 1000000ULL);
        memcpy(si, &up, 8);
        uint32_t unit = 1;
        memcpy(si + 104, &unit, 4);
        uint16_t procs = 1;
        memcpy(si + 80, &procs, 2);
        return lx_put((void *)a1, si, sizeof(si));
    }
    case LX_getrlimit: {
        lx_rlimit_t rl;
        lx_rlimit((long)a1, &rl);
        return lx_put((void *)a2, &rl, sizeof(rl));
    }
    case LX_setrlimit: return 0;
    case LX_prlimit64: {
        if (a4) {
            lx_rlimit_t rl;
            lx_rlimit((long)a2, &rl);
            if (lx_put((void *)a4, &rl, sizeof(rl))) return -EFAULT;
        }
        return 0;
    }
    case LX_sched_yield: return 0;
    case LX_sched_getaffinity: {
        if (a2 < 8) return -EINVAL;
        uint64_t mask = 1;
        if (lx_put((void *)a3, &mask, sizeof(mask))) return -EFAULT;
        return 8;
    }
    case LX_getcpu: {
        uint32_t z = 0;
        if (a1 && lx_put((void *)a1, &z, 4)) return -EFAULT;
        if (a2 && lx_put((void *)a2, &z, 4)) return -EFAULT;
        return 0;
    }
    case LX_statx: {
        /* Soft: ignore mask/flags quirks; fill a Linux struct statx from path. */
        long dfd = (long)(int)a1;
        const char *up = (const char *)a2;
        if ((r = lx_resolve(p, dfd, up, path, sizeof(path), (a3 & 0x1000) != 0))) /* AT_EMPTY_PATH */
            return r;
        stat_t st;
        if (!lx_path_kind(path, &st)) return -ENOENT;
        /* Linux struct statx is 0x100 bytes; fill the common fields. */
        uint8_t sx[0x100];
        memset(sx, 0, sizeof(sx));
        /* stx_mask at 0x00 = STATX_BASIC_STATS */
        uint32_t mask = 0x000007ffu;
        memcpy(sx + 0x00, &mask, 4);
        uint16_t mode = (uint16_t)st.st_mode;
        memcpy(sx + 0x1c, &mode, 2);
        uint64_t ino = st.st_ino, size = st.st_size;
        memcpy(sx + 0x40, &ino, 8);
        memcpy(sx + 0x48, &size, 8);
        return lx_put((void *)a5, sx, sizeof(sx));
    }
    case LX_getrandom: {
        size_t n = (size_t)a2;
        if (n > 4096) n = 4096;
        uint8_t buf[256];
        size_t done = 0;
        while (done < n) {
            size_t c = n - done;
            if (c > sizeof(buf)) c = sizeof(buf);
            for (size_t i = 0; i < c; i += 8) {
                uint64_t v = lx_random_u64();
                size_t k = c - i < 8 ? c - i : 8;
                memcpy(buf + i, &v, k);
            }
            if (lx_put((uint8_t *)a1 + done, buf, c)) return done ? (long)done : -EFAULT;
            done += c;
        }
        return (long)done;
    }
    
    case LX_sendfile: {
        /* sendfile(out_fd, in_fd, off_t *offset, size_t count) */
        long out_fd = (long)(int)a1, in_fd = (long)(int)a2;
        off_t *uoff = (off_t *)a3;
        size_t count = (size_t)a4;
        if (!lx_fd_ok(p, out_fd) || !lx_fd_ok(p, in_fd)) return -EBADF;
        off_t off = 0;
        if (uoff) {
            if (lx_get(&off, uoff, sizeof(off))) return -EFAULT;
        }
        if (count > (1u << 20)) count = 1u << 20; /* 1 MiB cap per call */
        uint8_t *buf = (uint8_t *)kalloc(count ? (uint32_t)count : 1);
        if (!buf) return -ENOMEM;
        long saved = -1;
        if (uoff) {
            saved = sys_seek((int)in_fd, 0, SEEK_CUR);
            if (saved < 0) { kfree(buf); return saved; }
            r = sys_seek((int)in_fd, off, SEEK_SET);
            if (r < 0) { kfree(buf); return r; }
        }
        long total = 0;
        while ((size_t)total < count) {
            long n = (long)sys_read((int)in_fd, buf + total, count - (size_t)total);
            if (n < 0) { total = n; break; }
            if (n == 0) break;
            long woff = 0;
            while (woff < n) {
                long w = (long)sys_write((int)out_fd, buf + total + woff, (size_t)(n - woff));
                if (w < 0) { total = w; goto sf_done; }
                if (w == 0) break;
                woff += w;
            }
            total += woff;
            if (woff < n) break;
        }
    sf_done:
        if (uoff && total >= 0) {
            off_t noff = off + (off_t)total;
            (void)lx_put(uoff, &noff, sizeof(noff));
            if (saved >= 0) (void)sys_seek((int)in_fd, (off_t)saved, SEEK_SET);
        }
        kfree(buf);
        return total;
    }
    case LX_copy_file_range: {
        /* copy_file_range(fd_in, off_in, fd_out, off_out, len, flags) */
        long in_fd = (long)(int)a1, out_fd = (long)(int)a3;
        off_t *uoff_in = (off_t *)a2, *uoff_out = (off_t *)a4;
        size_t len = (size_t)a5;
        (void)a6;
        if (!lx_fd_ok(p, in_fd) || !lx_fd_ok(p, out_fd)) return -EBADF;
        /* Reuse sendfile-style loop via temporary offset dance. */
        off_t in_off = 0, out_off = 0;
        if (uoff_in && lx_get(&in_off, uoff_in, sizeof(in_off))) return -EFAULT;
        if (uoff_out && lx_get(&out_off, uoff_out, sizeof(out_off))) return -EFAULT;
        if (len > (1u << 20)) len = 1u << 20;
        uint8_t *buf = (uint8_t *)kalloc(len ? (uint32_t)len : 1);
        if (!buf) return -ENOMEM;
        long sin = sys_seek((int)in_fd, 0, SEEK_CUR);
        long sout = sys_seek((int)out_fd, 0, SEEK_CUR);
        if (uoff_in) { r = sys_seek((int)in_fd, in_off, SEEK_SET); if (r < 0) { kfree(buf); return r; } }
        if (uoff_out) { r = sys_seek((int)out_fd, out_off, SEEK_SET); if (r < 0) { kfree(buf); return r; } }
        long total = 0;
        while ((size_t)total < len) {
            long n = (long)sys_read((int)in_fd, buf, len - (size_t)total);
            if (n < 0) { total = n; break; }
            if (n == 0) break;
            long woff = 0;
            while (woff < n) {
                long w = (long)sys_write((int)out_fd, buf + woff, (size_t)(n - woff));
                if (w <= 0) { if (w < 0) total = w; goto cfr_done; }
                woff += w;
            }
            total += woff;
        }
    cfr_done:
        if (total >= 0) {
            if (uoff_in) { off_t n = in_off + (off_t)total; (void)lx_put(uoff_in, &n, sizeof(n)); }
            if (uoff_out) { off_t n = out_off + (off_t)total; (void)lx_put(uoff_out, &n, sizeof(n)); }
        }
        if (sin >= 0 && uoff_in) (void)sys_seek((int)in_fd, (off_t)sin, SEEK_SET);
        if (sout >= 0 && uoff_out) (void)sys_seek((int)out_fd, (off_t)sout, SEEK_SET);
        if (!uoff_in && sin >= 0) (void)sys_seek((int)in_fd, (off_t)sin + (total > 0 ? total : 0), SEEK_SET);
        if (!uoff_out && sout >= 0) (void)sys_seek((int)out_fd, (off_t)sout + (total > 0 ? total : 0), SEEK_SET);
        kfree(buf);
        return total;
    }

    case LX_futex: {
        long op = (long)(a2 & 0x7f);
        if (op == 1 /* WAKE */ || op == 10 /* WAKE_BITSET */)
            return 0;
        if (op == 0 /* WAIT */ || op == 9 /* WAIT_BITSET */) {
            uint32_t cur;
            if (lx_get(&cur, (void *)a1, 4)) return -EFAULT;
            /* Single-threaded processes: nobody could ever wake us. */
            return cur != (uint32_t)a3 ? -EAGAIN : -ETIMEDOUT;
        }
        return lx_enosys(p, nr);
    }

    default:
        return lx_enosys(p, nr);
    }
}

/* Full register image for returning from an IRQ-delivered handler. Field
 * order matches lx_iret_full (syscallasm.asm): GPR pops, then iretq frame. */
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t rip, cs, rflags, rsp, ss;
} lx_iret_regs_t;
extern void lx_iret_full(lx_iret_regs_t *regs) __attribute__((noreturn));

__attribute__((noreturn))
static void lx_sigreturn_iret(process_t *p) {
    lx_iret_regs_t g;
    g.r15 = p->sig_saved_r15; g.r14 = p->sig_saved_r14;
    g.r13 = p->sig_saved_r13; g.r12 = p->sig_saved_r12;
    g.r11 = p->sig_saved_r11; g.r10 = p->sig_saved_r10;
    g.r9  = p->sig_saved_r9;  g.r8  = p->sig_saved_r8;
    g.rbp = p->sig_saved_rbp; g.rdi = p->sig_saved_rdi;
    g.rsi = p->sig_saved_rsi; g.rdx = p->sig_saved_rdx;
    g.rcx = p->sig_saved_rcx; g.rbx = p->sig_saved_rbx;
    g.rax = p->sig_saved_rax;
    g.rip = p->sig_saved_rip;
    g.rsp = p->sig_saved_rsp;
    /* Only arithmetic/DF flags from the user image; IF and bit 1 forced,
     * IOPL/NT/RF/VM/AC never taken from user. User selectors fixed. */
    g.rflags = (p->sig_saved_rflags & 0x0CD5ULL) | 0x202ULL;
    g.cs = 0x23;
    g.ss = 0x1B;
    if (g.rip == 0 || (g.rip >> 47) || (g.rsp >> 47)) {
        serial_printf("irqsig: bad sigreturn image pid=%u rip=0x%llx rsp=0x%llx -> SIGSEGV\n",
                      p->pid, (unsigned long long)g.rip, (unsigned long long)g.rsp);
        p->exit_status = 128 + 11;
        p->is_running = false;
        sys_exit(128 + 11);
        __builtin_unreachable();
    }
    lx_iret_full(&g);
}

uint64_t linux_syscall_dispatch(linux_syscall_frame_t *f) {
    process_t *p = lx_proc();

    /* Every SYSCALL entry: stash live user RIP/RSP/RFLAGS/GPRs into THIS
     * thread before any dispatch. Nested fork reads these by value. */
    {
        thread_t *thr = scheduler_get_current_thread();
        /* Boot-thread ash has owner_process==NULL. Pin while lx_proc() still
         * names a live usermode process (before a child-exit poisons the
         * global to kernel/parent). Later get_current_process() uses owner. */
        if (thr) {
            if (thr->owner_process) {
                process_t *op = (process_t *)thr->owner_process;
                if (op && op->is_usermode && !op->is_zombie)
                    p = op;
            } else if (p && p->is_usermode && !p->is_zombie && p->pid != 0) {
                thr->owner_process = (struct process *)p;
                if (!p->sched_thread)
                    p->sched_thread = thr;
            }
        }
        if (thr) {
            thr->sc_user_rip = f->user_rip;
            thr->sc_user_rsp = f->user_rsp;
            thr->sc_user_rflags = f->user_rflags;
            thr->sc_r15 = f->r15; thr->sc_r14 = f->r14;
            thr->sc_r13 = f->r13; thr->sc_r12 = f->r12;
            thr->sc_rbp = f->rbp; thr->sc_rbx = f->rbx;
            thr->sc_r9 = f->a6; thr->sc_r8 = f->a5;
            thr->sc_r10 = f->a4; thr->sc_rdx = f->a3;
            thr->sc_rsi = f->a2; thr->sc_rdi = f->a1;
            thr->sc_frame_valid = 1;
        }
    }

    /* CR3 re-sync is done inside process_deliver_signals (and fork paths),
     * not on every SYSCALL -- boot-thread ash has owner_process==NULL so
     * lx_proc() can briefly name the wrong process under SMP. */

    if (p && !p->linux_abi) {

        p->linux_abi = true;
        serial_printf("linux: PID %u entered the Linux syscall ABI (first nr=%llu)\n",
                      p->pid, (unsigned long long)f->nr);
    }

    /* Ctrl+C on the foreground program (same policy as the INT 0x80 path,
     * minus graceful SIGINT delivery: Linux signal handlers never run yet). */
    if (get_terminate_flag() && get_program_running()) {
        set_terminate_flag(false);
        uint32_t fg = process_get_fg_pgid();
        if (!fg && p) fg = p->pgid ? p->pgid : p->pid;
        process_kill_pgid(fg, SIGINT);
        if (p && !p->is_running)
            return 0;
    }

    long r = lx_dispatch(f);

    if (p && p->sig_iret_pending && f->nr == (uint64_t)LX_rt_sigreturn) {
        p->sig_iret_pending = false;
        lx_sigreturn_iret(p); /* noreturn */
    }

    /* SA_RESTART: rewind to the SYSCALL insn (2 bytes) with rax=nr so the
     * call re-executes after the handler. pause/sigsuspend never restart. */
    if (r == -EINTR && p && !p->sig_in_handler &&
        f->nr != (uint64_t)LX_pause && f->nr != (uint64_t)LX_rt_sigsuspend) {
        for (int s = 1; s < NSIG; s++) {
            uint32_t b = 1u << s;
            if (!(p->pending_signals & b) || (p->blocked_signals & b)) continue;
            if (s == SIGCHLD || s == SIGCONT) continue;
            sighandler_t h = p->signal_handlers[s];
            if (h == SIG_IGN || h == SIG_DFL) continue;
            if (p->signal_sa_flags[s] & SA_RESTART) {
                f->user_rip -= 2;
                r = (long)f->nr;
            }
            break;
        }
    }

    /* Deliver pending custom / default signals before SYSRET.
     * Stash syscall return in sig_saved_rax so rt_sigreturn restores it
     * (else write()-after-SIGPIPE returns n=0 instead of -EPIPE). */
    if (p && p->is_running) {
        /* Do not clobber sig_saved_rax while already in a handler — nested
         * syscalls (ash during SIGCHLD) would corrupt the outer return and
         * break pipelines / sigreturn. */
        if (!p->sig_in_handler)
            p->sig_saved_rax = (uint64_t)r;
        /* defer_sig_on_fork: delivering SIGCHLD into ash on fork/clone return
         * races with COW + held kids and has GPF'd right after PIPEOK 6.
         * Leave pending; next non-fork syscall delivers. */
        if (f->nr != (uint64_t)LX_fork && f->nr != (uint64_t)LX_clone &&
            f->nr != (uint64_t)LX_vfork)
            process_deliver_signals(p, f);
    }

    /* exit via sys_exit after fatal default signal to ourselves, so a fork
     * child becomes a real waitable zombie (closes fds, wakes parent). */
    if (p && !p->is_running && p->linux_abi) {
        serial_printf("linux: PID %u killed by signal %d\n", p->pid,
                      p->exit_status - 128);
        sys_exit(p->exit_status);
    }

    /* Wake pipeline pair after signals so writer SIGCHLD is not delivered
     * mid-fork-return (ash exit_group 255). */
    fork_release_after_syscall(p);

    if (linux_syscall_trace)
        serial_printf("linux: [%u] nr=%llu (%llx, %llx, %llx) = %ld\n",
                      p ? p->pid : 0, (unsigned long long)f->nr,
                      (unsigned long long)f->a1, (unsigned long long)f->a2,
                      (unsigned long long)f->a3, r);
    return (uint64_t)r;
}
