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
#include "inotify.h"
#include "lx_fbdev.h"
#include "wm_dev.h"
#include "input.h"
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
#define LX_O_NOFOLLOW  0x20000
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
    LX_sendfile = 40, LX_socket = 41, LX_connect = 42, LX_accept = 43,
    LX_sendto = 44, LX_recvfrom = 45, LX_sendmsg = 46, LX_recvmsg = 47,
    LX_shutdown = 48, LX_bind = 49, LX_listen = 50, LX_getsockname = 51,
    LX_getpeername = 52, LX_socketpair = 53, LX_setsockopt = 54,
    LX_getsockopt = 55, LX_accept4 = 288, LX_eventfd = 284, LX_eventfd2 = 290, LX_clone = 56, LX_fork = 57, LX_vfork = 58,
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
    LX_epoll_create = 213, LX_epoll_wait = 232, LX_epoll_ctl = 233,
    LX_epoll_pwait = 281, LX_epoll_create1 = 291, LX_epoll_pwait2 = 441,
    LX_timerfd_create = 283, LX_timerfd_settime = 286, LX_timerfd_gettime = 287,
    LX_signalfd = 282, LX_signalfd4 = 289, LX_rt_sigtimedwait = 128,
    LX_get_robust_list = 274, LX_utimensat = 280, LX_dup3 = 292,
    LX_pipe2 = 293, LX_prlimit64 = 302, LX_getcpu = 309,
    LX_renameat2 = 316, LX_copy_file_range = 326, LX_getrandom = 318, LX_membarrier = 324,
    LX_statx = 332, LX_rseq = 334, LX_faccessat2 = 439,
    LX_utime = 132, LX_mknod = 133, LX_getpriority = 140, LX_setpriority = 141,
    LX_sched_setaffinity = 203, LX_fadvise64 = 221, LX_utimes = 235,
    LX_waitid = 247, LX_mknodat = 259, LX_futimesat = 261, LX_linkat = 265,
    LX_symlinkat = 266, LX_fallocate = 285, LX_preadv = 295, LX_pwritev = 296,
    LX_preadv2 = 327, LX_pwritev2 = 328, LX_memfd_create = 319,
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
#include "pty.h"
#include "pipe.h"
/* /dev/zero, /dev/urandom tags in a DEV_NULL fd's file_handle */
#define LX_NULLTAG_ZERO    ((void *)1)
#define LX_NULLTAG_RANDOM  ((void *)2)
static long lx_read_nulltag(void *tag, void *ubuf, size_t count);
static bool lx_proc_text_path(process_t *p, const char *path);
/* kterm pty: fds on the desktop terminal's shell pipes are terminals. */
static kpty_t *lx_fd_pty(process_t *p, long fd) {
    if (!lx_fd_ok(p, fd)) return NULL;
    uint8_t ty = p->fds[fd].fd_type;
    if (ty != FD_TYPE_PIPE_READ && ty != FD_TYPE_PIPE_WRITE) return NULL;
    pipe_t *pp = (pipe_t *)p->fds[fd].file_handle;
    return pp ? (kpty_t *)pp->pty : NULL;
}

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

/* Linux socket glue (syscall_net.c) */
long lx_sock_socket(long domain, long type, long protocol);
long lx_sock_recv(int fd, void *ubuf, size_t len, int flags);
long lx_sock_send(int fd, const void *ubuf, size_t len, int flags);
long lx_sock_sendto(int fd, const void *ubuf, size_t len, int flags, const void *uaddr, uint32_t alen);
long lx_sock_recvfrom(int fd, void *ubuf, size_t len, int flags, void *uaddr, uint32_t *ualen);
long lx_sock_name(int fd, void *uaddr, uint32_t *ualen, bool peer);
long lx_sock_getsockopt(int fd, int level, int opt, void *uval, uint32_t *ulen);
long lx_sock_setsockopt(int fd, int level, int opt, const void *uval, uint32_t len);
long lx_sock_shutdown(int fd, int how);
bool lx_fd_is_socket(process_t *p, int fd);
int sys_bind(int fd, const sockaddr_in_t *addr, uint32_t addrlen);
int sys_listen(int fd, int backlog);
int sys_accept(int fd, sockaddr_in_t *addr, uint32_t *addrlen);
int sys_connect(int fd, sockaddr_in_t *addr, uint32_t addrlen);


/* eventfd: 64-bit counter object (curl uses it as its multi wakeup fd) */
#define FD_TYPE_EVENTFD 8
extern int lx_strace;
static long lx_put(void *uptr, const void *k, size_t n);
static long lx_get(void *k, const void *uptr, size_t n);
static long lx_eventfd_create(process_t *p, uint64_t initval, long flags) {
    int fd = allocate_fd(p);
    if (fd < 0) return -EMFILE;
    /* ctr[0] = counter, ctr[1] = EPOLLET edge generations (lo32 in, hi32 out) */
    uint64_t *ctr = (uint64_t *)kalloc(2 * sizeof(uint64_t));
    if (!ctr) { p->fds[fd].in_use = false; return -ENOMEM; }
    ctr[0] = initval;
    ctr[1] = 0;
    p->fds[fd].ref_count = fd_refcount_create();
    if (!p->fds[fd].ref_count) { kfree(ctr); p->fds[fd].in_use = false; return -ENOMEM; }
    p->fds[fd].fd_type = FD_TYPE_EVENTFD;
    p->fds[fd].file_handle = ctr;
    p->fds[fd].flags = (flags & 04000) ? O_NONBLOCK : 0;   /* EFD_NONBLOCK */
    p->fds[fd].position = 0;
    p->fds[fd].cloexec = (flags & 02000000) != 0;          /* EFD_CLOEXEC */
    p->fds[fd].in_use = true;
    return fd;
}

/* epoll: interest list over sys_poll_kernel.
 * Level-triggered by default.  EPOLLET reports an entry only on a new
 * readiness edge, as Linux does: when it becomes ready (or gains new bits
 * such as HUP), or when a new wakeup arrives for a reported bit -- data
 * added (EPOLLIN) / room freed (EPOLLOUT).  Pipes, AF_UNIX sockets and
 * eventfds keep exact edge generations; other objects use a per-object I/O
 * counter (any data-moving syscall on that object), which can only add a
 * spurious wakeup, never lose one.  EPOLLONESHOT disarms the entry after it
 * is reported (re-arm with EPOLL_CTL_MOD).  Entries whose fd was closed or
 * reused for another object are dropped silently. */
#define FD_TYPE_EPOLL 13
#define LX_EPOLL_MAX  64
#define LX_EPOLLONESHOT (1u << 30)
#define LX_EPOLLET      (1u << 31)
typedef struct __attribute__((packed)) { uint32_t events; uint64_t data; } lx_epoll_event_t;
typedef struct {
    int      fd;
    uint32_t events;
    uint64_t data;
    void    *key;        /* file_handle at ADD time: detects fd reuse */
    uint8_t  type;
    bool     armed;
    uint32_t et_rev;        /* EPOLLET: bits reported and still ready since */
    uint32_t et_in, et_out; /* edge generations at the last report */
} lx_epitem_t;
typedef struct { int n; lx_epitem_t it[LX_EPOLL_MAX]; } lx_epoll_t;

static bool lx_ep_valid(process_t *p, const lx_epitem_t *e) {
    if (e->fd < 0 || e->fd >= MAX_OPEN_FILES_SYSCALL) return false;
    const file_descriptor_t *f = &p->fds[e->fd];
    return f->in_use && f->file_handle == e->key && f->fd_type == e->type;
}
static void lx_ep_gc(process_t *p, lx_epoll_t *ep) {
    int w = 0;
    for (int i = 0; i < ep->n; i++)
        if (lx_ep_valid(p, &ep->it[i])) ep->it[w++] = ep->it[i];
    ep->n = w;
}
static long lx_epoll_create(process_t *p, long flags) {
    if (flags & ~02000000L) return -EINVAL;               /* only EPOLL_CLOEXEC */
    int fd = allocate_fd(p);
    if (fd < 0) return -EMFILE;
    lx_epoll_t *ep = (lx_epoll_t *)kalloc(sizeof(lx_epoll_t));
    if (!ep) { p->fds[fd].in_use = false; return -ENOMEM; }
    memset(ep, 0, sizeof(*ep));
    p->fds[fd].ref_count = fd_refcount_create();
    if (!p->fds[fd].ref_count) { kfree(ep); p->fds[fd].in_use = false; return -ENOMEM; }
    p->fds[fd].fd_type = FD_TYPE_EPOLL;
    p->fds[fd].file_handle = ep;
    p->fds[fd].flags = 0;
    p->fds[fd].position = 0;
    p->fds[fd].cloexec = (flags & 02000000L) != 0;
    p->fds[fd].in_use = true;
    return fd;
}
static lx_epoll_t *lx_ep_get(process_t *p, long epfd, long *err) {
    if (epfd < 0 || epfd >= MAX_OPEN_FILES_SYSCALL || !p->fds[epfd].in_use) { *err = -EBADF; return NULL; }
    if (p->fds[epfd].fd_type != FD_TYPE_EPOLL) { *err = -EINVAL; return NULL; }
    return (lx_epoll_t *)p->fds[epfd].file_handle;
}
static long lx_epoll_ctl(process_t *p, long epfd, long op, long fd, const void *uev) {
    long err = 0;
    lx_epoll_t *ep = lx_ep_get(p, epfd, &err);
    if (!ep) return err;
    if (fd < 0 || fd >= MAX_OPEN_FILES_SYSCALL || !p->fds[fd].in_use) return -EBADF;
    if (fd == epfd) return -EINVAL;
    uint8_t ty = p->fds[fd].fd_type;
    if (ty == FD_TYPE_EPOLL) return -EINVAL;              /* nested epoll: not supported */
    if (ty == FD_TYPE_VFS || ty == FD_TYPE_FILE || ty == FD_TYPE_VFS_DIR)
        return -EPERM;                                    /* Linux: regular files/dirs */
    lx_epoll_event_t ev = {0, 0};
    if (op != 2 /* DEL */) {
        if (!uev) return -EFAULT;
        if (lx_get(&ev, uev, sizeof(ev))) return -EFAULT;
    }
    lx_ep_gc(p, ep);
    int idx = -1;
    for (int i = 0; i < ep->n; i++) if (ep->it[i].fd == fd) { idx = i; break; }
    switch (op) {
    case 1: /* EPOLL_CTL_ADD */
        if (idx >= 0) return -EEXIST;
        if (ep->n >= LX_EPOLL_MAX) return -ENOSPC;
        idx = ep->n++;
        ep->it[idx].fd = (int)fd;
        ep->it[idx].key = p->fds[fd].file_handle;
        ep->it[idx].type = ty;
        /* fallthrough */
    case 3: /* EPOLL_CTL_MOD */
        if (idx < 0) return -ENOENT;
        ep->it[idx].events = ev.events;
        ep->it[idx].data = ev.data;
        ep->it[idx].armed = true;
        ep->it[idx].et_rev = 0;          /* ADD/MOD on a ready fd reports it */
        return 0;
    case 2: /* EPOLL_CTL_DEL */
        if (idx < 0) return -ENOENT;
        ep->it[idx] = ep->it[--ep->n];
        return 0;
    default:
        return -EINVAL;
    }
}
/* Per-object I/O counters for EPOLLET on objects without exact edge
 * generations (TCP/UDP sockets, ttys, timerfd, ...): bumped by every
 * data-moving syscall on an fd of that object.  A hash slot collision only
 * causes an extra (harmless) edge. */
#define LX_IOTAB 256
static volatile uint32_t lx_io_tab[LX_IOTAB];
static inline uint32_t lx_io_slot(const void *h) {
    uint64_t x = (uint64_t)(uintptr_t)h;
    x ^= x >> 17; x *= 0x9E3779B97F4A7C15ULL; x ^= x >> 29;
    return (uint32_t)(x & (LX_IOTAB - 1));
}
static void lx_io_bump(process_t *p, long fd) {
    if (!p || fd < 0 || fd >= MAX_OPEN_FILES_SYSCALL || !p->fds[fd].in_use) return;
    __atomic_add_fetch(&lx_io_tab[lx_io_slot(p->fds[fd].file_handle)], 1, __ATOMIC_RELEASE);
}
bool ux_fd_gens(void *file_handle, uint32_t *in_gen, uint32_t *out_gen);
/* Edge generations of an epoll entry's object. */
static void lx_ep_gens(process_t *p, const lx_epitem_t *it, uint32_t *gi, uint32_t *go) {
    const file_descriptor_t *f = &p->fds[it->fd];
    void *h = f->file_handle;
    if ((f->fd_type == FD_TYPE_PIPE_READ || f->fd_type == FD_TYPE_PIPE_WRITE) && h) {
        pipe_t *pp = (pipe_t *)h;
        *gi = __atomic_load_n(&pp->in_gen, __ATOMIC_ACQUIRE);
        *go = __atomic_load_n(&pp->out_gen, __ATOMIC_ACQUIRE);
        return;
    }
    if (f->fd_type == FD_TYPE_EVENTFD && h) {
        uint64_t g = __atomic_load_n(&((volatile uint64_t *)h)[1], __ATOMIC_ACQUIRE);
        *gi = (uint32_t)g; *go = (uint32_t)(g >> 32);
        return;
    }
    if (f->fd_type == FD_TYPE_SOCKET && ux_fd_gens(h, gi, go)) return;
    if (f->fd_type == FD_TYPE_INOTIFY && h) { *gi = inotify_gen(h); *go = 0; return; }
    *gi = *go = __atomic_load_n(&lx_io_tab[lx_io_slot(h)], __ATOMIC_ACQUIRE);
}
static long lx_epoll_wait(process_t *p, long epfd, void *uevs, long maxevents, int timeout_ms) {
    long err = 0;
    lx_epoll_t *ep = lx_ep_get(p, epfd, &err);
    if (!ep) return err;
    if (maxevents <= 0 || maxevents > 0x7fffffff / (long)sizeof(lx_epoll_event_t)) return -EINVAL;
    if (!uevs) return -EFAULT;
    extern uint64_t ktime_deadline_ms(uint64_t);
    extern bool     ktime_expired(uint64_t);
    extern bool     proc_signal_interrupts(process_t *);
    uint64_t dl = timeout_ms > 0 ? ktime_deadline_ms((uint64_t)timeout_ms) : 0;
    pollfd_t pf[LX_EPOLL_MAX];
    int map[LX_EPOLL_MAX];
    for (;;) {
        lx_ep_gc(p, ep);
        int np = 0;
        for (int i = 0; i < ep->n; i++) {
            if (!ep->it[i].armed) continue;
            pf[np].fd = ep->it[i].fd;
            /* poll bit values match EPOLLIN/PRI/OUT/ERR/HUP/RDNORM/WRNORM/RDHUP */
            pf[np].events = (short)(ep->it[i].events & 0x23ff);
            pf[np].revents = 0;
            map[np++] = i;
        }
        long r = 0;
        if (np) r = sys_poll_kernel(pf, (unsigned)np, 0);
        if (r < 0) return r;
        long out = 0;
        bool suppressed = false;
        for (int j = 0; j < np; j++) {
            lx_epitem_t *it = &ep->it[map[j]];
            /* ERR/HUP are always reported, like poll */
            uint32_t rev = (uint16_t)pf[j].revents & ((it->events & 0x23ff) | 0x18);
            uint32_t gi = 0, go = 0;
            if (it->events & LX_EPOLLET) {
                lx_ep_gens(p, it, &gi, &go);
                /* bits that went un-ready since the last report re-arm */
                it->et_rev &= rev;
                if (!rev) { it->et_in = gi; it->et_out = go; continue; }
                bool edge = (rev & ~it->et_rev) != 0 ||
                            ((rev & 0x41) && gi != it->et_in) ||      /* IN|RDNORM: new data */
                            ((rev & 0x104) && go != it->et_out);      /* OUT|WRNORM: new room */
                if (!edge) { suppressed = true; continue; }
            }
            if (!rev || out >= maxevents) continue;
            lx_epoll_event_t e = { rev, it->data };
            if (lx_put((char *)uevs + out * sizeof(e), &e, sizeof(e))) return out ? out : -EFAULT;
            if (it->events & LX_EPOLLONESHOT) it->armed = false;
            if (it->events & LX_EPOLLET) { it->et_rev = rev; it->et_in = gi; it->et_out = go; }
            out++;
        }
        if (out) return out;
        if (timeout_ms == 0) return 0;
        if (timeout_ms > 0 && ktime_expired(dl)) return 0;
        /* sleep until something not yet reported may be ready: wait on the
         * non-suppressed entries in short slices (suppressed ET entries are
         * re-checked each slice) */
        int slice = 10;
        if (!suppressed) slice = timeout_ms < 0 ? 50 : timeout_ms;
        int k = 0;
        for (int j = 0; j < np; j++) {
            lx_epitem_t *it = &ep->it[map[j]];
            if ((it->events & LX_EPOLLET) && it->et_rev && pf[j].revents) continue;
            pf[k].fd = pf[j].fd; pf[k].events = (short)(it->events & 0x23ff); pf[k].revents = 0; k++;
        }
        if (!k) { pf[0].fd = -1; pf[0].events = 0; pf[0].revents = 0; k = 1; }
        if (timeout_ms > 0) {
            extern uint64_t ktime_now_ms(void);
            uint64_t now = ktime_now_ms();
            int left = dl > now ? (int)(dl - now) : 0;
            if (slice > left) slice = left;
        }
        r = sys_poll_kernel(pf, (unsigned)k, slice);
        if (r < 0) return r;
        if (proc_signal_interrupts(p)) return -EINTR;
    }
}

#define FD_TYPE_TIMERFD  14
#define FD_TYPE_SIGNALFD 15
static long lx_timerfd_read(process_t *p, long fd, void *ubuf, size_t len);
static long lx_signalfd_read(process_t *p, long fd, void *ubuf, size_t len);
static long lx_inotify_read(process_t *p, long fd, void *ubuf, size_t len);
static void lx_futex_wake_addr(uint64_t uaddr, int n);
static long lx_eventfd_rw(process_t *p, long fd, void *ubuf, size_t len, bool wr) {
    if (len < 8) return -EINVAL;
    volatile uint64_t *ctr = (volatile uint64_t *)p->fds[fd].file_handle;
    uint64_t v;
    if (lx_strace)
        serial_printf("lxt:  eventfd %s pid %u fd %ld ctr@%p=%llu\n", wr ? "write" : "read",
                      p->pid, fd, (void *)ctr, (unsigned long long)*ctr);
    if (wr) {
        if (lx_get(&v, ubuf, 8)) return -EFAULT;
        if (v == ~0ULL) return -EINVAL;
        *ctr += v;
        ctr[1] = (ctr[1] & ~0xffffffffULL) | (uint32_t)(ctr[1] + 1);   /* in edge */
        return 8;
    }
    for (;;) {
        v = *ctr;
        if (v) { *ctr = 0; ctr[1] += 1ULL << 32;   /* out edge */
                 return lx_put(ubuf, &v, 8) ? -EFAULT : 8; }
        if (p->fds[fd].flags & O_NONBLOCK) return -EAGAIN;
        if (proc_kill_pending(p)) return -EINTR;
        thread_sleep(1);
    }
}

static long lx_read(process_t *p, long fd, void *ubuf, size_t count) {
    if (!lx_fd_ok(p, fd)) return -EBADF;
    uint8_t ty = p->fds[fd].fd_type;
    if (ty == FD_TYPE_DEV_NULL)
        return p->fds[fd].file_handle ? lx_read_nulltag(p->fds[fd].file_handle, ubuf, count) : 0;
    if (ty == FD_TYPE_DEV_FB)
        return fbdev_read(p, &p->fds[fd].position, ubuf, count);
    if (ty == FD_TYPE_DEV_INPUT)
        return evdev_read(p->fds[fd].file_handle, ubuf, count, (p->fds[fd].flags & O_NONBLOCK) != 0);
    if (ty == FD_TYPE_DEV_UINPUT)
        return uinput_read(p->fds[fd].file_handle, ubuf, count, (p->fds[fd].flags & O_NONBLOCK) != 0);
    if (ty == FD_TYPE_DEV_WM)
        return wm_dev_read(p->fds[fd].file_handle, ubuf, count, (p->fds[fd].flags & O_NONBLOCK) != 0);
    if (ty == FD_TYPE_PIPE_READ && lx_fd_pty(p, fd)) {
        /* pty read: the window does the line discipline; honour
         * non-canonical VMIN=0 (poll with VTIME*100ms timeout). */
        kpty_t *pt = lx_fd_pty(p, fd);
        pipe_t *pp = (pipe_t *)p->fds[fd].file_handle;
        if (!(pt->lflag & KPTY_ICANON) && pt->cc[KPTY_VMIN] == 0) {
            extern uint64_t ktime_now_ms(void);
            extern void thread_yield(void);
            uint64_t end = ktime_now_ms() + (uint64_t)pt->cc[KPTY_VTIME] * 100u;
            while (pp->count == 0 && pp->writers > 0) {
                if (ktime_now_ms() >= end) return 0;
                thread_yield();
            }
            if (pp->count == 0 && pp->writers > 0) return 0;
        }
        return (long)sys_read((int)fd, ubuf, count);
    }
    if (ty == FD_TYPE_DEV_TTY || lx_fd_is_tty(p, fd))
        return lx_read_tty(p, fd, ubuf, count);
    if (ty == FD_TYPE_SOCKET)
        return lx_sock_recv((int)fd, ubuf, count, 0);
    if (ty == FD_TYPE_EPOLL) return -EINVAL;
    if (ty == FD_TYPE_INOTIFY) return lx_inotify_read(p, fd, ubuf, count);
    if (ty == FD_TYPE_TIMERFD) return lx_timerfd_read(p, fd, ubuf, count);
    if (ty == FD_TYPE_SIGNALFD) return lx_signalfd_read(p, fd, ubuf, count);
    if (ty == FD_TYPE_EVENTFD)
        return lx_eventfd_rw(p, fd, ubuf, count, false);
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
static long lx_resolve_kf(process_t *p, long dirfd, const char *kpath,
                          char *out, size_t outsz, bool follow);
/* lx_resolve_f: @follow = expand a symlink in the last component too
 * (stat/open/chdir...); false for lstat/unlink/rename/readlink/...
 * Symlinks in earlier components are always expanded (vfs_resolve). */
static long lx_resolve_f(process_t *p, long dirfd, const char *upath,
                         char *out, size_t outsz, bool allow_empty, bool follow)
{
    char kpath[256];
    if (!upath || !strncpy_from_user(kpath, upath, sizeof(kpath)))
        return -EFAULT;
    if (kpath[0] == '\0' && !allow_empty)
        return -ENOENT;
    return lx_resolve_kf(p, dirfd, kpath, out, outsz, follow);
}
static long lx_resolve(process_t *p, long dirfd, const char *upath,
                       char *out, size_t outsz, bool allow_empty)
{
    return lx_resolve_f(p, dirfd, upath, out, outsz, allow_empty, true);
}
static long lx_resolve_k(process_t *p, long dirfd, const char *kpath,
                         char *out, size_t outsz)
{
    return lx_resolve_kf(p, dirfd, kpath, out, outsz, true);
}

/* Same as lx_resolve_f, for a path already in kernel memory. */
static long lx_resolve_kf(process_t *p, long dirfd, const char *kpath,
                          char *out, size_t outsz, bool follow)
{

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

    /* Normalise into out, expanding symlinks (tmpfs, ext4). */
    {
        int vr = vfs_resolve(tmp, out, outsz, follow);
        if (vr != -ENOMEM) return vr;
    }
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

/* AF_UNIX address -> registry name: abstract "\0name" (length-exact) or
 * an absolute normalised path. *outlen 0 = unnamed. */
long lx_unix_parse_addr(process_t *p, const void *uaddr, uint32_t alen,
                        char *out, int *outlen)
{
    *outlen = 0;
    if (!uaddr) return -EFAULT;
    if (alen < 2 || alen > 110) return -EINVAL;
    uint8_t raw[112];
    if (copy_from_user(raw, uaddr, alen)) return -EFAULT;
    if ((raw[0] | (raw[1] << 8)) != 1 /* AF_UNIX */) return -EINVAL;
    if (alen == 2) return 0;
    if (raw[2] == 0) {
        memcpy(out, raw + 2, alen - 2);
        *outlen = (int)(alen - 2);
        return 0;
    }
    char kp[112];
    uint32_t n = 0;
    while (n < alen - 2 && raw[2 + n]) { kp[n] = (char)raw[2 + n]; n++; }
    kp[n] = 0;
    char abs[256];
    long r = lx_resolve_k(p, LX_AT_FDCWD, kp, abs, sizeof(abs));
    if (r < 0) return r;
    size_t l = strlen(abs);
    if (l >= 109) return -ENAMETOOLONG;
    memcpy(out, abs, l + 1);
    *outlen = (int)l;
    return 0;
}

typedef struct { uint64_t base, len; } lx_ux_iov_t;
long ux_socket(process_t *p, long type);
long ux_socketpair(process_t *p, long type, int *usv);
long ux_bind(process_t *p, kernel_socket_t *s, const void *uaddr, uint32_t alen);
long ux_listen(kernel_socket_t *s, int backlog);
long ux_connect(process_t *p, int fd, kernel_socket_t *s, const void *uaddr, uint32_t alen);
long ux_accept(process_t *p, int fd, kernel_socket_t *s, void *uaddr, uint32_t *ualen, long flags);
long ux_sendmsg(process_t *p, int fd, kernel_socket_t *s, const lx_ux_iov_t *iov, int niov,
                const void *uname, uint32_t namelen, const void *uctl, uint64_t ctllen, int flags);
long ux_recvmsg(process_t *p, int fd, kernel_socket_t *s, const lx_ux_iov_t *iov, int niov,
                void *uname, uint32_t *unamelen, void *uctl, uint64_t ctllen,
                uint64_t *octl, int *oflags, int flags);

static kernel_socket_t *lx_unix_sock(process_t *p, long fd) {
    if (fd < 0 || fd >= MAX_OPEN_FILES_SYSCALL || !p->fds[fd].in_use ||
        p->fds[fd].fd_type != FD_TYPE_SOCKET) return NULL;
    kernel_socket_t *s = (kernel_socket_t *)p->fds[fd].file_handle;
    return (s && s->domain == 1) ? s : NULL;
}

extern long flk_op(process_t *p, void *owner, const char *path, int op);
extern int  fifo_reg(const char *path);
extern bool fifo_is(const char *path);
extern void fifo_unreg(const char *path);
extern void fifo_rename(const char *from, const char *to);
extern long fifo_open(process_t *p, const char *path, int acc, bool nonblock, bool cloexec);
extern int  lx_nice_get(uint32_t pid);
extern void lx_nice_set(uint32_t pid, int v);

static int lx_path_kind(const char *abspath, stat_t *st_out) {
    stat_t st;
    if (syscall_stat_abspath(abspath, &st) != 0)
        return 0;                               /* missing */
    if (st_out) *st_out = st;
    return ((st.st_mode & S_IFMT) == S_IFDIR) ? 2 : 1;
}

/*
 * inDevOS programs live as /bin/<name>.exe. Shells (busybox ash) and make
 * search PATH with stat()/access() before execve(), so "tcc" would never be
 * found. For metadata lookups only, a missing extensionless path that has a
 * regular-file "<path>.exe" sibling resolves to it (execve already does this).
 */
extern const char *syscall_busybox_applet_path(const char *path);
static void lx_exe_alias2(char *path, size_t sz, bool bb_ok) {
    if (lx_path_kind(path, NULL)) return;
    size_t n = strlen(path);
    const char *slash = path;
    for (size_t i = 0; i < n; i++) if (path[i] == '/') slash = path + i + 1;
    for (const char *c = slash; *c; c++) if (*c == '.') return;
    if (!*slash || n + 5 > sz) return;
    memcpy(path + n, ".exe", 5);
    if (lx_path_kind(path, NULL) == 1) return;
    path[n] = '\0';
    /* missing BusyBox applet path (/bin/sed, /usr/bin/awk): execve runs it */
    const char *bb = bb_ok ? syscall_busybox_applet_path(path) : NULL;
    if (bb && strlen(bb) < sz) kstrncpy(path, bb, sz);
}
static void lx_exe_alias(char *path, size_t sz) { lx_exe_alias2(path, sz, true); }

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
    /* Filesystems with real inode times (ext2/3/4) report epoch seconds;
     * the rest fill boot-relative values, for which "now" is reported. */
    if (st->st_mtime > 100000000) {
        ls->st_atime_sec = st->st_atime;
        ls->st_mtime_sec = st->st_mtime;
        ls->st_ctime_sec = st->st_ctime;
    } else {
        ls->st_atime_sec = ls->st_mtime_sec = ls->st_ctime_sec = now;
    }
}

/* Synthetic character devices: stat/access by path (fbdev apps, isatty
 * helpers).  Returns st_rdev = (major << 8) | minor, 0 if not a device. */
static uint32_t lx_dev_path(const char *path, uint32_t *mode) {
    static const struct { const char *p; uint32_t rdev, mode; } devs[] = {
        { "/dev/fb0", (29u << 8) | 0, 0660 }, { "/dev/fb/0", (29u << 8) | 0, 0660 },
        { "/dev/tty0", (4u << 8) | 0, 0620 }, { "/dev/tty1", (4u << 8) | 1, 0620 },
        { "/dev/tty2", (4u << 8) | 2, 0620 }, { "/dev/tty3", (4u << 8) | 3, 0620 },
        { "/dev/tty4", (4u << 8) | 4, 0620 }, { "/dev/tty", (5u << 8) | 0, 0666 },
        { "/dev/console", (5u << 8) | 1, 0600 }, { "/dev/null", (1u << 8) | 3, 0666 },
        { "/dev/zero", (1u << 8) | 5, 0666 }, { "/dev/random", (1u << 8) | 8, 0666 },
        { "/dev/urandom", (1u << 8) | 9, 0666 },
    };
    for (unsigned i = 0; i < sizeof(devs) / sizeof(devs[0]); i++)
        if (kstrcmp(path, devs[i].p) == 0) { if (mode) *mode = devs[i].mode; return devs[i].rdev; }
    {
        int minor = evdev_parse_path(path);
        if (minor >= 0) { if (mode) *mode = 0660; return (13u << 8) | (uint32_t)(64 + minor); }
    }
    if (kstrcmp(path, "/dev/uinput") == 0) { if (mode) *mode = 0660; return (10u << 8) | 223u; }
    if (kstrcmp(path, "/dev/wm") == 0) { if (mode) *mode = 0666; return (10u << 8) | 62u; }
    return 0;
}

/* /dev/input (and /dev when the root fs has none) are synthetic dirs. */
static bool lx_synth_dir(const char *path) {
    char nm[1][16]; uint8_t isd[1];
    return input_dev_dir_list(path, nm, isd, 0) >= 0;
}

/* ---- permission bits on mode-less filesystems (FAT32, ISO) ----
 * chmod() overrides live in a small in-memory table (per boot).  Without an
 * override, regular files under /bin, /usr/bin, /sbin and files starting
 * with an ELF header or "#!" report 0755 instead of the fs default 0644. */
#define LX_MODE_OVR_MAX 256
static struct { char path[160]; uint16_t mode; bool used; } lx_mode_ovr[LX_MODE_OVR_MAX];

static int lx_mode_ovr_find(const char *abspath) {
    for (int i = 0; i < LX_MODE_OVR_MAX; i++)
        if (lx_mode_ovr[i].used && kstrcmp(lx_mode_ovr[i].path, abspath) == 0)
            return i;
    return -1;
}

static long lx_mode_set(const char *abspath, uint32_t mode) {
    if (strlen(abspath) >= sizeof(lx_mode_ovr[0].path)) return 0;   /* accepted, not stored */
    int i = lx_mode_ovr_find(abspath);
    if (i < 0)
        for (i = 0; i < LX_MODE_OVR_MAX && lx_mode_ovr[i].used; i++) ;
    if (i >= LX_MODE_OVR_MAX) return 0;
    kstrncpy(lx_mode_ovr[i].path, abspath, sizeof(lx_mode_ovr[i].path));
    lx_mode_ovr[i].mode = (uint16_t)(mode & 07777);
    lx_mode_ovr[i].used = true;
    return 0;
}

static bool lx_file_looks_exec(const char *abspath) {
    if (!kstrncmp(abspath, "/bin/", 5) || !kstrncmp(abspath, "/usr/bin/", 9) ||
        !kstrncmp(abspath, "/sbin/", 6) || !kstrncmp(abspath, "/usr/local/bin/", 15))
        return true;
    char h[4];
    int fd = vfs_open(abspath, VFS_O_READ);
    if (fd < 0) return false;
    int n = vfs_read(fd, h, 4);
    vfs_close(fd);
    if (n >= 4 && h[0] == 0x7f && h[1] == 'E' && h[2] == 'L' && h[3] == 'F') return true;
    return n >= 2 && h[0] == '#' && h[1] == '!';
}

static void lx_mode_adjust(const char *abspath, stat_t *st) {
    if ((st->st_mode & 0170000) != 0100000) return;    /* regular files only */
    if (vfs_has_inode_perms(abspath)) return;           /* tmpfs/ext4: real mode bits */
    int i = lx_mode_ovr_find(abspath);
    if (i >= 0) {
        st->st_mode = (st->st_mode & ~07777u) | lx_mode_ovr[i].mode;
        return;
    }
    if (!(st->st_mode & 0111) && lx_file_looks_exec(abspath))
        st->st_mode |= 0111;
}

/* access()/faccessat() with real X_OK (R/W stay permissive: single user). */
static long lx_access_path(const char *abspath, long amode) {
    stat_t st;
    memset(&st, 0, sizeof(st));
    if (!lx_path_kind(abspath, &st)) return -ENOENT;
    if (amode & 1) {                                    /* X_OK */
        lx_mode_adjust(abspath, &st);
        if ((st.st_mode & 0170000) != 0040000 && !(st.st_mode & 0111))
            return -EACCES;
    }
    return 0;
}

static long lx_stat_path_k(const char *abspath, lx_stat_t *out) {
    stat_t st;
    {
        uint32_t dmode = 0, rdev = lx_dev_path(abspath, &dmode);
        if (rdev) {
            memset(&st, 0, sizeof(st));
            st.st_mode = S_IFCHR | dmode;
            st.st_nlink = 1;
            st.st_blksize = 4096;
            st.st_rdev = rdev;
            st.st_ino = 0x10000 + rdev;
            lx_fill_stat(out, &st);
            return 0;
        }
    }
    if (lx_proc_text_path(get_current_process(), abspath)) {
        memset(&st, 0, sizeof(st));             /* /proc/cpuinfo etc: size 0 like Linux */
        st.st_mode = 0100000 | 0444;
        st.st_nlink = 1;
        st.st_blksize = 1024;
        st.st_ino = 0xF000 + (uint64_t)strlen(abspath);
    } else if (lx_path_kind(abspath, &st)) {
        lx_mode_adjust(abspath, &st);
        if (fifo_is(abspath)) { st.st_mode = LX_S_IFIFO | 0644; st.st_size = 0; }
    } else {
        if (!lx_synth_dir(abspath))
            return -ENOENT;
        memset(&st, 0, sizeof(st));             /* /dev, /dev/input */
        st.st_mode = S_IFDIR | 0755;
        st.st_nlink = 2;
        st.st_blksize = 4096;
        st.st_ino = kstrcmp(abspath, "/dev") == 0 ? 0xD000 : 0xD001;
    }
    lx_fill_stat(out, &st);
    return 0;
}

static long lx_stat_path(const char *abspath, void *ubuf) {
    lx_stat_t ls;
    long r = lx_stat_path_k(abspath, &ls);
    if (r) return r;
    return lx_put(ubuf, &ls, sizeof(ls));
}

/* Path of an fd for fchmod/fchown: 0 ok, 1 = not a file (accept), -EBADF. */
static long lx_fd_path(process_t *p, long fd, char *out, size_t outsz) {
    if (!lx_fd_ok(p, fd)) return -EBADF;
    file_descriptor_t *f = &p->fds[fd];
    if (f->fd_type == FD_TYPE_VFS_DIR && f->file_handle) {
        kstrncpy(out, (const char *)f->file_handle, outsz);
        return 0;
    }
    if (f->fd_type == FD_TYPE_VFS && vfs_fd_get_path(f->vfs_fd, out, outsz) >= 0)
        return 0;
    return 1;
}

/* utime*(): store on filesystems with a times hook (FAT32); the others
 * accept and ignore.  Only a read-only medium is an error. */
static long lx_store_times(const char *path, bool sa, int64_t at, bool sm, int64_t mt) {
    if (!sa && !sm) return 0;
    int r = vfs_times(path, 1, sm ? &mt : NULL, sa ? &at : NULL);
    return r == -EROFS ? -EROFS : 0;
}

static long lx_fstat(process_t *p, long fd, void *ubuf) {
    if (!lx_fd_ok(p, fd))
        return -EBADF;
    file_descriptor_t *f = &p->fds[fd];
    stat_t st;
    memset(&st, 0, sizeof(st));
    if (f->fd_type == FD_TYPE_DEV_FB) {
        st.st_mode = 0660 | S_IFCHR;
        st.st_nlink = 1;
        st.st_blksize = 4096;
        st.st_rdev = (29u << 8) | 0;            /* fb0 = 29,0 */
        lx_stat_t ls;
        lx_fill_stat(&ls, &st);
        return lx_put(ubuf, &ls, sizeof(ls));
    }
    if (f->fd_type == FD_TYPE_DEV_INPUT) {
        st.st_mode = 0660 | S_IFCHR;
        st.st_nlink = 1;
        st.st_blksize = 4096;
        st.st_rdev = (13u << 8) | (uint32_t)(64 + evdev_minor(f->file_handle));   /* input: 13, 64+N */
        st.st_ino = 0x10000 + st.st_rdev;
        lx_stat_t ls;
        lx_fill_stat(&ls, &st);
        return lx_put(ubuf, &ls, sizeof(ls));
    }
    if (f->fd_type == FD_TYPE_DEV_WM) {
        st.st_mode = 0666 | S_IFCHR;
        st.st_nlink = 1;
        st.st_blksize = 4096;
        st.st_rdev = (10u << 8) | 62u;          /* misc 10,62 (inDevOS wm) */
        st.st_ino = 0x10000 + st.st_rdev;
        lx_stat_t ls;
        lx_fill_stat(&ls, &st);
        return lx_put(ubuf, &ls, sizeof(ls));
    }
    if (f->fd_type == FD_TYPE_DEV_UINPUT) {
        st.st_mode = 0660 | S_IFCHR;
        st.st_nlink = 1;
        st.st_blksize = 4096;
        st.st_rdev = (10u << 8) | 223u;         /* misc 10,223 */
        st.st_ino = 0x10000 + st.st_rdev;
        lx_stat_t ls;
        lx_fill_stat(&ls, &st);
        return lx_put(ubuf, &ls, sizeof(ls));
    }
    if (f->fd_type == FD_TYPE_DEV_NULL || f->fd_type == FD_TYPE_DEV_TTY) {
        st.st_mode = (f->fd_type == FD_TYPE_DEV_NULL ? 0666 : 0622) | S_IFCHR;
        st.st_nlink = 1;
        st.st_blksize = 4096;
        st.st_rdev = f->fd_type == FD_TYPE_DEV_NULL ? 0x103 : 0x500; /* 1,3 / 5,0 */
        if (f->fd_type == FD_TYPE_DEV_NULL && f->file_handle)
            st.st_rdev = f->file_handle == LX_NULLTAG_ZERO ? 0x105 : 0x109;   /* 1,5 / 1,9 */
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
    } else if (lx_fd_pty(p, fd)) {
        st.st_mode = S_IFCHR | 0620;            /* /dev/pts/0 */
        st.st_rdev = (136u << 8) | 0;
        st.st_blksize = 1024;
        st.st_ino = 2000;
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
    if (p->fds[fd].fd_type == FD_TYPE_DEV_FB)
        return fbdev_write(p, &p->fds[fd].position, ubuf, len);
    if (p->fds[fd].fd_type == FD_TYPE_DEV_INPUT)
        return evdev_write(p->fds[fd].file_handle, ubuf, len);
    if (p->fds[fd].fd_type == FD_TYPE_DEV_UINPUT)
        return uinput_write(p->fds[fd].file_handle, ubuf, len);
    if (p->fds[fd].fd_type == FD_TYPE_DEV_WM)
        return -EINVAL;
    if (p->fds[fd].fd_type == FD_TYPE_SOCKET)
        return lx_sock_send((int)fd, ubuf, len, 0);
    if (p->fds[fd].fd_type == FD_TYPE_EPOLL || p->fds[fd].fd_type == FD_TYPE_TIMERFD ||
        p->fds[fd].fd_type == FD_TYPE_SIGNALFD || p->fds[fd].fd_type == FD_TYPE_INOTIFY) return -EINVAL;
    if (p->fds[fd].fd_type == FD_TYPE_EVENTFD)
        return lx_eventfd_rw(p, fd, (void *)ubuf, len, true);
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

/* ---- /dev/zero, /dev/urandom: FD_TYPE_DEV_NULL with a tag in file_handle
 * (close never frees DEV_NULL handles). ---- */

static long lx_read_nulltag(void *tag, void *ubuf, size_t count) {
    uint8_t buf[256];
    size_t done = 0;
    if (count > (1u << 20)) count = 1u << 20;
    while (done < count) {
        size_t c = count - done;
        if (c > sizeof(buf)) c = sizeof(buf);
        if (tag == LX_NULLTAG_ZERO) memset(buf, 0, c);
        else for (size_t i = 0; i < c; i += 8) { uint64_t v = lx_random_u64(); memcpy(buf + i, &v, c - i < 8 ? c - i : 8); }
        if (lx_put((uint8_t *)ubuf + done, buf, c)) return done ? (long)done : -EFAULT;
        done += c;
    }
    return (long)done;
}

/* ---- minimal /proc text files: cpuinfo, meminfo, self/stat, <pid>/stat.
 * Content is generated at open() into a pipe whose write end is closed, so
 * read() returns the text then EOF. ---- */
static process_t *lx_proc_pid_stat(process_t *p, const char *path) {
    if (kstrcmp(path, "/proc/self/stat") == 0 || kstrcmp(path, "/proc/thread-self/stat") == 0) return p;
    if (kstrncmp(path, "/proc/", 6) != 0) return NULL;
    const char *s = path + 6;
    uint32_t pid = 0;
    if (*s < '0' || *s > '9') return NULL;
    while (*s >= '0' && *s <= '9') pid = pid * 10 + (uint32_t)(*s++ - '0');
    if (kstrcmp(s, "/stat") != 0) return NULL;
    /* self and ancestors (process table is private to syscall.c) */
    int depth = 0;
    for (process_t *q = p; q && depth < 64; q = q->parent, depth++)
        if (q->pid == pid) return q;
    return NULL;
}

static bool lx_proc_text_path(process_t *p, const char *path) {
    return kstrcmp(path, "/proc/cpuinfo") == 0 || kstrcmp(path, "/proc/meminfo") == 0 ||
           lx_proc_pid_stat(p, path) != NULL;
}

static int lx_proc_gen(process_t *p, const char *path, char *out, int cap) {
    if (kstrcmp(path, "/proc/meminfo") == 0) {
        extern uint64_t total_memory_bytes;
        uint64_t tot = total_memory_bytes >> 10, fr = (num_pages_available * (uint64_t)PAGE_SIZE) >> 10;
        return snprintf(out, (size_t)cap,
            "MemTotal:       %8lu kB\nMemFree:        %8lu kB\nMemAvailable:   %8lu kB\n"
            "Buffers:               0 kB\nCached:                0 kB\nSwapCached:            0 kB\n"
            "SwapTotal:             0 kB\nSwapFree:              0 kB\nShmem:                 0 kB\n",
            (unsigned long)tot, (unsigned long)fr, (unsigned long)fr);
    }
    if (kstrcmp(path, "/proc/cpuinfo") == 0) {
        extern uint64_t smp_get_online_count(void);
        char brand[49];
        uint32_t r[12];
        uint32_t a, b, c, d;
        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000000u), "c"(0));
        if (a >= 0x80000004u) {
            for (uint32_t i = 0; i < 3; i++)
                __asm__ volatile("cpuid" : "=a"(r[i*4]), "=b"(r[i*4+1]), "=c"(r[i*4+2]), "=d"(r[i*4+3])
                                 : "a"(0x80000002u + i), "c"(0));
            memcpy(brand, r, 48);
            brand[48] = 0;
        } else {
            kstrncpy(brand, "x86_64 CPU", sizeof(brand));
        }
        char *bp = brand; while (*bp == ' ') bp++;
        char vendor[13];
        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0), "c"(0));
        memcpy(vendor, &b, 4); memcpy(vendor + 4, &d, 4); memcpy(vendor + 8, &c, 4); vendor[12] = 0;
        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
        uint32_t fam = (a >> 8) & 0xf, model = (a >> 4) & 0xf, step = a & 0xf;
        if (fam == 0xf) fam += (a >> 20) & 0xff;
        if (fam == 0x6 || fam >= 0xf) model |= ((a >> 16) & 0xf) << 4;
        int n = (int)smp_get_online_count();
        if (n < 1) n = 1;
        int len = 0;
        for (int i = 0; i < n && len < cap - 400; i++)
            len += snprintf(out + len, (size_t)(cap - len),
                "processor\t: %d\nvendor_id\t: %s\ncpu family\t: %u\nmodel\t\t: %u\n"
                "model name\t: %s\nstepping\t: %u\ncpu MHz\t\t: 0.000\nphysical id\t: 0\n"
                "siblings\t: %d\ncore id\t\t: %d\ncpu cores\t: %d\nflags\t\t: fpu tsc msr pae cx8 apic sse sse2%s\n\n",
                i, vendor, fam, model, bp, step, n, i, n, (c & (1u << 0)) ? " pni" : "");
        return len;
    }
    process_t *t = lx_proc_pid_stat(p, path);
    if (t) {
        const char *exe = t->linux_exe_path[0] ? t->linux_exe_path : "init";
        const char *bn = exe;
        for (const char *q = exe; *q; q++) if (*q == '/') bn = q + 1;
        char comm[16]; kstrncpy(comm, bn, sizeof(comm));
        uint32_t ppid = t->parent ? t->parent->pid : 0;
        char st = t->is_zombie ? 'Z' : (t == p ? 'R' : 'S');
        /* 52 fields; times/sizes unknown -> 0 */
        return snprintf(out, (size_t)cap,
            "%u (%s) %c %u %u %u 0 -1 4194304 0 0 0 0 0 0 0 0 20 0 1 0 0 0 0 "
            "18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n",
            t->pid, comm, st, ppid, t->pgid ? t->pgid : t->pid, t->sid ? t->sid : t->pid);
    }
    return -1;
}

static long lx_open_proc_text(process_t *p, const char *path, long lflags) {
    if (!lx_proc_text_path(p, path)) return -ENOENT;
    if (lflags & LX_O_ACCMODE) return -EACCES;
    char *buf = (char *)kalloc(PIPE_BUF_SIZE);
    if (!buf) return -ENOMEM;
    int n = lx_proc_gen(p, path, buf, PIPE_BUF_SIZE);
    if (n < 0) { kfree(buf); return -ENOENT; }
    if (n > PIPE_BUF_SIZE) n = PIPE_BUF_SIZE;
    pipe_t *pp = pipe_create();
    if (!pp) { kfree(buf); return -ENFILE; }
    int w = pipe_write(pp, buf, (size_t)n);
    kfree(buf);
    (void)w;
    int fd = allocate_fd(p);
    if (fd < 0) { pipe_close_write(pp); pipe_close_read(pp); return -EMFILE; }
    file_descriptor_t *f = &p->fds[fd];
    memset(f, 0, sizeof(*f));
    f->ref_count = fd_refcount_create();
    if (!f->ref_count) { pipe_close_write(pp); pipe_close_read(pp); return -ENOMEM; }
    f->in_use = true;
    f->fd_type = FD_TYPE_PIPE_READ;
    f->file_handle = pp;
    f->vfs_fd = -1;
    f->cloexec = (lflags & LX_O_CLOEXEC) != 0;
    pipe_close_write(pp);          /* reader sees the text, then EOF */
    return fd;
}

static long lx_open_special(process_t *p, const char *path, long lflags) {
    /* Synthetic devices used by musl/busybox (isatty, ash, redirects). */
    if (kstrcmp(path, "/dev/null") == 0 || kstrcmp(path, "/dev/zero") == 0 ||
        kstrcmp(path, "/dev/urandom") == 0 || kstrcmp(path, "/dev/random") == 0) {
        int fd = allocate_fd(p);
        if (fd < 0) return -EMFILE;
        p->fds[fd].fd_type = FD_TYPE_DEV_NULL;
        p->fds[fd].file_handle = path[5] == 'z' ? LX_NULLTAG_ZERO
                               : (path[5] == 'u' || path[5] == 'r') ? LX_NULLTAG_RANDOM : NULL;
        p->fds[fd].flags = (uint32_t)(lflags & 3);
        p->fds[fd].position = 0;
        p->fds[fd].cloexec = (lflags & LX_O_CLOEXEC) != 0;
        p->fds[fd].in_use = true;
        return fd;
    }
    if (kstrcmp(path, "/dev/fb0") == 0 || kstrcmp(path, "/dev/fb/0") == 0) {
        int r = fbdev_open(p);
        if (r < 0) return r;
        int fd = allocate_fd(p);
        if (fd < 0) { fbdev_close(p); return -EMFILE; }
        p->fds[fd].fd_type = FD_TYPE_DEV_FB;
        p->fds[fd].file_handle = NULL;
        p->fds[fd].flags = (uint32_t)(lflags & 3);
        p->fds[fd].position = 0;
        p->fds[fd].cloexec = (lflags & LX_O_CLOEXEC) != 0;
        p->fds[fd].in_use = true;
        return fd;
    }
    {
        int minor = evdev_parse_path(path);
        if (minor >= 0) {
            void *h = NULL;
            long r = evdev_open(minor, &h);
            if (r < 0) return r;
            int fd = allocate_fd(p);
            if (fd < 0) { evdev_release(h); return -EMFILE; }
            p->fds[fd].ref_count = fd_refcount_create();   /* fork/dup share one client */
            if (!p->fds[fd].ref_count) { evdev_release(h); p->fds[fd].in_use = false; return -ENOMEM; }
            p->fds[fd].fd_type = FD_TYPE_DEV_INPUT;
            p->fds[fd].file_handle = h;
            p->fds[fd].flags = (uint32_t)(lflags & 3) | ((lflags & LX_O_NONBLOCK) ? O_NONBLOCK : 0);
            p->fds[fd].position = 0;
            p->fds[fd].cloexec = (lflags & LX_O_CLOEXEC) != 0;
            p->fds[fd].in_use = true;
            return fd;
        }
    }
    if (kstrcmp(path, "/dev/wm") == 0) {
        void *h = NULL;
        long r = wm_dev_open(&h);
        if (r < 0) return r;
        int fd = allocate_fd(p);
        if (fd < 0) { wm_dev_release(h); return -EMFILE; }
        p->fds[fd].ref_count = fd_refcount_create();
        if (!p->fds[fd].ref_count) { wm_dev_release(h); p->fds[fd].in_use = false; return -ENOMEM; }
        p->fds[fd].fd_type = FD_TYPE_DEV_WM;
        p->fds[fd].file_handle = h;
        p->fds[fd].flags = (uint32_t)(lflags & 3) | ((lflags & LX_O_NONBLOCK) ? O_NONBLOCK : 0);
        p->fds[fd].position = 0;
        p->fds[fd].cloexec = (lflags & LX_O_CLOEXEC) != 0;
        p->fds[fd].in_use = true;
        return fd;
    }
    if (kstrcmp(path, "/dev/uinput") == 0 || kstrcmp(path, "/dev/input/uinput") == 0) {
        void *h = NULL;
        long r = uinput_open(&h);
        if (r < 0) return r;
        int fd = allocate_fd(p);
        if (fd < 0) { uinput_release(h); return -EMFILE; }
        p->fds[fd].ref_count = fd_refcount_create();
        if (!p->fds[fd].ref_count) { uinput_release(h); p->fds[fd].in_use = false; return -ENOMEM; }
        p->fds[fd].fd_type = FD_TYPE_DEV_UINPUT;
        p->fds[fd].file_handle = h;
        p->fds[fd].flags = (uint32_t)(lflags & 3) | ((lflags & LX_O_NONBLOCK) ? O_NONBLOCK : 0);
        p->fds[fd].position = 0;
        p->fds[fd].cloexec = (lflags & LX_O_CLOEXEC) != 0;
        p->fds[fd].in_use = true;
        return fd;
    }
    if (lx_synth_dir(path) && lx_path_kind(path, NULL) != 2) {
        /* /dev/input (and /dev if the root fs lacks it): directory fd whose
         * entries come from input_dev_dir_list() in lx_getdents64 */
        size_t pl = strlen(path) + 1;
        char *copy = (char *)kalloc(pl);
        if (!copy) return -ENOMEM;
        memcpy(copy, path, pl);
        int fd = allocate_fd(p);
        if (fd < 0) { kfree(copy); return -EMFILE; }
        p->fds[fd].ref_count = fd_refcount_create();
        if (!p->fds[fd].ref_count) { kfree(copy); p->fds[fd].in_use = false; return -ENOMEM; }
        p->fds[fd].fd_type = FD_TYPE_VFS_DIR;
        p->fds[fd].file_handle = copy;
        p->fds[fd].vfs_fd = -1;
        p->fds[fd].flags = (uint32_t)(lflags & 3);
        p->fds[fd].position = 0;
        p->fds[fd].cloexec = (lflags & LX_O_CLOEXEC) != 0;
        p->fds[fd].in_use = true;
        return fd;
    }
    if (kstrcmp(path, "/dev/tty") == 0 || kstrcmp(path, "/dev/console") == 0 ||
        kstrcmp(path, "/dev/tty0") == 0 || kstrcmp(path, "/dev/tty1") == 0 ||
        kstrcmp(path, "/dev/tty2") == 0 || kstrcmp(path, "/dev/tty3") == 0 ||
        kstrcmp(path, "/dev/tty4") == 0) {
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
    if (kstrncmp(path, "/proc/", 6) == 0)
        return lx_open_proc_text(p, path, lflags);
    return -ENOENT; /* not special */
}

static long lx_openat(process_t *p, long dirfd, const char *upath, long lflags, long mode) {
    char path[256];
    /* O_NOFOLLOW / O_CREAT|O_EXCL: a symlink in the last component is
     * ELOOP / EEXIST instead of being followed. */
    bool nofol = (lflags & LX_O_NOFOLLOW) || ((lflags & LX_O_CREAT) && (lflags & LX_O_EXCL));
    long r = lx_resolve_f(p, dirfd, upath, path, sizeof(path), false, !nofol);
    if (r) return r;
    if (nofol) {
        char lb[4];
        if (vfs_readlink(path, lb, sizeof(lb)) >= 0)
            return (lflags & LX_O_CREAT) && (lflags & LX_O_EXCL) ? -EEXIST : -ELOOP;
    }

    r = lx_open_special(p, path, lflags);
    if (r != -ENOENT) return r;

    if (fifo_is(path)) {
        if ((lflags & LX_O_CREAT) && (lflags & LX_O_EXCL)) return -EEXIST;
        if (lflags & LX_O_DIRECTORY) return -ENOTDIR;
        return fifo_open(p, path, (int)(lflags & LX_O_ACCMODE), (lflags & LX_O_NONBLOCK) != 0,
                         (lflags & LX_O_CLOEXEC) != 0);
    }

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

/* /dev, /dev/input: append synthetic device nodes not on disk */
static int lx_dev_augment(const char *path, vfs_dirent_t *ents, int total, int cap) {
    enum { NSYN = 24 };
    char sn[NSYN][16]; uint8_t sd[NSYN];
    int ns = input_dev_dir_list(path, sn, sd, NSYN);
    if (ns < 0) return total;
    if (total < 0) total = 0;
    for (int i = 0; i < ns && total < cap; i++) {
        bool dup = false;
        for (int j = 0; j < total && !dup; j++) dup = strcmp(ents[j].name, sn[i]) == 0;
        if (dup) continue;
        memset(&ents[total], 0, sizeof(ents[total]));
        memcpy(ents[total].name, sn[i], strlen(sn[i]) + 1);
        ents[total].type = sd[i] ? VFS_DIRECTORY : 0;
        ents[total].inode = 0xD100 + (uint32_t)total;
        total++;
    }
    return total;
}

static long lx_getdents64(process_t *p, long fd, void *udirp, size_t count) {
    if (!lx_fd_ok(p, fd)) return -EBADF;
    file_descriptor_t *f = &p->fds[fd];
    if (f->fd_type != FD_TYPE_VFS_DIR || !f->file_handle) return -ENOTDIR;
    if (count > 65536) count = 65536;

    /* the smallest record is 24 bytes: never fetch more entries than fit */
    uint32_t want = (uint32_t)(count / 24) + 1;
    if (want > 256) want = 256;
    vfs_dirent_t *ents = (vfs_dirent_t *)kalloc(sizeof(vfs_dirent_t) * want);
    uint8_t *kbuf = (uint8_t *)kalloc(count ? count : 1);
    if (!ents || !kbuf) {
        if (ents) kfree(ents);
        if (kbuf) kfree(kbuf);
        return -ENOMEM;
    }
    uint32_t pos0 = f->position;
    int got = vfs_dircache_read(f->file_handle, (const char *)f->file_handle, pos0, ents, want, lx_dev_augment, 24);
    if (got < 0) { kfree(ents); kfree(kbuf); return got == -ENOMEM ? -ENOMEM : -EIO; }
    uint32_t total = pos0 + (uint32_t)got;

    uint32_t pos = pos0;
    size_t off = 0;
    while (pos < total) {
        vfs_dirent_t *e = &ents[pos - pos0];
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
        rec[18] = e->lnk ? 10 /* DT_LNK */ :
                  (e->type & VFS_DIRECTORY) ? LX_DT_DIR :
                  (e->type & VFS_FILE) ? LX_DT_REG :
                  (e->inode >= 0xD100 && e->inode < 0xD200) ? 2 /* DT_CHR */ : LX_DT_UNKNOWN;
        memcpy(rec + 19, e->name, nl + 1);
        off += reclen;
        pos++;
    }
    long r;
    if (off == 0 && pos < total) {
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
    if (f->fd_type == FD_TYPE_DEV_FB)
        return fbdev_lseek(&f->position, off, whence);
    if (f->fd_type == FD_TYPE_VFS_DIR) {         /* rewinddir / seekdir */
        if (whence == SEEK_SET && off >= 0) { f->position = (uint32_t)off; return off; }
        if (whence == SEEK_CUR && off == 0) return f->position;
        return -EINVAL;
    }
    if (f->fd_type != FD_TYPE_VFS) return -ESPIPE;
    if (whence < 0 || whence > 2) return -EINVAL;
    return sys_seek((int)fd, (off_t)off, (int)whence);
}

static uint16_t lx_con_ws_rows, lx_con_ws_cols;   /* console TIOCSWINSZ override */

static long lx_pty_ioctl(process_t *p, long fd, kpty_t *pt, unsigned long req, void *arg) {
    switch (req) {
        case LX_TCGETS: {
            lx_termios_t ti;
            memset(&ti, 0, sizeof(ti));
            ti.c_iflag = pt->iflag; ti.c_oflag = pt->oflag;
            ti.c_cflag = pt->cflag; ti.c_lflag = pt->lflag;
            memcpy(ti.c_cc, pt->cc, sizeof(ti.c_cc) < KPTY_NCC ? sizeof(ti.c_cc) : KPTY_NCC);
            return lx_put(arg, &ti, sizeof(ti));
        }
        case LX_TCSETS: case LX_TCSETSW: case LX_TCSETSF: {
            lx_termios_t ti;
            if (lx_get(&ti, arg, sizeof(ti))) return -EFAULT;
            pt->iflag = ti.c_iflag; pt->oflag = ti.c_oflag;
            pt->cflag = ti.c_cflag; pt->lflag = ti.c_lflag;
            memcpy(pt->cc, ti.c_cc, sizeof(ti.c_cc) < KPTY_NCC ? sizeof(ti.c_cc) : KPTY_NCC);
            return 0;
        }
        case LX_TIOCGWINSZ: {
            lx_winsize_t ws = { pt->rows ? pt->rows : 24, pt->cols ? pt->cols : 80, 0, 0 };
            return lx_put(arg, &ws, sizeof(ws));
        }
        case LX_TIOCSWINSZ: {
            lx_winsize_t ws;
            if (lx_get(&ws, arg, sizeof(ws))) return -EFAULT;
            pt->rows = ws.ws_row; pt->cols = ws.ws_col;
            return 0;
        }
        case LX_TIOCGPGRP: {
            int pg = pt->pgid ? pt->pgid : (int)p->pgid;
            return lx_put(arg, &pg, sizeof(pg));
        }
        case LX_TIOCSPGRP: {
            int pg = 0;
            if (lx_get(&pg, arg, sizeof(pg))) return -EFAULT;
            if (pg < 0) return -EINVAL;
            pt->pgid = pg;     /* per-terminal; Ctrl-C stays tree-based in kterm */
            return 0;
        }
        case 0x540E: /* TIOCSCTTY */
        case 0x5422: /* TIOCNOTTY */
            return 0;
        case 0x5429: { /* TIOCGSID */
            int sid = (int)p->sid;
            return lx_put(arg, &sid, sizeof(sid));
        }
        case LX_FIONREAD:
        case LX_FIONBIO:
            return sys_ioctl((int)fd, req, arg);
        default:
            return -ENOTTY;
    }
}

/* inotify (253 init, 254 add_watch, 255 rm_watch, 294 init1): see inotify.c */
static long lx_inotify_init(process_t *p, long flags) {
    if (flags & ~(long)(04000 | 02000000)) return -EINVAL;
    int fd = allocate_fd(p);
    if (fd < 0) return -EMFILE;
    void *h = inotify_create();
    if (!h) { p->fds[fd].in_use = false; return -ENOMEM; }
    p->fds[fd].ref_count = fd_refcount_create();
    if (!p->fds[fd].ref_count) { inotify_release(h); p->fds[fd].in_use = false; return -ENOMEM; }
    p->fds[fd].fd_type = FD_TYPE_INOTIFY;
    p->fds[fd].file_handle = h;
    p->fds[fd].flags = (flags & 04000) ? O_NONBLOCK : 0;    /* IN_NONBLOCK */
    p->fds[fd].position = 0;
    p->fds[fd].cloexec = (flags & 02000000) != 0;           /* IN_CLOEXEC */
    p->fds[fd].in_use = true;
    return fd;
}
static void *lx_ino_get(process_t *p, long fd, long *err) {
    if (!lx_fd_ok(p, fd)) { *err = -EBADF; return NULL; }
    if (p->fds[fd].fd_type != FD_TYPE_INOTIFY || !p->fds[fd].file_handle) { *err = -EINVAL; return NULL; }
    return p->fds[fd].file_handle;
}
static long lx_inotify_add(process_t *p, long fd, const char *upath, uint32_t mask) {
    long err = 0;
    void *h = lx_ino_get(p, fd, &err);
    if (!h) return err;
    char path[256];
    long r = lx_resolve_f(p, LX_AT_FDCWD, upath, path, sizeof(path), false,
                          !(mask & 0x02000000u) /* IN_DONT_FOLLOW */);
    if (r) return r;
    int kind = lx_path_kind(path, NULL);
    if (!kind) return -ENOENT;
    return inotify_add(h, path, mask, kind == 2);
}
static long lx_inotify_rm(process_t *p, long fd, long wd) {
    long err = 0;
    void *h = lx_ino_get(p, fd, &err);
    if (!h) return err;
    return inotify_rm(h, (int)wd);
}
static long lx_inotify_read(process_t *p, long fd, void *ubuf, size_t len) {
    void *h = p->fds[fd].file_handle;
    extern bool proc_signal_interrupts(process_t *);
    char kb[512];
    for (;;) {
        if (inotify_ready(h)) {
            size_t n = len < sizeof(kb) ? len : sizeof(kb);
            long got = inotify_take(h, kb, n);
            if (got == -EINVAL && n < len) return -EINVAL;
            if (got < 0) return got;
            if (got > 0) return lx_put(ubuf, kb, (size_t)got) ? -EFAULT : got;
        }
        if (p->fds[fd].flags & O_NONBLOCK) return -EAGAIN;
        if (proc_kill_pending(p) || (p->linux_abi && proc_signal_interrupts(p))) return -EINTR;
        fork_release_pending(p);
        thread_sleep(1);
    }
}

static long lx_ioctl(process_t *p, long fd, unsigned long req, void *arg) {
    if (!lx_fd_ok(p, fd)) return -EBADF;
    if (p->fds[fd].fd_type == FD_TYPE_INOTIFY) {
        if (req == LX_FIONREAD) {
            int n = (int)inotify_pending_bytes(p->fds[fd].file_handle);
            return lx_put(arg, &n, sizeof(n));
        }
        if (req == LX_FIONBIO) return sys_ioctl((int)fd, req, arg);
        return -ENOTTY;
    }
    {
        kpty_t *pt = lx_fd_pty(p, fd);
        if (pt) return lx_pty_ioctl(p, fd, pt, req, arg);
    }
    if (p->fds[fd].fd_type == FD_TYPE_DEV_TTY) {
        /* /dev/tty in a desktop-terminal process is that window's pty, so
         * each terminal has its own termios, winsize and foreground pgid.
         * Before this, ash's job control (tcgetpgrp/tcsetpgrp on /dev/tty)
         * used the console's global foreground pgid: the first terminal's
         * ash claimed it, and every later terminal's ash saw a foreign pgid
         * and stopped itself with SIGTTIN (exit status 149). */
        for (long f = 0; f <= 2; f++) {
            kpty_t *ct = lx_fd_pty(p, f);
            if (ct) return lx_pty_ioctl(p, fd, ct, req, arg);
        }
    }
    if (p->fds[fd].fd_type == FD_TYPE_DEV_FB)
        return fbdev_ioctl(p, req, arg);
    if (p->fds[fd].fd_type == FD_TYPE_DEV_INPUT)
        return evdev_ioctl(p->fds[fd].file_handle, req, arg);
    if (p->fds[fd].fd_type == FD_TYPE_DEV_UINPUT)
        return uinput_ioctl(p->fds[fd].file_handle, req, arg);
    if (p->fds[fd].fd_type == FD_TYPE_DEV_WM)
        return wm_dev_ioctl(p->fds[fd].file_handle, req, arg);
    bool tty = lx_fd_is_tty(p, fd);
    if (tty) {                                  /* KD / VT ioctls (fbdev apps) */
        bool handled = false;
        long kr = fbdev_tty_ioctl(p, req, arg, &handled);
        if (handled) return kr;
    }
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
            {
                extern void keyboard_set_linux_noncanon(bool enable);
                keyboard_set_linux_noncanon(!(ti.c_lflag & LX_ICANON));
            }
            return 0;
        }
        case LX_TIOCGWINSZ: {
            if (!tty) return -ENOTTY;
            uint32_t cols = 80, rows = 25;
            fb_console_get_size(&cols, &rows);
            if (lx_con_ws_rows && lx_con_ws_cols) { rows = lx_con_ws_rows; cols = lx_con_ws_cols; }
            lx_winsize_t ws = { (uint16_t)rows, (uint16_t)cols, 0, 0 };
            return lx_put(arg, &ws, sizeof(ws));
        }
        case LX_TIOCSWINSZ: {
            if (!tty) return -ENOTTY;
            lx_winsize_t ws;
            if (lx_get(&ws, arg, sizeof(ws))) return -EFAULT;
            uint32_t cols = 80, rows = 25;
            fb_console_get_size(&cols, &rows);
            if (ws.ws_row == rows && ws.ws_col == cols) lx_con_ws_rows = lx_con_ws_cols = 0;
            else { lx_con_ws_rows = ws.ws_row; lx_con_ws_cols = ws.ws_col; }
            return 0;
        }
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

/* the address space this syscall runs in (what sys_mmap mapped into) */
static uint64_t *lx_cur_pml4(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return (uint64_t *)phys_to_virt(cr3 & PTE_ADDR_MASK);
}

static long lx_mmap(process_t *p, uint64_t addr, uint64_t len, long prot, long flags,
                    long fd, int64_t off) {
    if (len == 0) return -EINVAL;
    if (flags & LX_MAP_ANONYMOUS) {
        /* MAP_SHARED|MAP_ANONYMOUS: same frames in parent and fork children
         * (VMM_SHARED PTEs are never COW-split). */
        int kf = MAP_ANONYMOUS | MAP_PRIVATE | ((flags & LX_MAP_FIXED) ? MAP_FIXED : 0);
        if (flags & LX_MAP_SHARED)
            kf |= SYS_MMAP_KSHARED;
        return (long)sys_mmap((void *)addr, (size_t)len, (int)prot, kf);
    }
    if (lx_fd_ok(p, fd) && p->fds[fd].fd_type == FD_TYPE_DEV_FB)
        return fbdev_mmap(p, addr, len, prot, flags, off);
    if (lx_fd_ok(p, fd) && p->fds[fd].fd_type == FD_TYPE_DEV_WM)
        return wm_dev_mmap(p->fds[fd].file_handle, addr, len, prot, flags, off);
    /* File mapping: private copy of the file contents (no write-back). */
    if (!lx_fd_ok(p, fd) || p->fds[fd].fd_type != FD_TYPE_VFS)
        return -EBADF;
    if (off < 0 || (off & (PAGE_SIZE - 1)))
        return -EINVAL;
    if (flags & LX_MAP_SHARED) {
        /* MAP_SHARED file: shared frames + write-back record (mmap_shared.c) */
        extern long shmf_map(uint64_t *pml4, uint64_t va, uint64_t len, int vfs_fd, uint64_t foff, bool writable);
        int kf = MAP_ANONYMOUS | MAP_PRIVATE | SYS_MMAP_KSHARED | ((flags & LX_MAP_FIXED) ? MAP_FIXED : 0);
        long va = (long)sys_mmap((void *)addr, (size_t)len, PROT_READ | PROT_WRITE | (int)(prot & PROT_EXEC), kf);
        if (va < 0 && va > -4096) return va;
        long r = shmf_map(lx_cur_pml4(), (uint64_t)va, len, p->fds[fd].vfs_fd, (uint64_t)off,
                          (prot & PROT_WRITE) != 0);
        if (r < 0) { sys_munmap((void *)va, (size_t)len); return r; }
        return va;
    }
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
    long r = sys_wait_pid(pid, uwstatus);
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

/* waitid(idtype, id, infop, options, rusage). Exited children only (no
 * job-control stop/continue reporting); WNOWAIT peeks without reaping. */
static void lx_siginfo_child(uint8_t *si, uint32_t pid, int raw) {
    memset(si, 0, 128);
    int signo = 17, code, st;                               /* SIGCHLD */
    if (raw > 128 && raw < 128 + 32) { code = 2; st = raw - 128; }   /* CLD_KILLED */
    else { code = 1; st = raw & 0xff; }                     /* CLD_EXITED */
    memcpy(si + 0, &signo, 4);
    memcpy(si + 8, &code, 4);
    memcpy(si + 16, &pid, 4);
    memcpy(si + 24, &st, 4);
}

static long lx_waitid(process_t *p, long idtype, long id, void *uinfo, long options, void *urusage) {
    const long known = LX_WNOHANG | 2 | 4 | 8 | 0x01000000 | 0x20000000 | 0x40000000 | (long)0x80000000;
    if (options & ~known) return -EINVAL;
    if (!(options & (2 | 4 | 8))) return -EINVAL;
    long pid;
    switch (idtype) {
        case 0: pid = -1; break;                                    /* P_ALL */
        case 1: if (id <= 0) return -EINVAL; pid = id; break;       /* P_PID */
        case 2: pid = id > 0 ? -id : 0; break;                      /* P_PGID */
        default: return -EINVAL;
    }
    if (urusage) {
        uint8_t z[144];
        memset(z, 0, sizeof(z));
        lx_put(urusage, z, sizeof(z));
    }
    uint8_t si[128];
    memset(si, 0, sizeof(si));
    bool match = false;
    for (process_t *c = p->child; c; c = c->sibling)
        if (pid == -1 || (pid > 0 && (long)c->pid == pid) || (pid < -1 && (long)c->pgid == -pid) ||
            (pid == 0 && c->pgid == p->pgid)) { match = true; break; }
    if (!match) return -ECHILD;
    if (!(options & 4)) {                       /* only stop/continue asked: never reported */
        if (options & LX_WNOHANG) return uinfo ? (lx_put(uinfo, si, sizeof(si)) ? -EFAULT : 0) : 0;
        return -ECHILD;
    }
    if (options & 0x01000000) {                 /* WNOWAIT: report, leave the zombie */
        for (;;) {
            fork_release_pending(p);
            process_t *z = NULL;
            bool any = false;
            for (process_t *c = p->child; c; c = c->sibling) {
                bool m = pid == -1 || (pid > 0 && (long)c->pid == pid) ||
                         (pid < -1 && (long)c->pgid == -pid) || (pid == 0 && c->pgid == p->pgid);
                if (!m) continue;
                any = true;
                if (c->is_zombie) { z = c; break; }
            }
            if (!any) return -ECHILD;
            if (z) {
                lx_siginfo_child(si, z->pid, z->exit_status);
                return uinfo ? (lx_put(uinfo, si, sizeof(si)) ? -EFAULT : 0) : 0;
            }
            if (options & LX_WNOHANG) return uinfo ? (lx_put(uinfo, si, sizeof(si)) ? -EFAULT : 0) : 0;
            if (proc_kill_pending(p)) return -EINTR;
            thread_sleep(5);
        }
    }
    if (options & LX_WNOHANG) {
        bool any = false;
        for (process_t *c = p->child; c; c = c->sibling)
            if (c->is_zombie && (pid == -1 || (pid > 0 && (long)c->pid == pid) ||
                (pid < -1 && (long)c->pgid == -pid) || (pid == 0 && c->pgid == p->pgid))) { any = true; break; }
        if (!any) return uinfo ? (lx_put(uinfo, si, sizeof(si)) ? -EFAULT : 0) : 0;
    }
    fork_release_pending(p);
    /* the user infop doubles as the raw-status scratch (sys_wait_pid copies out) */
    long r = sys_wait_pid(pid, uinfo ? (int *)uinfo : NULL);
    if (r < 0) return r;
    int raw = 0;
    if (uinfo && lx_get(&raw, uinfo, sizeof(raw))) return -EFAULT;
    lx_siginfo_child(si, (uint32_t)r, raw);
    if (uinfo && lx_put(uinfo, si, sizeof(si))) return -EFAULT;
    return 0;
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

int lx_strace;   /* zeus `lxtrace on`: log Linux syscalls to serial */

/* ---- futex: real wait queues ----
 * Key: (address space, va) -- or, for a non-PRIVATE futex on a VMM_SHARED
 * page (MAP_SHARED anon/file), the physical address of the word so that
 * waiters in different processes meet.  Waiters live on their own kernel
 * stack; WAKE/REQUEUE flip w->woken / retarget w->key under lx_futex_lock.
 * A waiter whose word changed before it was queued returns -EAGAIN; once
 * queued it sleeps until woken, timed out or signalled (it still polls the
 * word as a spurious-wake fallback for exits that clear a word without
 * FUTEX_WAKE, e.g. killed threads). */
typedef struct lx_futex_waiter {
    struct lx_futex_waiter *next;
    uint64_t key_as, key_addr;
    uint32_t bitset;
    volatile int woken;
    bool requeued;
} lx_futex_waiter_t;
static lx_futex_waiter_t *lx_futex_q;
static spinlock_t lx_futex_lock;
#define LX_FUTEX_PRIVATE 128
#define LX_FUTEX_CLOCK_RT 256

static int lx_futex_key(uint64_t uaddr, long opflags, uint64_t *as, uint64_t *addr) {
    if (uaddr & 3) return -EINVAL;
    uint64_t *pml4 = lx_cur_pml4();
    if (!(opflags & LX_FUTEX_PRIVATE)) {
        uint64_t *pte = vmm_get_pte_in(pml4, uaddr & ~0xFFFULL);
        if (pte && (*pte & 1) && (*pte & VMM_SHARED)) {
            *as = 0;                                   /* global (physical) key */
            *addr = (*pte & PTE_ADDR_MASK) | (uaddr & 0xFFF);
            return 0;
        }
    }
    *as = (uint64_t)(uintptr_t)pml4;
    *addr = uaddr;
    return 0;
}
static void lx_futex_unlink(lx_futex_waiter_t *w) {
    for (lx_futex_waiter_t **pp = &lx_futex_q; *pp; pp = &(*pp)->next)
        if (*pp == w) { *pp = w->next; w->next = NULL; return; }
}
/* wake up to n waiters on key (bitset match); caller holds the lock */
static int lx_futex_wake_locked(uint64_t as, uint64_t addr, int n, uint32_t bitset) {
    int woke = 0;
    lx_futex_waiter_t **pp = &lx_futex_q;
    while (*pp && woke < n) {
        lx_futex_waiter_t *w = *pp;
        if (w->key_as == as && w->key_addr == addr && (w->bitset & bitset)) {
            *pp = w->next; w->next = NULL;
            __atomic_store_n(&w->woken, 1, __ATOMIC_RELEASE);
            woke++;
        } else pp = &w->next;
    }
    return woke;
}
/* used on thread exit (CLONE_CHILD_CLEARTID): FUTEX_WAKE(tid word, 1) */
static void lx_futex_wake_addr(uint64_t uaddr, int n) {
    uint64_t as, addr;
    if (lx_futex_key(uaddr, 0, &as, &addr)) return;
    klock(&lx_futex_lock);
    lx_futex_wake_locked(as, addr, n, 0xffffffffu);
    kunlock(&lx_futex_lock);
}
static long lx_futex(process_t *p, uint64_t uaddr, long opw, uint32_t val, uint64_t a4,
                     uint64_t uaddr2, uint32_t val3) {
    long op = opw & 0x7f;
    uint64_t as, addr;
    long r;
    uint32_t cur;
    switch (op) {
    case 0: /* WAIT */
    case 9: /* WAIT_BITSET */ {
        uint32_t bitset = op == 9 ? val3 : 0xffffffffu;
        if (!bitset) return -EINVAL;
        if ((r = lx_futex_key(uaddr, opw, &as, &addr))) return r;
        extern uint64_t ktime_deadline_ms(uint64_t);
        extern bool     ktime_expired(uint64_t);
        extern bool     proc_signal_interrupts(process_t *);
        bool has_to = a4 != 0;
        uint64_t dl = 0;
        if (has_to) {
            lx_timespec_t ts;
            if (lx_get(&ts, (void *)a4, sizeof(ts))) return -EFAULT;
            if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000) return -EINVAL;
            uint64_t rel_us = (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000;
            if (op == 9) {          /* absolute: CLOCK_MONOTONIC or CLOCK_REALTIME */
                uint64_t now = (opw & LX_FUTEX_CLOCK_RT) ? lx_real_us() : lx_mono_us();
                rel_us = rel_us > now ? rel_us - now : 0;
            }
            dl = ktime_deadline_ms((rel_us + 999) / 1000);
        }
        if (lx_get(&cur, (void *)uaddr, 4)) return -EFAULT;   /* fault the page in */
        lx_futex_waiter_t w = { NULL, as, addr, bitset, 0, false };
        klock(&lx_futex_lock);
        if (lx_get(&cur, (void *)uaddr, 4)) { kunlock(&lx_futex_lock); return -EFAULT; }
        if (cur != val) { kunlock(&lx_futex_lock); return -EAGAIN; }
        w.next = lx_futex_q; lx_futex_q = &w;
        kunlock(&lx_futex_lock);
        long ret = 0;
        for (;;) {
            if (__atomic_load_n(&w.woken, __ATOMIC_ACQUIRE)) return 0;
            if (has_to && ktime_expired(dl)) { ret = -ETIMEDOUT; break; }
            if (proc_kill_pending(p) || (p->linux_abi && proc_signal_interrupts(p))) { ret = -EINTR; break; }
            if (!w.requeued && lx_get(&cur, (void *)uaddr, 4) == 0 && cur != val) { ret = 0; break; }
            fork_release_pending(p);
            thread_sleep(1);
        }
        klock(&lx_futex_lock);
        if (__atomic_load_n(&w.woken, __ATOMIC_ACQUIRE)) ret = 0;   /* woken meanwhile */
        else lx_futex_unlink(&w);
        kunlock(&lx_futex_lock);
        return ret;
    }
    case 1:  /* WAKE */
    case 10: /* WAKE_BITSET */ {
        uint32_t bitset = op == 10 ? val3 : 0xffffffffu;
        if (!bitset) return -EINVAL;
        if ((r = lx_futex_key(uaddr, opw, &as, &addr))) return r;
        klock(&lx_futex_lock);
        int n = lx_futex_wake_locked(as, addr, (int)(val > 0x7fffffff ? 0x7fffffff : val), bitset);
        kunlock(&lx_futex_lock);
        return n;
    }
    case 3: /* REQUEUE */
    case 4: /* CMP_REQUEUE */ {
        uint64_t as2, addr2;
        if ((r = lx_futex_key(uaddr, opw, &as, &addr))) return r;
        if ((r = lx_futex_key(uaddr2, opw, &as2, &addr2))) return r;
        int nreq = (int)((uint32_t)a4 > 0x7fffffff ? 0x7fffffff : (uint32_t)a4);
        if ((int)val < 0 || nreq < 0) return -EINVAL;
        if (op == 4 && lx_get(&cur, (void *)uaddr, 4)) return -EFAULT;
        klock(&lx_futex_lock);
        if (op == 4) {
            if (lx_get(&cur, (void *)uaddr, 4)) { kunlock(&lx_futex_lock); return -EFAULT; }
            if (cur != val3) { kunlock(&lx_futex_lock); return -EAGAIN; }
        }
        int woke = lx_futex_wake_locked(as, addr, (int)val, 0xffffffffu);
        int moved = 0;
        for (lx_futex_waiter_t *w = lx_futex_q; w && moved < nreq; w = w->next)
            if (w->key_as == as && w->key_addr == addr) {
                w->key_as = as2; w->key_addr = addr2; w->requeued = true; moved++;
            }
        kunlock(&lx_futex_lock);
        return op == 4 ? woke + moved : woke;
    }
    case 5: /* WAKE_OP: atomic w.r.t. other futex ops (not user atomics) */ {
        uint64_t as2, addr2;
        if ((r = lx_futex_key(uaddr, opw, &as, &addr))) return r;
        if ((r = lx_futex_key(uaddr2, opw, &as2, &addr2))) return r;
        uint32_t fop = (val3 >> 28) & 7, cmp = (val3 >> 24) & 15;
        uint32_t oparg = (val3 >> 12) & 0xfff, cmparg = val3 & 0xfff;
        if ((val3 >> 28) & 8) oparg = 1u << (oparg & 31);
        uint32_t old, nv;
        if (lx_get(&old, (void *)uaddr2, 4)) return -EFAULT;
        klock(&lx_futex_lock);
        if (lx_get(&old, (void *)uaddr2, 4)) { kunlock(&lx_futex_lock); return -EFAULT; }
        switch (fop) {
        case 0: nv = oparg; break;
        case 1: nv = old + oparg; break;
        case 2: nv = old | oparg; break;
        case 3: nv = old & ~oparg; break;
        case 4: nv = old ^ oparg; break;
        default: kunlock(&lx_futex_lock); return -ENOSYS;
        }
        if (lx_put((void *)uaddr2, &nv, 4)) { kunlock(&lx_futex_lock); return -EFAULT; }
        int woke = lx_futex_wake_locked(as, addr, (int)val, 0xffffffffu);
        bool c;
        int32_t so = (int32_t)old, sa = (int32_t)cmparg;
        switch (cmp) {
        case 0: c = so == sa; break; case 1: c = so != sa; break;
        case 2: c = so < sa;  break; case 3: c = so <= sa; break;
        case 4: c = so > sa;  break; case 5: c = so >= sa; break;
        default: c = false;
        }
        if (c) woke += lx_futex_wake_locked(as2, addr2, (int)(uint32_t)a4, 0xffffffffu);
        kunlock(&lx_futex_lock);
        return woke;
    }
    default:
        return -ENOSYS;
    }
}

/* ---- timerfd: lazily evaluated deadline on the monotonic clock ---- */
typedef struct {
    spinlock_t lock;
    int clock;
    uint64_t next_us;       /* monotonic deadline, 0 = disarmed */
    uint64_t interval_us;
    uint64_t exp;           /* expirations not yet read */
} lx_timerfd_t;
static void lx_tfd_update(lx_timerfd_t *t) {
    if (!t->next_us) return;
    uint64_t now = lx_mono_us();
    if (now < t->next_us) return;
    if (t->interval_us) {
        uint64_t k = (now - t->next_us) / t->interval_us + 1;
        t->exp += k;
        t->next_us += k * t->interval_us;
    } else {
        t->exp++;
        t->next_us = 0;
    }
}
bool lx_timerfd_ready(void *h) {
    lx_timerfd_t *t = (lx_timerfd_t *)h;
    if (!t) return false;
    klock(&t->lock);
    lx_tfd_update(t);
    bool r = t->exp != 0;
    kunlock(&t->lock);
    return r;
}
static long lx_timerfd_create(process_t *p, long clockid, long flags) {
    if (clockid != 0 && clockid != 1 && clockid != 7 && clockid != 8 && clockid != 9) return -EINVAL;
    if (flags & ~(04000L | 02000000L)) return -EINVAL;
    int fd = allocate_fd(p);
    if (fd < 0) return -EMFILE;
    lx_timerfd_t *t = (lx_timerfd_t *)kalloc(sizeof(*t));
    if (!t) { p->fds[fd].in_use = false; return -ENOMEM; }
    memset(t, 0, sizeof(*t));
    t->clock = (int)clockid;
    p->fds[fd].ref_count = fd_refcount_create();
    if (!p->fds[fd].ref_count) { kfree(t); p->fds[fd].in_use = false; return -ENOMEM; }
    p->fds[fd].fd_type = FD_TYPE_TIMERFD;
    p->fds[fd].file_handle = t;
    p->fds[fd].flags = (flags & 04000) ? O_NONBLOCK : 0;
    p->fds[fd].position = 0;
    p->fds[fd].cloexec = (flags & 02000000L) != 0;
    p->fds[fd].in_use = true;
    return fd;
}
static lx_timerfd_t *lx_tfd_get(process_t *p, long fd, long *err) {
    if (fd < 0 || fd >= MAX_OPEN_FILES_SYSCALL || !p->fds[fd].in_use) { *err = -EBADF; return NULL; }
    if (p->fds[fd].fd_type != FD_TYPE_TIMERFD) { *err = -EINVAL; return NULL; }
    return (lx_timerfd_t *)p->fds[fd].file_handle;
}
static void lx_tfd_cur(lx_timerfd_t *t, lx_timespec_t out[2]) {
    uint64_t now = lx_mono_us();
    uint64_t left = t->next_us ? (t->next_us > now ? t->next_us - now : 1) : 0;
    out[0].tv_sec = (int64_t)(t->interval_us / 1000000ULL);
    out[0].tv_nsec = (int64_t)(t->interval_us % 1000000ULL) * 1000;
    out[1].tv_sec = (int64_t)(left / 1000000ULL);
    out[1].tv_nsec = (int64_t)(left % 1000000ULL) * 1000;
}
static long lx_timerfd_settime(process_t *p, long fd, long flags, const void *unew, void *uold) {
    long err = 0;
    lx_timerfd_t *t = lx_tfd_get(p, fd, &err);
    if (!t) return err;
    if (flags & ~3L) return -EINVAL;
    lx_timespec_t nv[2];
    if (lx_get(nv, unew, sizeof(nv))) return -EFAULT;
    for (int i = 0; i < 2; i++)
        if (nv[i].tv_sec < 0 || nv[i].tv_nsec < 0 || nv[i].tv_nsec >= 1000000000) return -EINVAL;
    uint64_t iv = (uint64_t)nv[0].tv_sec * 1000000ULL + (uint64_t)nv[0].tv_nsec / 1000;
    uint64_t v  = (uint64_t)nv[1].tv_sec * 1000000ULL + (uint64_t)nv[1].tv_nsec / 1000;
    if (!v && (nv[1].tv_sec || nv[1].tv_nsec)) v = 1;
    if (!iv && (nv[0].tv_sec || nv[0].tv_nsec)) iv = 1;
    klock(&t->lock);
    lx_tfd_update(t);
    lx_timespec_t old[2];
    lx_tfd_cur(t, old);
    t->exp = 0;
    t->interval_us = iv;
    if (!v && !(nv[1].tv_sec || nv[1].tv_nsec)) {
        t->next_us = 0;                                   /* disarm */
    } else if (flags & 1) {                               /* TFD_TIMER_ABSTIME */
        bool rt = t->clock == 0 || t->clock == 8;
        uint64_t now_c = rt ? lx_real_us() : lx_mono_us(), mono = lx_mono_us();
        uint64_t abs = (uint64_t)nv[1].tv_sec * 1000000ULL + (uint64_t)nv[1].tv_nsec / 1000;
        t->next_us = abs > now_c ? mono + (abs - now_c) : mono;
        if (!t->next_us) t->next_us = 1;
    } else {
        t->next_us = lx_mono_us() + v;
    }
    kunlock(&t->lock);
    if (uold && lx_put(uold, old, sizeof(old))) return -EFAULT;
    return 0;
}
static long lx_timerfd_gettime(process_t *p, long fd, void *ucur) {
    long err = 0;
    lx_timerfd_t *t = lx_tfd_get(p, fd, &err);
    if (!t) return err;
    lx_timespec_t c[2];
    klock(&t->lock);
    lx_tfd_update(t);
    lx_tfd_cur(t, c);
    kunlock(&t->lock);
    return lx_put(ucur, c, sizeof(c));
}
static long lx_timerfd_read(process_t *p, long fd, void *ubuf, size_t len) {
    if (len < 8) return -EINVAL;
    lx_timerfd_t *t = (lx_timerfd_t *)p->fds[fd].file_handle;
    extern bool proc_signal_interrupts(process_t *);
    for (;;) {
        klock(&t->lock);
        lx_tfd_update(t);
        uint64_t v = t->exp;
        t->exp = 0;
        kunlock(&t->lock);
        if (v) return lx_put(ubuf, &v, 8) ? -EFAULT : 8;
        if (p->fds[fd].flags & O_NONBLOCK) return -EAGAIN;
        if (proc_kill_pending(p) || (p->linux_abi && proc_signal_interrupts(p))) return -EINTR;
        fork_release_pending(p);
        thread_sleep(1);
    }
}

/* ---- signalfd: reads dequeue pending signals in its mask ---- */
typedef struct { uint32_t mask; } lx_signalfd_t;   /* internal bits: 1u << signo */
bool lx_signalfd_ready(void *h, process_t *proc) {
    lx_signalfd_t *s = (lx_signalfd_t *)h;
    return s && proc && (__atomic_load_n(&proc->pending_signals, __ATOMIC_ACQUIRE) & s->mask);
}
static uint32_t lx_sigmask_from_user(uint64_t m) {
    uint32_t sm = (uint32_t)((m << 1) & 0xFFFFFFFEu);
    return sm & ~((1u << 9) | (1u << 19));          /* never KILL/STOP */
}
static long lx_signalfd(process_t *p, long fd, const void *umask, size_t sz, long flags) {
    if (flags & ~(04000L | 02000000L)) return -EINVAL;
    if (sz != 8) return -EINVAL;
    uint64_t m = 0;
    if (lx_get(&m, umask, 8)) return -EFAULT;
    if (fd != -1) {
        if (fd < 0 || fd >= MAX_OPEN_FILES_SYSCALL || !p->fds[fd].in_use) return -EBADF;
        if (p->fds[fd].fd_type != FD_TYPE_SIGNALFD) return -EINVAL;
        ((lx_signalfd_t *)p->fds[fd].file_handle)->mask = lx_sigmask_from_user(m);
        return fd;
    }
    int nfd = allocate_fd(p);
    if (nfd < 0) return -EMFILE;
    lx_signalfd_t *s = (lx_signalfd_t *)kalloc(sizeof(*s));
    if (!s) { p->fds[nfd].in_use = false; return -ENOMEM; }
    s->mask = lx_sigmask_from_user(m);
    p->fds[nfd].ref_count = fd_refcount_create();
    if (!p->fds[nfd].ref_count) { kfree(s); p->fds[nfd].in_use = false; return -ENOMEM; }
    p->fds[nfd].fd_type = FD_TYPE_SIGNALFD;
    p->fds[nfd].file_handle = s;
    p->fds[nfd].flags = (flags & 04000) ? O_NONBLOCK : 0;
    p->fds[nfd].position = 0;
    p->fds[nfd].cloexec = (flags & 02000000L) != 0;
    p->fds[nfd].in_use = true;
    return nfd;
}
/* take the lowest pending signal in mask; 0 if none */
static int lx_sig_dequeue(process_t *p, uint32_t mask) {
    for (;;) {
        uint32_t pend = __atomic_load_n(&p->pending_signals, __ATOMIC_ACQUIRE) & mask;
        if (!pend) return 0;
        int s = __builtin_ctz(pend);
        uint32_t bit = 1u << s;
        uint32_t prev = __atomic_fetch_and(&p->pending_signals, ~bit, __ATOMIC_SEQ_CST);
        if (prev & bit) return s;
    }
}
typedef struct {
    uint32_t ssi_signo; int32_t ssi_errno; int32_t ssi_code; uint32_t ssi_pid;
    uint32_t ssi_uid; int32_t ssi_fd; uint32_t ssi_tid; uint32_t ssi_band;
    uint32_t ssi_overrun; uint32_t ssi_trapno; int32_t ssi_status; int32_t ssi_int;
    uint64_t ssi_ptr; uint64_t ssi_utime; uint64_t ssi_stime; uint64_t ssi_addr;
    uint16_t ssi_addr_lsb; uint8_t pad[46];
} lx_sfd_siginfo_t;
_Static_assert(sizeof(lx_sfd_siginfo_t) == 128, "signalfd_siginfo is 128 bytes");
static long lx_signalfd_read(process_t *p, long fd, void *ubuf, size_t len) {
    if (len < sizeof(lx_sfd_siginfo_t)) return -EINVAL;
    lx_signalfd_t *s = (lx_signalfd_t *)p->fds[fd].file_handle;
    extern bool proc_signal_interrupts(process_t *);
    size_t got = 0;
    for (;;) {
        while (got + sizeof(lx_sfd_siginfo_t) <= len) {
            int sig = lx_sig_dequeue(p, s->mask);
            if (!sig) break;
            lx_sfd_siginfo_t si;
            memset(&si, 0, sizeof(si));
            si.ssi_signo = (uint32_t)sig;
            si.ssi_code = 0;                              /* SI_USER */
            if (lx_put((char *)ubuf + got, &si, sizeof(si))) return got ? (long)got : -EFAULT;
            got += sizeof(si);
        }
        if (got) return (long)got;
        if (p->fds[fd].flags & O_NONBLOCK) return -EAGAIN;
        if (proc_kill_pending(p) || (p->linux_abi && proc_signal_interrupts(p))) return -EINTR;
        fork_release_pending(p);
        thread_sleep(1);
    }
}
/* rt_sigtimedwait(set, info, timeout, sigsetsize): same dequeue */
static long lx_sigtimedwait(process_t *p, const void *uset, void *uinfo, const void *uts, size_t sz) {
    if (sz != 8) return -EINVAL;
    uint64_t m = 0;
    if (lx_get(&m, uset, 8)) return -EFAULT;
    uint32_t mask = lx_sigmask_from_user(m);
    extern uint64_t ktime_deadline_ms(uint64_t);
    extern bool     ktime_expired(uint64_t);
    extern bool     proc_signal_interrupts(process_t *);
    bool has_to = uts != NULL;
    uint64_t dl = 0;
    if (has_to) {
        lx_timespec_t ts;
        if (lx_get(&ts, uts, sizeof(ts))) return -EFAULT;
        if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000) return -EINVAL;
        dl = ktime_deadline_ms((uint64_t)ts.tv_sec * 1000 + ((uint64_t)ts.tv_nsec + 999999) / 1000000);
    }
    for (;;) {
        int sig = lx_sig_dequeue(p, mask);
        if (sig) {
            if (uinfo) {
                uint8_t si[128];
                memset(si, 0, sizeof(si));
                *(int32_t *)si = sig;                     /* si_signo; errno/code 0 */
                if (lx_put(uinfo, si, sizeof(si))) return -EFAULT;
            }
            return sig;
        }
        if (has_to && ktime_expired(dl)) return -EAGAIN;
        if (proc_kill_pending(p) || (p->linux_abi && proc_signal_interrupts(p))) return -EINTR;
        fork_release_pending(p);
        thread_sleep(1);
    }
}

static long lx_dispatch(linux_syscall_frame_t *fr) {
    process_t *p = lx_proc();
    const uint64_t nr = fr->nr;
    const uint64_t a1 = fr->a1, a2 = fr->a2, a3 = fr->a3,
                   a4 = fr->a4, a5 = fr->a5, a6 = fr->a6;
    char path[256], path2[256];
    long r;

    /* sigsuspend woke on a signal that ran no handler (ignored/default):
     * put the caller's mask back before anything else. */
    if (p && p->sigsuspend_restore && !p->sig_in_handler) {
        p->blocked_signals = p->sigsuspend_saved_mask;
        p->sigsuspend_restore = false;
    }

    switch (nr) {   /* EPOLLET edge source for objects without edge generations */
    case LX_read: case LX_write: case LX_readv: case LX_writev: case 17: case 18:
    case 43: case 44: case 45: case 46: case 47: case LX_accept4:
        lx_io_bump(p, (long)(int)a1);
        break;
    default: break;
    }
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
        /* fds 0..2 are ordinary descriptors on Linux: close() must free the
         * slot so open()/dup reuse it (ash `cmd &` -> close(0);
         * open("/dev/null") == 0).  Old code returned 0 for tty 0..2 and
         * sys_close refused fd < 3 with EBADF. */
        if (lx_fd_ok(p, (long)a1) && p->fds[a1].fd_type == FD_TYPE_DEV_FB) {
            p->fds[a1].in_use = false;
            p->fds[a1].fd_type = 0;
            fbdev_close(p);
            return 0;
        }
        if (lx_fd_ok(p, (long)a1) &&
            (p->fds[a1].fd_type == FD_TYPE_DEV_NULL ||
             p->fds[a1].fd_type == FD_TYPE_DEV_TTY)) {
            p->fds[a1].in_use = false;
            p->fds[a1].fd_type = 0;
            return 0;
        }
        { extern int sys_close_linux(int fd); return sys_close_linux((int)a1); }
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
        if (r == 0 && (a2 & (LX_O_CLOEXEC | LX_O_NONBLOCK))) {
            int fds[2];
            if (lx_get(fds, (void *)a1, sizeof(fds)) == 0) {
                for (int k = 0; k < 2; k++) {
                    if (!lx_fd_ok(p, fds[k])) continue;
                    if (a2 & LX_O_CLOEXEC)  p->fds[fds[k]].cloexec = true;
                    if (a2 & LX_O_NONBLOCK) p->fds[fds[k]].flags |= O_NONBLOCK;
                }
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
        int pr;
        if (np) {
            pr = sys_poll_kernel(pf, (unsigned)np, timeout_ms);
        } else {                    /* select(0, ..., timeout): plain sleep */
            if (timeout_ms > 0) {
                uint64_t dl = ktime_deadline_ms((uint64_t)timeout_ms);
                fork_release_pending(p);
                while (ktime_now_ms() < dl) {
                    extern bool proc_signal_interrupts(process_t *);
                    if (proc_signal_interrupts(p)) return -EINTR;
                    __asm__ volatile("sti; hlt" ::: "memory");
                }
            }
            pr = 0;
        }
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
        vfs_sync_all();                         /* exFAT: flush + clear VolumeDirty */
        return 0;

    /* ---- metadata / paths ---- */
    case LX_stat: case LX_lstat:
        if ((r = lx_resolve_f(p, LX_AT_FDCWD, (const char *)a1, path, sizeof(path), false,
                              nr == LX_stat))) return r;
        lx_exe_alias(path, sizeof(path));
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
        if ((r = lx_resolve_f(p, (long)(int)a1, (const char *)a2, path, sizeof(path), false,
                              !(a4 & LX_AT_SYMLINK_NOFOLLOW)))) return r;
        lx_exe_alias(path, sizeof(path));
        return lx_stat_path(path, (void *)a3);
    }
    case LX_access:
        if ((r = lx_resolve(p, LX_AT_FDCWD, (const char *)a1, path, sizeof(path), false))) return r;
        if (lx_dev_path(path, NULL) || lx_proc_text_path(p, path)) return 0;
        lx_exe_alias(path, sizeof(path));
        if (!lx_path_kind(path, NULL)) return lx_synth_dir(path) ? 0 : -ENOENT;
        return lx_access_path(path, (long)a2);
    case LX_faccessat: case LX_faccessat2:
        if ((r = lx_resolve(p, (long)(int)a1, (const char *)a2, path, sizeof(path), false))) return r;
        if (lx_dev_path(path, NULL) || lx_proc_text_path(p, path)) return 0;
        lx_exe_alias(path, sizeof(path));
        if (!lx_path_kind(path, NULL)) return lx_synth_dir(path) ? 0 : -ENOENT;
        return lx_access_path(path, (long)a3);
    case LX_readlink: case LX_readlinkat: {
        const char *up = (const char *)(nr == LX_readlink ? a1 : a2);
        long dfd = nr == LX_readlink ? LX_AT_FDCWD : (long)(int)a1;
        void *ubuf = (void *)(nr == LX_readlink ? a2 : a3);
        size_t bufsiz = (size_t)(nr == LX_readlink ? a3 : a4);
        if ((r = lx_resolve_f(p, dfd, up, path, sizeof(path), false, false))) return r;
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
        if ((long)bufsiz <= 0) return -EINVAL;
        {
            char *lb = (char *)kalloc(VFS_MAX_PATH);
            if (!lb) return -ENOMEM;
            int n = vfs_readlink(path, lb, VFS_MAX_PATH);
            if (n >= 0) {
                size_t c = (size_t)n < bufsiz ? (size_t)n : bufsiz;
                long pr = lx_put(ubuf, lb, c);
                kfree(lb);
                return pr ? -EFAULT : (long)c;
            }
            kfree(lb);
        }
        return lx_path_kind(path, NULL) ? -EINVAL : -ENOENT;   /* not a symlink */
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
        uint32_t dmode = (uint32_t)(nr == LX_mkdir ? a2 : a3);
        if ((r = lx_resolve_f(p, dfd, up, path, sizeof(path), false, false))) return r;
        if (lx_path_kind(path, NULL)) return -EEXIST;
        if (!vfs_has_root_mount()) return -ENODEV;
        r = vfs_mkdir(path);
        if (r >= 0 && vfs_has_inode_perms(path))
            vfs_setattr(path, VFS_SA_MODE | VFS_SA_QUIET, (dmode & 07777) & ~p->file_umask, 0, 0);
        return r < 0 ? (r == -1 ? -EIO : r) : 0;
    }
    case LX_rmdir: case LX_unlink: case LX_unlinkat: {
        long dfd = nr == LX_unlinkat ? (long)(int)a1 : LX_AT_FDCWD;
        const char *up = (const char *)(nr == LX_unlinkat ? a2 : a1);
        if ((r = lx_resolve_f(p, dfd, up, path, sizeof(path), false, false))) return r;
        int kind = lx_path_kind(path, NULL);
        if (!kind) return -ENOENT;
        bool want_dir = nr == LX_rmdir || (nr == LX_unlinkat && (a3 & LX_AT_REMOVEDIR));
        if (want_dir && kind != 2) return -ENOTDIR;
        if (!want_dir && kind == 2) return -EISDIR;
        if (want_dir) {
            r = vfs_rmdir(path);
        } else {
            r = vfs_unlink(path);
            if (r >= 0) fifo_unreg(path);
        }
        return r < 0 ? (r == -1 ? -EIO : r) : 0;
    }
    case LX_rename: case LX_renameat: case LX_renameat2: {
        long d1 = nr == LX_rename ? LX_AT_FDCWD : (long)(int)a1;
        const char *u1 = (const char *)(nr == LX_rename ? a1 : a2);
        long d2 = nr == LX_rename ? LX_AT_FDCWD : (long)(int)a3;
        const char *u2 = (const char *)(nr == LX_rename ? a2 : a4);
        if ((r = lx_resolve_f(p, d1, u1, path, sizeof(path), false, false))) return r;
        if ((r = lx_resolve_f(p, d2, u2, path2, sizeof(path2), false, false))) return r;
        if (!lx_path_kind(path, NULL)) return -ENOENT;
        r = vfs_rename(path, path2);
        if (r >= 0) fifo_rename(path, path2);
        return r < 0 ? (r == -1 ? -EIO : r) : 0;
    }
    case LX_truncate: case LX_ftruncate: {
        /* Best-effort: FAT32 truncate exists for paths; ftruncate via vfs. */
        if (nr == LX_truncate) {
            if ((r = lx_resolve(p, LX_AT_FDCWD, (const char *)a1, path, sizeof(path), false)))
                return r;
            if (!lx_path_kind(path, NULL)) return -ENOENT;
            r = vfs_truncate(path, (uint64_t)a2);
            return r < 0 ? (r == -1 ? -EIO : r) : 0;
        }
        if (!lx_fd_ok(p, (long)a1)) return -EBADF;
        if (p->fds[a1].fd_type == FD_TYPE_DEV_NULL) return 0;
        if (p->fds[a1].fd_type != FD_TYPE_VFS) return -EINVAL;
        /* Per-fs ftruncate (exFAT); FAT32 still ignores the length. */
        r = vfs_ftruncate(p->fds[a1].vfs_fd, (uint64_t)a2);
        return r < 0 ? (r == -1 ? -EIO : r) : 0;
    }
    case LX_umask:   return sys_umask((mode_t)a1);
    case LX_chmod: case LX_fchmodat: case LX_fchmod: {
        uint32_t mode;
        if (nr == LX_fchmod) {
            mode = (uint32_t)a2;
            r = lx_fd_path(p, (long)a1, path, sizeof(path));
            if (r == 1) return 0;               /* pipe/socket/tty: accepted */
            if (r < 0) return r;
        } else {
            const char *up = (const char *)(nr == LX_chmod ? a1 : a2);
            long dfd = nr == LX_chmod ? LX_AT_FDCWD : (long)(int)a1;
            mode = (uint32_t)(nr == LX_chmod ? a2 : a3);
            bool nf = nr == LX_fchmodat && (a4 & LX_AT_SYMLINK_NOFOLLOW);
            if (nr == LX_fchmodat && (a4 & ~(uint64_t)LX_AT_SYMLINK_NOFOLLOW)) return -EINVAL;
            if ((r = lx_resolve_f(p, dfd, up, path, sizeof(path), false, !nf))) return r;
            if (nf) { char lb[4]; if (vfs_readlink(path, lb, sizeof(lb)) >= 0) return -EOPNOTSUPP; }
        }
        if (lx_dev_path(path, NULL) || lx_proc_text_path(p, path)) return 0;
        lx_exe_alias2(path, sizeof(path), false);  /* never chmod busybox */
        if (!lx_path_kind(path, NULL)) return -ENOENT;
        r = vfs_setattr(path, VFS_SA_MODE, mode & 07777, 0, 0);
        if (r != -ENOSYS) return r;
        return lx_mode_set(path, mode);         /* FAT32/exFAT: in-memory table */
    }
    case LX_chown: case LX_fchown: case LX_lchown: case LX_fchownat: {
        uint32_t uid, gid;
        if (nr == LX_fchown) {
            uid = (uint32_t)a2; gid = (uint32_t)a3;
            r = lx_fd_path(p, (long)a1, path, sizeof(path));
            if (r == 1) return 0;
            if (r < 0) return r;
        } else {
            long dfd = nr == LX_fchownat ? (long)(int)a1 : LX_AT_FDCWD;
            const char *up = (const char *)(nr == LX_fchownat ? a2 : a1);
            uid = (uint32_t)(nr == LX_fchownat ? a3 : a2);
            gid = (uint32_t)(nr == LX_fchownat ? a4 : a3);
            bool nf = nr == LX_lchown || (nr == LX_fchownat && (a5 & LX_AT_SYMLINK_NOFOLLOW));
            if (nr == LX_fchownat && (a5 & ~(uint64_t)(LX_AT_SYMLINK_NOFOLLOW | LX_AT_EMPTY_PATH)))
                return -EINVAL;
            if ((r = lx_resolve_f(p, dfd, up, path, sizeof(path),
                                  nr == LX_fchownat && (a5 & LX_AT_EMPTY_PATH), !nf))) return r;
        }
        if (lx_dev_path(path, NULL) || lx_proc_text_path(p, path)) return 0;
        if (!lx_path_kind(path, NULL) && !lx_synth_dir(path)) return -ENOENT;
        r = vfs_setattr(path, VFS_SA_OWNER, 0, uid, gid);
        return r == -ENOSYS ? 0 : r;            /* FAT32/exFAT: accepted, not stored */
    }
    case LX_utimensat: {
        /* BusyBox touch: utimensat first; on ENOENT it open(O_CREAT).
         * Always returning 0 made touch exit success without creating.
         * FAT32 stores the times (tar, touch -d, make); other filesystems
         * without a set-times hook accept and ignore them. */
        long dfd = (long)(int)a1;
        const char *up = (const char *)a2;
        if (!up) {                                      /* futimens(fd, ...) */
            if (!lx_fd_ok(p, dfd)) return -EBADF;
            if (p->fds[dfd].fd_type != FD_TYPE_VFS || p->fds[dfd].vfs_fd < 0 ||
                vfs_fd_get_path(p->fds[dfd].vfs_fd, path, sizeof(path)) != 0)
                return 0;
        } else {
            if ((r = lx_resolve_f(p, dfd, up, path, sizeof(path), (a4 & 0x1000) != 0,
                                  !(a4 & LX_AT_SYMLINK_NOFOLLOW))))
                return r; /* AT_EMPTY_PATH */
            if (!lx_path_kind(path, NULL)) return -ENOENT;
        }
        int64_t nowt = (int64_t)(lx_real_us() / 1000000ULL), tv[4] = { 0, 0, 0, 0 };
        int64_t at = nowt, mt = nowt;
        bool sa = true, sm = true;
        if (a3) {
            if (lx_get(tv, (const void *)a3, sizeof(tv))) return -EFAULT;
            const int64_t NOW = (1L << 30) - 1, OMIT = (1L << 30) - 2;
            if (tv[1] == OMIT) sa = false; else if (tv[1] != NOW) at = tv[0];
            if (tv[3] == OMIT) sm = false; else if (tv[3] != NOW) mt = tv[2];
        }
        return lx_store_times(path, sa, at, sm, mt);
    }
    case LX_utime: case LX_utimes: case LX_futimesat: {
        /* FAT32 stores the times; other filesystems accept and ignore them.
         * Validates the path like Linux. */
        long dfd = nr == LX_futimesat ? (long)(int)a1 : LX_AT_FDCWD;
        const char *up = (const char *)(nr == LX_futimesat ? a2 : a1);
        const void *ut = (const void *)(nr == LX_futimesat ? a3 : a2);
        if (!up) return -EFAULT;
        if ((r = lx_resolve(p, dfd, up, path, sizeof(path), false))) return r;
        if (!lx_path_kind(path, NULL) && !fifo_is(path)) return -ENOENT;
        if (!lx_path_kind(path, NULL)) return 0;        /* fifo: nothing stored */
        int64_t nowt = (int64_t)(lx_real_us() / 1000000ULL), at = nowt, mt = nowt;
        if (ut) {
            int64_t v[4] = { 0, 0, 0, 0 };              /* utimbuf {a, m} / timeval[2] */
            if (lx_get(v, ut, nr == LX_utime ? 2 * sizeof(int64_t) : sizeof(v))) return -EFAULT;
            if (nr == LX_utime) { at = v[0]; mt = v[1]; } else { at = v[0]; mt = v[2]; }
        }
        return lx_store_times(path, true, at, true, mt);
    }
    case LX_symlink: case LX_symlinkat: {
        /* tmpfs and ext4 store symlinks; FAT32/exFAT: EPERM like vfat. */
        const char *ut = (const char *)a1;
        long dfd = nr == LX_symlinkat ? (long)(int)a2 : LX_AT_FDCWD;
        const char *up = (const char *)(nr == LX_symlinkat ? a3 : a2);
        if (!ut || !up) return -EFAULT;
        char tchk[8];
        if (lx_get(tchk, ut, 1)) return -EFAULT;
        if (!tchk[0]) return -ENOENT;
        if ((r = lx_resolve_f(p, dfd, up, path, sizeof(path), false, false))) return r;
        if (lx_path_kind(path, NULL) || fifo_is(path)) return -EEXIST;
        {
            char par[256]; size_t n = strlen(path);
            memcpy(par, path, n + 1);
            while (n > 1 && par[n - 1] != '/') n--;
            par[n > 1 ? n - 1 : 1] = 0;
            int pk = lx_path_kind(par, NULL);
            if (!pk && !lx_synth_dir(par)) return -ENOENT;
            if (pk == 1) return -ENOTDIR;
        }
        {
            char *tg = (char *)kalloc(VFS_MAX_PATH);
            if (!tg) return -ENOMEM;
            memset(tg, 0, VFS_MAX_PATH);
            if (!strncpy_from_user(tg, ut, VFS_MAX_PATH)) { kfree(tg); return -EFAULT; }
            if (strlen(tg) >= VFS_MAX_PATH - 1) { kfree(tg); return -ENAMETOOLONG; }
            r = vfs_symlink(tg, path);
            kfree(tg);
            /* vfs_symlink returns -errno; -1 is -EPERM (FAT32/exFAT), not EIO. */
            return r < 0 ? r : 0;
        }
    }
    case LX_link: case LX_linkat: {
        long d1 = nr == LX_linkat ? (long)(int)a1 : LX_AT_FDCWD;
        const char *u1 = (const char *)(nr == LX_linkat ? a2 : a1);
        long d2 = nr == LX_linkat ? (long)(int)a3 : LX_AT_FDCWD;
        const char *u2 = (const char *)(nr == LX_linkat ? a4 : a2);
        if (nr == LX_linkat && (a5 & ~(uint64_t)(0x400 | 0x1000))) return -EINVAL;
        if ((r = lx_resolve_f(p, d1, u1, path, sizeof(path), nr == LX_linkat && (a5 & 0x1000),
                              nr == LX_linkat && (a5 & 0x400) /* AT_SYMLINK_FOLLOW */))) return r;
        if ((r = lx_resolve_f(p, d2, u2, path2, sizeof(path2), false, false))) return r;
        int k = lx_path_kind(path, NULL);
        if (!k) return -ENOENT;
        if (k == 2) return -EPERM;                      /* dirs: EPERM on Linux too */
        if (lx_path_kind(path2, NULL) || fifo_is(path2)) return -EEXIST;
        r = vfs_link(path, path2);                      /* FAT32/exFAT: EPERM (vfat) */
        return r < 0 ? r : 0;                           /* -errno; -1 is -EPERM */
    }
    case LX_mknod: case LX_mknodat: {
        long dfd = nr == LX_mknodat ? (long)(int)a1 : LX_AT_FDCWD;
        const char *up = (const char *)(nr == LX_mknodat ? a2 : a1);
        uint32_t mode = (uint32_t)(nr == LX_mknodat ? a3 : a2);
        uint32_t fmt = mode & 0170000;
        if ((r = lx_resolve_f(p, dfd, up, path, sizeof(path), false, false))) return r;
        if (lx_path_kind(path, NULL) || fifo_is(path)) return -EEXIST;
        if (fmt == 0140000 || fmt == 0020000 || fmt == 0060000) return -EPERM; /* sock/chr/blk */
        if (fmt != 0 && fmt != 0100000 && fmt != LX_S_IFIFO) return -EINVAL;
        long fd = sys_open_kpath(path, O_CREAT | O_WRONLY, (mode_t)(mode & 0777));
        if (fd < 0) return fd;
        sys_close((int)fd);
        if (fmt == LX_S_IFIFO) {
            r = fifo_reg(path);
            if (r < 0) { vfs_unlink(path); return r; }
        }
        return 0;
    }
    case LX_flock: {
        if (!lx_fd_ok(p, (long)a1)) return -EBADF;
        int op = (int)a2;
        if ((op & ~4) != 1 && (op & ~4) != 2 && (op & ~4) != 8) return -EINVAL;
        file_descriptor_t *fe = &p->fds[a1];
        if (fe->fd_type != FD_TYPE_VFS || !fe->ref_count) return 0;  /* nothing to share */
        char kp[VFS_MAX_PATH];
        if (vfs_fd_get_path(fe->vfs_fd, kp, sizeof(kp)) < 0) return 0;
        return flk_op(p, fe->ref_count, kp, op);
    }
    case LX_fallocate: {
        long fd = (long)a1;
        if (!lx_fd_ok(p, fd)) return -EBADF;
        int64_t off = (int64_t)a3, len = (int64_t)a4;
        uint8_t ty = p->fds[fd].fd_type;
        if (ty == FD_TYPE_PIPE_READ || ty == FD_TYPE_PIPE_WRITE) return -ESPIPE;
        if (ty != FD_TYPE_VFS) return -ENODEV;
        if (off < 0 || len <= 0) return -EINVAL;
        if (a2 & ~(uint64_t)1) return -EOPNOTSUPP;      /* punch/zero/collapse/insert */
        if (a2 & 1) return 0;                           /* KEEP_SIZE: nothing to reserve */
        if (off + len > 0x7fffffffLL) return -EFBIG;
        int vfd = p->fds[fd].vfs_fd;
        int cur = vfs_seek(vfd, 0, 1);
        int end = vfs_seek(vfd, 0, 2);
        if (cur < 0 || end < 0) return -EIO;
        long res = 0;
        if (off + len > end) {
            uint8_t *z = (uint8_t *)kalloc(4096);
            if (!z) { vfs_seek(vfd, cur, 0); return -ENOMEM; }
            memset(z, 0, 4096);
            int64_t left = off + len - end;
            while (left > 0) {
                int c = left > 4096 ? 4096 : (int)left;
                int w = vfs_write(vfd, z, (uint32_t)c);
                if (w <= 0) { res = w == 0 ? -ENOSPC : (w == -1 ? -EIO : w); break; }
                left -= w;
            }
            kfree(z);
        }
        vfs_seek(vfd, cur, 0);
        return res;
    }
    case LX_memfd_create: {
        extern long memfd_create_k(process_t *p, const char *uname, unsigned flags);
        return memfd_create_k(p, (const char *)a1, (unsigned)a2);
    }
    case LX_fadvise64: {
        if (!lx_fd_ok(p, (long)a1)) return -EBADF;
        uint8_t ty = p->fds[a1].fd_type;
        if (ty == FD_TYPE_PIPE_READ || ty == FD_TYPE_PIPE_WRITE) return -ESPIPE;
        if ((long)a4 < 0 || (long)a4 > 5) return -EINVAL;
        return 0;
    }
    case LX_preadv: case LX_pwritev: case LX_preadv2: case LX_pwritev2: {
        long fd = (long)a1;
        bool wr = nr == LX_pwritev || nr == LX_pwritev2;
        int64_t off = (int64_t)a4;
        if (!lx_fd_ok(p, fd)) return -EBADF;
        if ((nr == LX_preadv2 || nr == LX_pwritev2) && (a6 & ~(uint64_t)0x1f)) return -EOPNOTSUPP;
        if ((nr == LX_preadv2 || nr == LX_pwritev2) && off == -1)
            return lx_rw_vec(p, fd, (const void *)a2, (long)a3, wr);
        if (p->fds[fd].fd_type != FD_TYPE_VFS) return -ESPIPE;
        if (off < 0) return -EINVAL;
        long cur = sys_seek((int)fd, 0, SEEK_CUR);
        if (cur < 0) return cur;
        r = sys_seek((int)fd, (off_t)off, SEEK_SET);
        if (r < 0) return r;
        r = lx_rw_vec(p, fd, (const void *)a2, (long)a3, wr);
        sys_seek((int)fd, (off_t)cur, SEEK_SET);
        return r;
    }
    case LX_getpriority: case LX_setpriority: {
        long which = (long)a1, who = (long)(int)a2;
        if (which < 0 || which > 2) return -EINVAL;
        uint32_t tpid = (uint32_t)p->pid;
        if (which == 0 && who != 0 && who != (long)p->pid) {
            if (who < 0 || sys_kill((pid_t)who, 0) < 0) return -ESRCH;
            tpid = (uint32_t)who;
        } else if (which != 0 && who != 0 && who != (long)(which == 1 ? p->pgid : p->uid)) {
            return -ESRCH;
        }
        if (nr == LX_getpriority) return 20 - lx_nice_get(tpid);   /* kernel ABI: 20 - nice */
        long v = (long)(int)a3;
        if (v < -20) v = -20;
        if (v > 19) v = 19;
        lx_nice_set(tpid, (int)v);
        return 0;
    }
    case LX_sched_setaffinity: {
        long who = (long)(int)a1;
        if (who < 0) return -ESRCH;
        if (who != 0 && who != (long)p->pid && sys_kill((pid_t)who, 0) < 0) return -ESRCH;
        if (a2 == 0 || !a3) return a3 ? -EINVAL : -EFAULT;
        uint8_t b0;
        if (lx_get(&b0, (const void *)a3, 1)) return -EFAULT;
        extern uint64_t cpu_count;
        uint64_t ncpu = cpu_count ? cpu_count : 1;
        uint64_t m = 0;
        lx_get(&m, (const void *)a3, a2 < 8 ? (size_t)a2 : 8);
        if (ncpu < 64) m &= (1ULL << ncpu) - 1;
        if (!m) return -EINVAL;                         /* no online CPU in the mask */
        return 0;                                       /* accepted; scheduler is not pinned */
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
    case LX_msync: {
        if (a1 & (PAGE_SIZE - 1)) return -EINVAL;
        if (a3 & ~7ull) return -EINVAL;           /* MS_ASYNC|MS_INVALIDATE|MS_SYNC */
        extern void shmf_range(uint64_t *pml4, uint64_t va, uint64_t len, bool unmap);
        shmf_range(lx_cur_pml4(), a1, a2, false);
        return 0;
    }
    case LX_madvise:
        return 0;
    case LX_mremap:  return -ENOMEM;            /* callers fall back to copy */

    /* ---- process ---- */
    case LX_exit: case LX_exit_group:
        if (p->linux_clear_child_tid) {
            /* CLONE_CHILD_CLEARTID: zero the tid word so pthread_join's
             * futex wait (polling) sees the thread is gone. */
            uint32_t z = 0;
            if (lx_put((void *)p->linux_clear_child_tid, &z, 4) == 0)
                lx_futex_wake_addr(p->linux_clear_child_tid, 1);
            p->linux_clear_child_tid = 0;
        }
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
        r = sys_exec_env((const char *)a1, (char *const *)a2, (char *const *)a3);
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
    case LX_waitid:  return lx_waitid(p, (long)a1, (long)(int)a2, (void *)a3, (long)a4, (void *)a5);
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
        if (p->sigsuspend_restore) {
            p->blocked_signals = p->sigsuspend_saved_mask;
            p->sigsuspend_restore = false;
        }
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
        fork_release_pending(p);
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
        fork_release_pending(p);
        for (int i = 0; i < 500; i++) {
            if (p->pending_signals & ~p->blocked_signals) break;
            ktime_yield_until(ktime_deadline_ms(5));
        }
        if (p->pending_signals & ~p->blocked_signals) {
            /* Linux: the handler runs under the temporary mask; the old mask
             * comes back at rt_sigreturn.  Restoring it here re-blocked the
             * signal before the return path could deliver it. */
            p->sigsuspend_saved_mask = old;
            p->sigsuspend_restore = true;
        } else {
            p->blocked_signals = old;
        }
        return -EINTR;
    }
    case LX_sigaltstack:
        if (a2) {
            uint64_t ss[3] = { 0, 2 /* SS_DISABLE */, 0 };
            if (lx_put((void *)a2, ss, sizeof(ss))) return -EFAULT;
        }
        return 0;
    case LX_alarm: {
        extern void process_alarm_set(process_t *, uint64_t, uint64_t, uint64_t *, uint64_t *);
        uint64_t old = 0;
        process_alarm_set(p, (uint64_t)(unsigned)a1 * 1000ULL, 0, &old, NULL);
        /* Linux: seconds left, rounded to nearest, at least 1 if armed. */
        return old ? (long)((old + 500) / 1000 ? (old + 500) / 1000 : 1) : 0;
    }
    case LX_setitimer: case LX_getitimer: {
        extern void process_alarm_set(process_t *, uint64_t, uint64_t, uint64_t *, uint64_t *);
        extern void process_alarm_get(process_t *, uint64_t *, uint64_t *);
        /* struct itimerval { timeval it_interval; timeval it_value; } */
        int64_t tv[4];
        uint64_t ov = 0, oi = 0;
        if (a1 != 0)                        /* only ITIMER_REAL is backed */
            return a1 <= 2 ? 0 : -EINVAL;
        void *outp = (nr == LX_getitimer) ? (void *)a2 : (void *)a3;
        if (nr == LX_setitimer) {
            if (!a2) return -EFAULT;
            if (lx_get(tv, (void *)a2, sizeof(tv))) return -EFAULT;
            if (tv[1] < 0 || tv[1] >= 1000000 || tv[3] < 0 || tv[3] >= 1000000 ||
                tv[0] < 0 || tv[2] < 0)
                return -EINVAL;
            uint64_t iv = (uint64_t)tv[0] * 1000ULL + ((uint64_t)tv[1] + 999) / 1000;
            uint64_t vv = (uint64_t)tv[2] * 1000ULL + ((uint64_t)tv[3] + 999) / 1000;
            process_alarm_set(p, vv, iv, &ov, &oi);
        } else {
            process_alarm_get(p, &ov, &oi);
        }
        if (outp) {
            int64_t o[4] = { (int64_t)(oi / 1000), (int64_t)(oi % 1000) * 1000,
                             (int64_t)(ov / 1000), (int64_t)(ov % 1000) * 1000 };
            if (lx_put(outp, o, sizeof(o))) return -EFAULT;
        }
        return 0;
    }

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
            fork_release_pending(p);   /* sleeping parent: let held kids run */
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
        /* struct sysinfo: totalram @32, freeram @40 (bytes, mem_unit=1).
         * freeram is the PMM's free-page count -- lets user tests watch for
         * frame leaks (e.g. munmap/brk churn). */
        uint64_t totalram = total_memory_bytes;
        uint64_t freeram = num_pages_available * (uint64_t)PAGE_SIZE;
        memcpy(si + 32, &totalram, 8);
        memcpy(si + 40, &freeram, 8);
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
        if (a2 < 8 || (a2 & 7)) return -EINVAL;
        if ((long)(int)a1 < 0) return -ESRCH;
        if ((int)a1 != 0 && (long)(int)a1 != (long)p->pid && sys_kill((pid_t)a1, 0) < 0) return -ESRCH;
        uint64_t mask = 1;                      /* unchanged: report one CPU (nproc = 1) */
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
        if ((r = lx_resolve_f(p, dfd, up, path, sizeof(path), (a3 & 0x1000) != 0, /* AT_EMPTY_PATH */
                              !(a3 & LX_AT_SYMLINK_NOFOLLOW))))
            return r;
        lx_stat_t ls;
        if ((r = lx_stat_path_k(path, &ls))) return r;
        /* Linux struct statx (0x100 bytes) from the stat result. */
        uint8_t sx[0x100];
        memset(sx, 0, sizeof(sx));
        uint32_t mask = 0x000007ffu;                    /* STATX_BASIC_STATS */
        uint32_t bsz = (uint32_t)ls.st_blksize, nl = (uint32_t)ls.st_nlink;
        uint32_t uid = ls.st_uid, gid = ls.st_gid;
        uint16_t mode = (uint16_t)ls.st_mode;
        uint64_t ino = ls.st_ino, size = (uint64_t)ls.st_size, blk = (uint64_t)ls.st_blocks;
        int64_t at = ls.st_atime_sec, mt = ls.st_mtime_sec, ct = ls.st_ctime_sec;
        memcpy(sx + 0x00, &mask, 4);
        memcpy(sx + 0x04, &bsz, 4);
        memcpy(sx + 0x10, &nl, 4);
        memcpy(sx + 0x14, &uid, 4);
        memcpy(sx + 0x18, &gid, 4);
        memcpy(sx + 0x1c, &mode, 2);
        memcpy(sx + 0x20, &ino, 8);
        memcpy(sx + 0x28, &size, 8);
        memcpy(sx + 0x30, &blk, 8);
        memcpy(sx + 0x40, &at, 8);                      /* stx_atime */
        memcpy(sx + 0x60, &ct, 8);                      /* stx_ctime */
        memcpy(sx + 0x70, &mt, 8);                      /* stx_mtime */
        uint32_t rmaj = (uint32_t)(ls.st_rdev >> 8), rmin = (uint32_t)(ls.st_rdev & 0xff);
        memcpy(sx + 0x80, &rmaj, 4);
        memcpy(sx + 0x84, &rmin, 4);
        return lx_put((void *)a5, sx, sizeof(sx));
    }
    /* ---- sockets (AF_INET only) ---- */
    case LX_socket:      return lx_sock_socket((long)a1, (long)a2, (long)a3);
    case LX_connect: {
        if (!lx_fd_is_socket(p, (int)a1)) return -ENOTSOCK;
        if (lx_unix_sock(p, a1))
            return ux_connect(p, (int)a1, lx_unix_sock(p, a1), (const void *)a2, (uint32_t)a3);
        if (a3 < sizeof(sockaddr_in_t)) return -EINVAL;
        uint16_t fam = 0;
        if (lx_get(&fam, (const void *)a2, sizeof(fam))) return -EFAULT;
        if (fam != AF_INET) return -EAFNOSUPPORT;
        return sys_connect((int)a1, (sockaddr_in_t *)a2, sizeof(sockaddr_in_t));
    }
    case LX_bind: {
        if (!lx_fd_is_socket(p, (int)a1)) return -ENOTSOCK;
        if (lx_unix_sock(p, a1)) {
            char nm[112];
            int nl = 0;
            long r = lx_unix_parse_addr(p, (const void *)a2, (uint32_t)a3, nm, &nl);
            if (r < 0) return r;
            if (nl > 0 && nm[0] && lx_path_kind(nm, NULL)) return -EADDRINUSE;
            return ux_bind(p, lx_unix_sock(p, a1), (const void *)a2, (uint32_t)a3);
        }
        uint16_t fam = 0;
        if (lx_get(&fam, (const void *)a2, sizeof(fam))) return -EFAULT;
        if (fam != AF_INET) return -EAFNOSUPPORT;
        return sys_bind((int)a1, (const sockaddr_in_t *)a2, (uint32_t)a3);
    }
    case LX_listen:
        if (lx_unix_sock(p, a1)) return ux_listen(lx_unix_sock(p, a1), (int)a2);
        return sys_listen((int)a1, (int)a2);
    case LX_accept: case LX_accept4: {
        if (lx_unix_sock(p, a1))
            return ux_accept(p, (int)a1, lx_unix_sock(p, a1), (void *)a2, (uint32_t *)a3,
                             nr == LX_accept4 ? (long)a4 : 0);
        long fd = sys_accept((int)a1, (sockaddr_in_t *)a2, (uint32_t *)a3);
        if (fd >= 0 && nr == LX_accept4) {
            if (a4 & 04000) p->fds[fd].flags |= O_NONBLOCK;
            if (a4 & 02000000) p->fds[fd].cloexec = true;
        }
        return fd;
    }
    case LX_sendto:      return lx_sock_sendto((int)a1, (const void *)a2, (size_t)a3, (int)a4,
                                               (const void *)a5, (uint32_t)a6);
    case LX_recvfrom:    return lx_sock_recvfrom((int)a1, (void *)a2, (size_t)a3, (int)a4,
                                                 (void *)a5, (uint32_t *)a6);
    case LX_sendmsg: case LX_recvmsg: {
        /* struct msghdr { name, namelen, iov, iovlen, control, controllen, flags } */
        struct { uint64_t name; uint32_t namelen, pad; uint64_t iov, iovlen, control, controllen; int flags; } m;
        if (lx_get(&m, (const void *)a2, sizeof(m))) return -EFAULT;
        if (lx_unix_sock(p, a1)) {
            if (m.iovlen > 1024) return -EMSGSIZE;
            int niov = (int)m.iovlen;
            lx_ux_iov_t kiov[64];
            lx_ux_iov_t *iv = kiov;
            if (niov > 64) {
                iv = (lx_ux_iov_t *)kalloc(sizeof(lx_ux_iov_t) * (size_t)niov);
                if (!iv) return -ENOMEM;
            }
            long r = 0;
            if (niov && lx_get(iv, (const void *)m.iov, sizeof(lx_ux_iov_t) * (size_t)niov)) r = -EFAULT;
            if (!r) {
                if (nr == LX_sendmsg) {
                    r = ux_sendmsg(p, (int)a1, lx_unix_sock(p, a1), iv, niov,
                                   (const void *)m.name, m.namelen,
                                   (const void *)m.control, m.controllen, (int)a3);
                } else {
                    uint64_t octl = 0;
                    int oflags = 0;
                    r = ux_recvmsg(p, (int)a1, lx_unix_sock(p, a1), iv, niov,
                                   m.name ? (void *)m.name : NULL,
                                   m.name ? (uint32_t *)((uint8_t *)a2 + 8) : NULL,
                                   (void *)m.control, m.controllen, &octl, &oflags, (int)a3);
                    if (r >= 0) {
                        lx_put((uint8_t *)a2 + 40, &octl, sizeof(octl));
                        lx_put((uint8_t *)a2 + 48, &oflags, sizeof(oflags));
                    }
                }
            }
            if (iv != kiov) kfree(iv);
            return r;
        }
        int flags = (int)a3 | m.flags;
        if (m.iovlen == 0) {
            /* Still must consume one datagram on recv so poll(POLLIN) clears. */
            if (nr == LX_recvmsg) {
                uint8_t bitbucket[1];
                long r = lx_sock_recvfrom((int)a1, bitbucket, 0, flags, NULL, NULL);
                return r < 0 ? r : 0;
            }
            return 0;
        }
        long total = 0;
        for (uint64_t i = 0; i < m.iovlen && i < 64; i++) {
            lx_iovec_t v;
            if (lx_get(&v, (const lx_iovec_t *)m.iov + i, sizeof(v))) return total ? total : -EFAULT;
            if (!v.iov_len) continue;
            long r;
            if (nr == LX_sendmsg)
                r = lx_sock_sendto((int)a1, (const void *)v.iov_base, (size_t)v.iov_len, flags,
                                   m.name ? (const void *)m.name : NULL, m.namelen);
            else
                r = lx_sock_recvfrom((int)a1, (void *)v.iov_base, (size_t)v.iov_len, flags,
                                     m.name ? (void *)m.name : NULL,
                                     m.name ? (uint32_t *)((uint8_t *)a2 + 8) : NULL);
            if (r < 0) return total ? total : r;
            total += r;
            if ((uint64_t)r < v.iov_len || nr == LX_recvmsg) break;
        }
        /* All iovecs empty: still consume one datagram (POSIX). */
        if (nr == LX_recvmsg && total == 0) {
            uint8_t bitbucket[1];
            long r = lx_sock_recvfrom((int)a1, bitbucket, 0, flags, NULL, NULL);
            if (r < 0) return r;
        }
        return total;
    }
    case LX_shutdown:    return lx_sock_shutdown((int)a1, (int)a2);
    case LX_getsockname: return lx_sock_name((int)a1, (void *)a2, (uint32_t *)a3, false);
    case LX_getpeername: return lx_sock_name((int)a1, (void *)a2, (uint32_t *)a3, true);
    case LX_setsockopt:  return lx_sock_setsockopt((int)a1, (int)a2, (int)a3, (const void *)a4, (uint32_t)a5);
    case LX_getsockopt:  return lx_sock_getsockopt((int)a1, (int)a2, (int)a3, (void *)a4, (uint32_t *)a5);
    case LX_socketpair:
        if (a1 != 1 /* AF_UNIX */) return -EAFNOSUPPORT;
        if (a3 != 0) return -EPROTONOSUPPORT;
        return ux_socketpair(p, (long)a2, (int *)a4);
    case LX_eventfd:     return lx_eventfd_create(p, (uint32_t)a1, 0);
    case LX_eventfd2:    return lx_eventfd_create(p, (uint32_t)a1, (long)a2);
    case 253 /* inotify_init */:      return lx_inotify_init(p, 0);
    case 294 /* inotify_init1 */:     return lx_inotify_init(p, (long)(int)a1);
    case 254 /* inotify_add_watch */: return lx_inotify_add(p, (long)(int)a1, (const char *)a2, (uint32_t)a3);
    case 255 /* inotify_rm_watch */:  return lx_inotify_rm(p, (long)(int)a1, (long)(int)a2);
    case LX_timerfd_create:  return lx_timerfd_create(p, (long)(int)a1, (long)(int)a2);
    case LX_timerfd_settime: return lx_timerfd_settime(p, (long)(int)a1, (long)(int)a2, (const void *)a3, (void *)a4);
    case LX_timerfd_gettime: return lx_timerfd_gettime(p, (long)(int)a1, (void *)a2);
    case LX_signalfd:        return lx_signalfd(p, (long)(int)a1, (const void *)a2, (size_t)a3, 0);
    case LX_signalfd4:       return lx_signalfd(p, (long)(int)a1, (const void *)a2, (size_t)a3, (long)(int)a4);
    case LX_rt_sigtimedwait: return lx_sigtimedwait(p, (const void *)a1, (void *)a2, (const void *)a3, (size_t)a4);
    case LX_epoll_create:  return (long)(int)a1 <= 0 ? -EINVAL : lx_epoll_create(p, 0);
    case LX_epoll_create1: return lx_epoll_create(p, (long)(int)a1);
    case LX_epoll_ctl:     return lx_epoll_ctl(p, (long)(int)a1, (long)(int)a2, (long)(int)a3, (const void *)a4);
    case LX_epoll_wait:
    case LX_epoll_pwait:   /* sigmask ignored, as for ppoll/pselect6 */
        return lx_epoll_wait(p, (long)(int)a1, (void *)a2, (long)(int)a3, (int)a4);
    case LX_epoll_pwait2: {
        int ms = -1;
        if (a4) {
            lx_timespec_t ts;
            if (lx_get(&ts, (void *)a4, sizeof(ts))) return -EFAULT;
            if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000) return -EINVAL;
            ms = (int)(ts.tv_sec * 1000 + (ts.tv_nsec + 999999) / 1000000);
        }
        return lx_epoll_wait(p, (long)(int)a1, (void *)a2, (long)(int)a3, ms);
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
        long fr_ = lx_futex(p, a1, (long)a2, (uint32_t)a3, a4, a5, (uint32_t)a6);
        if (fr_ == -ENOSYS) return lx_enosys(p, nr);
        return fr_;
    }

    case 1000: {    /* inDevOS private: network helper for /bin/dhcp.
                     * a1: 1 = run Zeus "dhcp", 2 = "ddhcp" (QEMU user-net
                     * defaults) on the kernel main loop; 3 = status:
                     * bits 0-31 IPv4 (network order bytes), bit 32 lease,
                     * bits 40-63 completed-request counter. */
        extern volatile int lx_net_req;
        extern volatile uint32_t lx_net_gen;
        extern void net_get_ip_address(uint8_t *ip);
        extern int dhcp_has_lease(void);
        if (a1 == 1 || a1 == 2) { lx_net_req = (int)a1; return 0; }
        if (a1 == 3) {
            uint8_t ip[4];
            net_get_ip_address(ip);
            uint64_t v = (uint64_t)ip[0] | ((uint64_t)ip[1] << 8) |
                         ((uint64_t)ip[2] << 16) | ((uint64_t)ip[3] << 24);
            if (dhcp_has_lease()) v |= 1ULL << 32;
            v |= ((uint64_t)(lx_net_gen & 0xffffff)) << 40;
            return (long)v;
        }
        return -EINVAL;
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

    if (lx_strace)
        serial_printf("lxt: pid %u tid %u nr %llu (%llx, %llx, %llx, %llx)\n", p ? p->pid : 0,
                      (unsigned)(scheduler_get_current_thread() ? scheduler_get_current_thread()->id : 0),
                      (unsigned long long)f->nr, (unsigned long long)f->a1, (unsigned long long)f->a2,
                      (unsigned long long)f->a3, (unsigned long long)f->a4);
    long r = lx_dispatch(f);
    if (lx_strace)
        serial_printf("lxt: pid %u nr %llu -> %ld\n", p ? p->pid : 0, (unsigned long long)f->nr, r);

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

    /* Remote kill (process_request_kill): this is the safe point -- the
     * syscall has unwound (a blocking one returned -EINTR) and released its
     * locks.  Checked even when delivery was skipped above (fork/clone return,
     * or already inside a handler). */
    if (p && p->is_running && __atomic_load_n(&p->kill_sig, __ATOMIC_ACQUIRE) &&
        process_is_self(p)) {
        p->exit_status = 128 + p->kill_sig;
        p->is_running = false;
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
