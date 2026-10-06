#ifndef SYSCALLS_H
#define SYSCALLS_H

// User-space syscall interface
// This header should be included by user programs

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include "indevos_syscall_numbers.h"
#include "indevos_stat.h"
#include "indevos_errno.h"
#include "indevos_gui.h"
#include "indevos_dirent.h"
#include "indevos_rtc.h"

/* Kernel returns -(errno) for many failures; libc-style errno for wrappers. */
#ifndef INDEVOS_NO_ERRNO
static int indevos_errno_var;
static inline int *__errno_location(void) { return &indevos_errno_var; }
#define errno (*__errno_location())
static inline long indevos_syscall_result(long r) {
    if (r >= -4095 && r < 0) {
        errno = (int)-r;
        return -1;
    }
    errno = 0;
    return r;
}
#else
static inline long indevos_syscall_result(long r) { return r; }
#endif

typedef uint32_t mode_t;

// Signal definitions
#define SIGHUP    1   // Hangup
#define SIGINT    2   // Interrupt (Ctrl+C)
#define SIGQUIT   3   // Quit
#define SIGILL    4   // Illegal instruction
#define SIGTRAP   5   // Trap
#define SIGABRT   6   // Abort
#define SIGBUS    7   // Bus error
#define SIGFPE    8   // Floating point exception
#define SIGKILL   9   // Kill (cannot be caught)
#define SIGUSR1   10  // User-defined signal 1
#define SIGSEGV   11  // Segmentation violation
#define SIGUSR2   12  // User-defined signal 2
#define SIGPIPE   13  // Broken pipe
#define SIGALRM   14  // Alarm clock
#define SIGTERM   15  // Termination
#define SIGCHLD   17  // Child status changed
#define SIGCONT   18  // Continue
#define SIGSTOP   19  // Stop (cannot be caught)
#define SIGTSTP   20  // Keyboard stop
#define SIGTTIN   21  // Background read from TTY
#define SIGTTOU   22  // Background write to TTY

// Signal handler types
typedef void (*sighandler_t)(int);
#define SIG_DFL ((sighandler_t)0)  // Default action
#define SIG_IGN ((sighandler_t)1)  // Ignore signal

// File descriptor constants
#define STDIN_FILENO    0
#define STDOUT_FILENO   1
#define STDERR_FILENO   2

// Open flags
#define O_RDONLY        0x00
#define O_WRONLY        0x01
#define O_RDWR          0x02
#define O_CREAT         0x04
#define O_TRUNC         0x08
#define O_APPEND        0x10
#define O_NONBLOCK      0x20

#define FD_CLOEXEC      1
#define F_DUPFD         0
#define F_GETFD         1
#define F_SETFD         2
#define F_GETFL         3
#define F_SETFL         4

#define PROT_READ       0x1
#define PROT_WRITE      0x2
#define PROT_EXEC       0x4
#define MAP_PRIVATE     0x02
#define MAP_ANONYMOUS   0x20
#define MAP_FAILED      ((void *)(uint64_t)0xFFFFFFFFFFFFFFFFull)

#define CLOCK_REALTIME   0
#define CLOCK_MONOTONIC  1

#define SIG_BLOCK       0
#define SIG_UNBLOCK     1
#define SIG_SETMASK     2

#define S_IFMT          0170000
#define S_IFREG         0100000
#define S_IFDIR         0040000
#define S_IFCHR         0020000

// Types
typedef long off_t;
typedef int pid_t;

typedef struct {
    int64_t tv_sec;
    int64_t tv_usec;
} timeval_ind_t;

typedef struct {
    int64_t tv_sec;
    long tv_nsec;
} timespec_ind_t;

typedef struct {
    uint64_t sa_handler;
    uint64_t sa_mask;
    uint32_t sa_flags;
    uint32_t sa_resv;
} sigaction_ind_t;

typedef struct {
    int fd;
    int16_t events;
    int16_t revents;
} pollfd_t;

// Low-level syscall function
static inline uint64_t syscall(uint64_t num, uint64_t arg1, uint64_t arg2, 
                              uint64_t arg3, uint64_t arg4, uint64_t arg5) {
    uint64_t result;
    register uint64_t r10_reg __asm__("r10") = arg4;
    register uint64_t r8_reg __asm__("r8") = arg5;
    __asm__ volatile (
        "int $0x80"
        : "=a" (result)
        : "a" (num), "D" (arg1), "S" (arg2), "d" (arg3), "r" (r10_reg), "r" (r8_reg)
        : "memory"
    );
    return result;
}

// Convenience functions that wrap the syscalls

/* Defined in apps/appio.c (one definition per program); this is a plain
 * declaration, not `static inline`, or every translation unit would get its own
 * copy and nothing would ever be flushed.  Declared here so exit()/read() and
 * the printf helpers below can flush pending output. */
#ifdef INDEVOS_UNBUFFERED_STDOUT
static inline void indevos_flush_stdout(void) { }
#else
void indevos_flush_stdout(void);
#endif

static inline void exit(int status) {
    indevos_flush_stdout();
    syscall(SYS_EXIT, status, 0, 0, 0, 0);
}

static inline int read(int fd, void *buf, size_t count) {
    /* Do not read while output is still buffered: a prompt must be visible
     * before the program waits for input. */
    if (fd == STDIN_FILENO) indevos_flush_stdout();
    return (int)indevos_syscall_result((long)syscall(SYS_READ, fd, (uint64_t)buf, count, 0, 0));
}

static inline int write(int fd, const void *buf, size_t count) {
    return (int)indevos_syscall_result((long)syscall(SYS_WRITE, fd, (uint64_t)buf, count, 0, 0));
}

static inline int open(const char *pathname, int flags) {
    return (int)indevos_syscall_result(
        (long)syscall(SYS_OPEN, (uint64_t)pathname, (uint64_t)flags, 0, 0, 0));
}
/** Pass explicit mode when using O_CREAT (umask applied in kernel). */
static inline int open3(const char *pathname, int flags, mode_t mode) {
    return (int)indevos_syscall_result(
        (long)syscall(SYS_OPEN, (uint64_t)pathname, (uint64_t)flags, (uint64_t)mode, 0, 0));
}

static inline int close(int fd) {
    return (int)indevos_syscall_result((long)syscall(SYS_CLOSE, fd, 0, 0, 0, 0));
}

static inline off_t lseek(int fd, off_t offset, int whence) {
    return (off_t)syscall(SYS_SEEK, fd, offset, whence, 0, 0);
}

static inline void* sbrk(intptr_t increment) {
    static void *current_break = NULL;
    if (current_break == NULL) {
        current_break = (void*)syscall(SYS_BRK, 0, 0, 0, 0, 0);
    }
    void *old_break = current_break;
    current_break = (void*)syscall(SYS_BRK, (uint64_t)current_break + increment, 0, 0, 0, 0);
    return old_break;
}

/* Apps with a custom allocator (e.g. herb engine) define INDEVOS_NO_BUILTIN_MALLOC
 * and provide malloc/free themselves before including this header. */
#ifndef INDEVOS_NO_BUILTIN_MALLOC
static inline void* malloc(size_t size) {
    // Simple malloc using sbrk
    return sbrk(size);
}

static inline void free(void *ptr) {
    // Simple free - no-op for now
    (void)ptr;
}
#endif

static inline pid_t getpid(void) {
    return (pid_t)syscall(SYS_GETPID, 0, 0, 0, 0, 0);
}

static inline pid_t fork(void) {
    return (pid_t)syscall(SYS_FORK, 0, 0, 0, 0, 0);
}

static inline int execv(const char *path, char *const argv[]) {
    return (int)syscall(SYS_EXEC, (uint64_t)path, (uint64_t)argv, 0, 0, 0);
}

static inline pid_t wait(int *status) {
    return (pid_t)syscall(SYS_WAIT, (uint64_t)status, 0, 0, 0, 0);
}

static inline sighandler_t signal(int signum, sighandler_t handler) {
    return (sighandler_t)syscall(SYS_SIGNAL, signum, (uint64_t)handler, 0, 0, 0);
}

static inline int kill(pid_t pid, int signum) {
    return (int)syscall(SYS_KILL, pid, signum, 0, 0, 0);
}

static inline unsigned int sleep(unsigned int seconds) {
    return (unsigned int)syscall(SYS_SLEEP, seconds, 0, 0, 0, 0);
}

static inline uint64_t time(uint64_t *tloc) {
    uint64_t t = syscall(SYS_TIME, 0, 0, 0, 0, 0);
    if (tloc) *tloc = t;
    return t;
}

static inline int print(const char *message) {
    return (int)syscall(SYS_PRINT, (uint64_t)message, 0, 0, 0, 0);
}

static inline int getchar(void) {
    indevos_flush_stdout();   /* show any pending prompt before blocking */
    return (int)syscall(SYS_GETCHAR, 0, 0, 0, 0, 0);
}

static inline int mkdir(const char *pathname) {
    return (int)syscall(SYS_MKDIR, (uint64_t)pathname, 0, 0, 0, 0);
}

static inline int rmdir(const char *pathname) {
    return (int)syscall(SYS_RMDIR, (uint64_t)pathname, 0, 0, 0, 0);
}

static inline int unlink(const char *pathname) {
    return (int)syscall(SYS_UNLINK, (uint64_t)pathname, 0, 0, 0, 0);
}

static inline int stat(const char *pathname, stat_t *statbuf) {
    return (int)indevos_syscall_result(
        (long)syscall(SYS_STAT, (uint64_t)pathname, (uint64_t)statbuf, 0, 0, 0));
}

static inline char* getcwd(char *buf, size_t size) {
    return (char*)syscall(SYS_GETCWD, (uint64_t)buf, size, 0, 0, 0);
}

static inline int chdir(const char *path) {
    return (int)syscall(SYS_CHDIR, (uint64_t)path, 0, 0, 0, 0);
}

static inline int mount(const char *device, const char *mountpoint, const char *fstype) {
    return (int)syscall(SYS_MOUNT, (uint64_t)device, (uint64_t)mountpoint, (uint64_t)fstype, 0, 0);
}

static inline int unmount(const char *mountpoint) {
    return (int)syscall(SYS_UNMOUNT, (uint64_t)mountpoint, 0, 0, 0, 0);
}

/* Directory iteration.  SYS_READDIR fills an array of indevos_dirent_t and
 * returns the number of entries written (0 at end of directory). */
static inline int readdir(int fd, indevos_dirent_t *entries, size_t max_entries) {
    return (int)syscall(SYS_READDIR, (uint64_t)fd, (uint64_t)entries, (uint64_t)max_entries, 0, 0);
}

/* Small string helpers user programs kept re-implementing. */
static inline char *strchr(const char *s, int c) {
    char ch = (char)c;
    while (*s) { if (*s == ch) return (char *)s; s++; }
    return (ch == '\0') ? (char *)s : (char *)0;
}

static inline int atoi(const char *s) {
    int neg = 0, v = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { neg = 1; s++; } else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

static inline int setuid(uint32_t uid) {
    return (int)syscall(SYS_SETUID, uid, 0, 0, 0, 0);
}

static inline int setgid(uint32_t gid) {
    return (int)syscall(SYS_SETGID, gid, 0, 0, 0, 0);
}

static inline uint32_t getuid(void) {
    return (uint32_t)syscall(SYS_GETUID, 0, 0, 0, 0, 0);
}

static inline uint32_t getgid(void) {
    return (uint32_t)syscall(SYS_GETGID, 0, 0, 0, 0, 0);
}

// Standard library-like functions
//
// printf used to ignore every argument and just print the format string, so no
// user program could format numbers at all.  The formatter below is the real
// thing: %d %i %u %x %X %p %c %s %% with width/zero-pad/left-align and the
// l/ll/z/h length modifiers.
static inline void ind_out(char *b, size_t cap, size_t *pos, char c) {
    if (*pos + 1 < cap) b[*pos] = c;
    (*pos)++;
}

static inline void ind_out_pad(char *b, size_t cap, size_t *pos, char c, int n) {
    while (n-- > 0) ind_out(b, cap, pos, c);
}

static inline int indevos_vsnprintf(char *out, size_t cap, const char *fmt, va_list ap) {
    size_t pos = 0;
    if (cap == 0) return 0;

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { ind_out(out, cap, &pos, *p); continue; }
        p++;
        if (*p == '%') { ind_out(out, cap, &pos, '%'); continue; }

        int left = 0, zero = 0, width = 0, long_mod = 0;
        if (*p == '-') { left = 1; p++; }
        if (*p == '0') { zero = 1; p++; }
        while (*p >= '0' && *p <= '9') { width = width * 10 + (*p - '0'); p++; }
        if (*p == 'l') { long_mod = 1; p++; if (*p == 'l') { long_mod = 2; p++; } }
        else if (*p == 'z') { long_mod = 2; p++; }
        else if (*p == 'h') { p++; if (*p == 'h') p++; }

        char tmp[32];
        int len = 0;
        switch (*p) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            len = 0;
            while (s[len]) len++;
            if (!left) ind_out_pad(out, cap, &pos, ' ', width - len);
            for (int i = 0; i < len; i++) ind_out(out, cap, &pos, s[i]);
            if (left) ind_out_pad(out, cap, &pos, ' ', width - len);
            continue;
        }
        case 'c':
            if (!left) ind_out_pad(out, cap, &pos, ' ', width - 1);
            ind_out(out, cap, &pos, (char)va_arg(ap, int));
            if (left) ind_out_pad(out, cap, &pos, ' ', width - 1);
            continue;
        case 'd': case 'i': {
            long long v = long_mod ? va_arg(ap, long long) : (long long)va_arg(ap, int);
            unsigned long long uv = (v < 0) ? (unsigned long long)(-v) : (unsigned long long)v;
            if (v < 0) tmp[len++] = '-';
            do { tmp[len++] = (char)('0' + (uv % 10)); uv /= 10; } while (uv);
            break;
        }
        case 'u': case 'x': case 'X': {
            unsigned long long v = long_mod ? va_arg(ap, unsigned long long)
                                            : (unsigned long long)va_arg(ap, unsigned int);
            int base = (*p == 'u') ? 10 : 16;
            const char *dig = (*p == 'X') ? "0123456789ABCDEF" : "0123456789abcdef";
            do { tmp[len++] = dig[v % base]; v /= base; } while (v);
            break;
        }
        case 'p': {
            unsigned long long v = (unsigned long long)va_arg(ap, void *);
            tmp[len++] = '0'; tmp[len++] = 'x';
            do { tmp[len++] = "0123456789abcdef"[v % 16]; v /= 16; } while (v);
            left = 1;
            break;
        }
        default:
            ind_out(out, cap, &pos, '%');
            ind_out(out, cap, &pos, *p);
            continue;
        }
        /* numeric: tmp holds digits little-endian, optional leading '-' at [0] */
        int neg = (tmp[0] == '-');
        int digits = len - neg;
        int pad = width - len;
        if (!left && pad > 0) ind_out_pad(out, cap, &pos, zero ? '0' : ' ', pad);
        if (neg) ind_out(out, cap, &pos, '-');
        for (int i = neg + digits - 1; i >= neg; i--) ind_out(out, cap, &pos, tmp[i]);
        if (left && pad > 0) ind_out_pad(out, cap, &pos, ' ', pad);
    }

    if (pos < cap) out[pos] = '\0'; else out[cap - 1] = '\0';
    return (int)pos;
}

static inline int snprintf(char *out, size_t cap, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = indevos_vsnprintf(out, cap, fmt, ap);
    va_end(ap);
    return n;
}

/* ---- Line-buffered stdout ------------------------------------------------
 *
 * Every write() syscall into this kernel costs milliseconds (profiler: ~5.6 ms
 * average for a console write), so a program that prints one line per call
 * (`ls` = 54 lines) spent most of its run time in syscall overhead.  Output
 * ending in a newline is therefore buffered and handed over in one syscall per
 * buffer-full; a partial line (a prompt, a "\r" progress update) is flushed
 * immediately so interactive programs still behave.
 *
 * The buffer itself is defined once per program in apps/appio.c: these helpers
 * are inline, so a `static` buffer here would give every .c file its own copy
 * and exit() would flush an empty one.  Programs that define
 * INDEVOS_UNBUFFERED_STDOUT (or their own printf, like DOOM) write directly. */
#ifdef INDEVOS_UNBUFFERED_STDOUT
static inline void indevos_stdout_write(const char *s, size_t n) {
    if (n > 0) write(STDOUT_FILENO, s, n);
}
#else
#define INDEVOS_STDOUT_BUFSZ 4096
extern char   indevos_stdout_buf[INDEVOS_STDOUT_BUFSZ];
extern size_t indevos_stdout_len;

static inline void indevos_stdout_write(const char *s, size_t n) {
    /* A line that is not terminated cannot be buffered without risking that it
     * never appears (a prompt waiting for input). */
    if (n == 0) return;
    if (s[n - 1] != '\n' || n >= INDEVOS_STDOUT_BUFSZ) {
        indevos_flush_stdout();
        write(STDOUT_FILENO, s, n);
        return;
    }

    if (indevos_stdout_len + n > INDEVOS_STDOUT_BUFSZ)
        indevos_flush_stdout();

    for (size_t i = 0; i < n; i++)
        indevos_stdout_buf[indevos_stdout_len++] = s[i];
}
#endif

static inline int printf(const char *format, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, format);
    int n = indevos_vsnprintf(buf, sizeof(buf), format, ap);
    va_end(ap);
    if (n > 0)
        indevos_stdout_write(buf, (size_t)n);
    return n;
}
static inline int puts(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    indevos_stdout_write(s, len);
    indevos_stdout_write("\n", 1);
    return (int)(len + 1);
}

// String functions
static inline size_t strlen(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

static inline char* strcpy(char *dest, const char *src) {
    char *d = dest;
    while ((*d++ = *src++));
    return dest;
}

static inline int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(unsigned char*)s1 - *(unsigned char*)s2;
}

static inline void* memset(void *s, int c, size_t n) {
    unsigned char *p = s;
    while (n--) *p++ = c;
    return s;
}

static inline void* memcpy(void *dest, const void *src, size_t n) {
    char *d = dest;
    const char *s = src;
    while (n--) *d++ = *s++;
    return dest;
}

// SDL wrapper functions
static inline int SDL_Init(uint32_t flags) {
    return (int)syscall(SYS_SDL_INIT, flags, 0, 0, 0, 0);
}

static inline void SDL_Quit(void) {
    syscall(SYS_SDL_QUIT, 0, 0, 0, 0, 0);
}

static inline void* SDL_CreateWindow(const char* title, int x, int y, int w, int h, uint32_t flags) {
    return (void*)syscall(SYS_SDL_CREATE_WINDOW, (uint64_t)title, x, y, w, h);
}

static inline void SDL_DestroyWindow(void* window) {
    syscall(SYS_SDL_DESTROY_WINDOW, (uint64_t)window, 0, 0, 0, 0);
}

static inline void* SDL_CreateRenderer(void* window, int index, uint32_t flags) {
    return (void*)syscall(SYS_SDL_CREATE_RENDERER, (uint64_t)window, index, flags, 0, 0);
}

static inline void SDL_DestroyRenderer(void* renderer) {
    syscall(SYS_SDL_DESTROY_RENDERER, (uint64_t)renderer, 0, 0, 0, 0);
}

static inline int SDL_SetRenderDrawColor(void* renderer, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return (int)syscall(SYS_SDL_SET_DRAW_COLOR, (uint64_t)renderer, r, g, b, a);
}

static inline int SDL_RenderClear(void* renderer) {
    return (int)syscall(SYS_SDL_RENDER_CLEAR, (uint64_t)renderer, 0, 0, 0, 0);
}

static inline void SDL_RenderPresent(void* renderer) {
    syscall(SYS_SDL_RENDER_PRESENT, (uint64_t)renderer, 0, 0, 0, 0);
}

static inline int SDL_PollEvent(void* event) {
    return (int)syscall(SYS_SDL_POLL_EVENT, (uint64_t)event, 0, 0, 0, 0);
}

static inline uint32_t SDL_GetTicks(void) {
    return (uint32_t)syscall(SYS_SDL_GET_TICKS, 0, 0, 0, 0, 0);
}

static inline void SDL_Delay(uint32_t ms) {
    syscall(SYS_SDL_DELAY, ms, 0, 0, 0, 0);
}

static inline int SDL_RenderDrawRect(void* renderer, const void* rect) {
    return (int)syscall(SYS_SDL_RENDER_DRAW_RECT, (uint64_t)renderer, (uint64_t)rect, 0, 0, 0);
}

static inline int SDL_RenderFillRect(void* renderer, const void* rect) {
    return (int)syscall(SYS_SDL_RENDER_FILL_RECT, (uint64_t)renderer, (uint64_t)rect, 0, 0, 0);
}

static inline const char* SDL_GetError(void) {
    return (const char*)syscall(SYS_SDL_GET_ERROR, 0, 0, 0, 0, 0);
}

/** Copy WIDTH*HEIGHT RGBA8888 pixels from user memory into the SDL window surface, then use SDL_RenderPresent. */
static inline int SDL_CopyPixelsFromUser(void *renderer, const void *pixels, uint32_t width, uint32_t height) {
    return (int)syscall(SYS_SDL_COPY_PIXELS, (uint64_t)renderer, (uint64_t)pixels, width, height, 0);
}

/** Copy sdl_keyboard_state[0..n) into user buffer (n <= 1024). */
static inline int indevos_get_keyboard_state(uint8_t *buf, size_t n) {
    return (int)syscall(SYS_SDL_GET_KEYBOARD_STATE, (uint64_t)buf, (uint64_t)n, 0, 0, 0);
}

/** Returns accumulated PS/2 dx,dy since last call; upper 32 bits = dx, lower 32 = dy (signed). */
static inline uint64_t indevos_get_mouse_rel_accum(void) {
    return syscall(SYS_SDL_GET_MOUSE_REL_ACCUM, 0, 0, 0, 0, 0);
}

/** Current mouse button bitmask (bit0=left, bit1=right, bit2=middle). */
static inline uint64_t indevos_get_mouse_buttons(void) {
    return syscall(SYS_SDL_GET_MOUSE_BUTTONS, 0, 0, 0, 0, 0);
}

/** Returns display size packed as (width << 32) | height. */
static inline uint64_t indevos_get_display_size(void) {
    return syscall(SYS_SDL_GET_DISPLAY_SIZE, 0, 0, 0, 0, 0);
}

typedef struct {
    int32_t active;
    int32_t content_x;
    int32_t content_y;
    int32_t content_w;
    int32_t content_h;
} indevos_desktop_embed_info_t;

static inline int indevos_desktop_embed_query(indevos_desktop_embed_info_t *info) {
    return (int)syscall(SYS_DESKTOP_EMBED_INFO, (uint64_t)info, 0, 0, 0, 0);
}

static inline int indevos_desktop_composite(const void *rgba, uint32_t width, uint32_t height) {
    return (int)syscall(SYS_DESKTOP_COMPOSITE, (uint64_t)rgba, width, height, 0, 0);
}

/** Poll one GUI event (non-blocking). Returns 1 if ev filled, 0 if empty, negative errno. */
static inline int indevos_desktop_poll_event(gui_event_t *ev) {
    return (int)syscall(SYS_DESKTOP_POLL_EVENT, (uint64_t)ev, 0, 0, 0, 0);
}

// BSD socket types and wrappers (must appear after syscall() is defined)
#define AF_INET         2
#define SOCK_STREAM     1
#define SOCK_DGRAM      2
#define IPPROTO_TCP     6
#define IPPROTO_UDP     17

typedef struct {
    uint16_t sin_family;
    uint16_t sin_port;      /* big-endian (use htons) */
    uint32_t sin_addr;      /* packed little-endian: ip[0] in lowest byte */
    uint8_t  sin_zero[8];
} sockaddr_in_t;

static inline uint16_t htons(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
static inline uint32_t htonl(uint32_t v) {
    return ((v & 0xFF000000u) >> 24) | ((v & 0x00FF0000u) >>  8)
         | ((v & 0x0000FF00u) <<  8) | ((v & 0x000000FFu) << 24);
}

static inline int socket(int domain, int type, int protocol) {
    return (int)indevos_syscall_result(
        (long)syscall(SYS_SOCKET, (uint64_t)domain, (uint64_t)type, (uint64_t)protocol, 0, 0));
}
static inline int connect(int fd, const sockaddr_in_t *addr, uint32_t addrlen) {
    return (int)indevos_syscall_result(
        (long)syscall(SYS_CONNECT, (uint64_t)fd, (uint64_t)addr, (uint64_t)addrlen, 0, 0));
}
static inline int send(int fd, const void *buf, uint32_t len, int flags) {
    return (int)indevos_syscall_result(
        (long)syscall(SYS_SEND, (uint64_t)fd, (uint64_t)buf, (uint64_t)len, (uint64_t)flags, 0));
}
static inline int recv(int fd, void *buf, uint32_t len, int flags) {
    return (int)indevos_syscall_result(
        (long)syscall(SYS_RECV, (uint64_t)fd, (uint64_t)buf, (uint64_t)len, (uint64_t)flags, 0));
}
static inline int sendto(int fd, const void *buf, uint32_t len, int flags,
                         const sockaddr_in_t *dest, uint32_t destlen) {
    (void)destlen;
    return (int)indevos_syscall_result(
        (long)syscall(SYS_SENDTO, (uint64_t)fd, (uint64_t)buf, (uint64_t)len,
                      (uint64_t)flags, (uint64_t)dest));
}
static inline int recvfrom(int fd, void *buf, uint32_t len, int flags,
                           sockaddr_in_t *src, uint32_t *srclen) {
    (void)srclen;
    return (int)indevos_syscall_result(
        (long)syscall(SYS_RECVFROM, (uint64_t)fd, (uint64_t)buf, (uint64_t)len,
                      (uint64_t)flags, (uint64_t)src));
}

static inline int bind(int fd, const sockaddr_in_t *addr, uint32_t addrlen) {
    return (int)indevos_syscall_result(
        (long)syscall(SYS_BIND, (uint64_t)fd, (uint64_t)addr, (uint64_t)addrlen, 0, 0));
}
static inline int listen(int fd, int backlog) {
    return (int)indevos_syscall_result((long)syscall(SYS_LISTEN, (uint64_t)fd, (uint64_t)backlog, 0, 0, 0));
}
static inline int accept(int fd, sockaddr_in_t *addr, uint32_t *addrlen) {
    return (int)indevos_syscall_result(
        (long)syscall(SYS_ACCEPT, (uint64_t)fd, (uint64_t)addr, (uint64_t)addrlen, 0, 0));
}
static inline int shutdown(int sockfd, int how) {
    return (int)indevos_syscall_result((long)syscall(SYS_SHUTDOWN, (uint64_t)sockfd, (uint64_t)how, 0, 0, 0));
}

static inline int pipe(int pipefd[2]) {
    return (int)indevos_syscall_result((long)syscall(SYS_PIPE, (uint64_t)pipefd, 0, 0, 0, 0));
}
static inline int dup(int oldfd) {
    return (int)indevos_syscall_result((long)syscall(SYS_DUP, (uint64_t)oldfd, 0, 0, 0, 0));
}
static inline int dup2(int oldfd, int newfd) {
    return (int)indevos_syscall_result((long)syscall(SYS_DUP2, (uint64_t)oldfd, (uint64_t)newfd, 0, 0, 0));
}
static inline int fcntl(int fd, int cmd, int arg) {
    return (int)indevos_syscall_result((long)syscall(SYS_FCNTL, (uint64_t)fd, (uint64_t)cmd, (uint64_t)arg, 0, 0));
}
static inline int ioctl(int fd, unsigned long request, void *arg) {
    return (int)indevos_syscall_result((long)syscall(SYS_IOCTL, (uint64_t)fd, request, (uint64_t)arg, 0, 0));
}

static inline void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset) {
    (void)fd;
    (void)offset;
    long r = (long)syscall(SYS_MMAP, (uint64_t)addr, (uint64_t)length, (uint64_t)(unsigned)prot,
                           (uint64_t)(unsigned)flags, 0);
    if (r < 0 && r >= -1024) {
#ifndef INDEVOS_NO_ERRNO
        errno = (int)-r;
#endif
        return MAP_FAILED;
    }
#ifndef INDEVOS_NO_ERRNO
    errno = 0;
#endif
    return (void *)(uint64_t)r;
}
static inline int munmap(void *addr, size_t length) {
    return (int)indevos_syscall_result((long)syscall(SYS_MUNMAP, (uint64_t)addr, (uint64_t)length, 0, 0, 0));
}

static inline int gettimeofday(timeval_ind_t *tv, void *tz) {
    return (int)indevos_syscall_result((long)syscall(SYS_GETTIMEOFDAY, (uint64_t)tv, (uint64_t)tz, 0, 0, 0));
}
static inline int poll(pollfd_t *fds, unsigned int nfds, int timeout_ms) {
    return (int)indevos_syscall_result((long)syscall(SYS_POLL, (uint64_t)fds, (uint64_t)nfds, (uint64_t)(int64_t)timeout_ms, 0, 0));
}

static inline pid_t getppid(void) {
    return (pid_t)indevos_syscall_result((long)syscall(SYS_GETPPID, 0, 0, 0, 0, 0));
}
static inline mode_t umask(mode_t mask) {
    return (mode_t)indevos_syscall_result((long)syscall(SYS_UMASK, (uint64_t)mask, 0, 0, 0, 0));
}
static inline int clock_gettime(int clockid, timespec_ind_t *tp) {
    return (int)indevos_syscall_result((long)syscall(SYS_CLOCK_GETTIME, (uint64_t)(unsigned)clockid, (uint64_t)tp, 0, 0, 0));
}
static inline int nanosleep(const timespec_ind_t *req, timespec_ind_t *rem) {
    return (int)indevos_syscall_result((long)syscall(SYS_NANOSLEEP, (uint64_t)req, (uint64_t)rem, 0, 0, 0));
}
static inline int sigaction(int signum, const sigaction_ind_t *act, sigaction_ind_t *old) {
    return (int)indevos_syscall_result(
        (long)syscall(SYS_SIGACTION, (uint64_t)(unsigned)signum, (uint64_t)act, (uint64_t)old, 0, 0));
}
static inline int sigprocmask(int how, uint32_t *set, uint32_t *oldset) {
    return (int)indevos_syscall_result(
        (long)syscall(SYS_SIGPROCMASK, (uint64_t)(unsigned)how, (uint64_t)set, (uint64_t)oldset, 0, 0));
}

static inline int fstat(int fd, stat_t *st) {
    return (int)indevos_syscall_result((long)syscall(SYS_FSTAT, (uint64_t)fd, (uint64_t)st, 0, 0, 0));
}
static inline int lstat(const char *path, stat_t *st) {
    return (int)indevos_syscall_result((long)syscall(SYS_LSTAT, (uint64_t)path, (uint64_t)st, 0, 0, 0));
}
static inline int chmod(const char *path, mode_t mode) {
    return (int)indevos_syscall_result((long)syscall(SYS_CHMOD, (uint64_t)path, (uint64_t)mode, 0, 0, 0));
}
static inline int rename(const char *oldpath, const char *newpath) {
    return (int)indevos_syscall_result((long)syscall(SYS_RENAME, (uint64_t)oldpath, (uint64_t)newpath, 0, 0, 0));
}

/* ---- Framebuffer + keyboard (used by full-screen programs such as matrix) -- */

static inline uint32_t fb_width(void) {
    return (uint32_t)syscall(SYS_FB_GET_WIDTH, 0, 0, 0, 0, 0);
}
static inline uint32_t fb_height(void) {
    return (uint32_t)syscall(SYS_FB_GET_HEIGHT, 0, 0, 0, 0, 0);
}

/** Fill the whole screen with `color` (0x00RRGGBB). */
static inline void fb_clear(uint32_t color) {
    syscall(SYS_FB_FILLRECT, color, 0, 0, (uint64_t)fb_width(), (uint64_t)fb_height());
}

/** Fill a rectangle; negative/oversized values are clipped. */
static inline void fb_fillrect(uint32_t color, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w <= 0 || h <= 0) return;
    syscall(SYS_FB_FILLRECT, color, (uint64_t)x, (uint64_t)y, (uint64_t)w, (uint64_t)h);
}

/** Non-blocking keyboard read; returns the number of bytes copied. */
static inline int kb_read(char *buf, size_t max) {
    return (int)indevos_syscall_result(
        (long)syscall(SYS_KB_READ, (uint64_t)buf, (uint64_t)max, 0, 0, 0));
}

/** Drop any buffered keystrokes (e.g. the Enter that launched this program). */
static inline void kb_clear(void) {
    syscall(SYS_KB_CLEAR, 0, 0, 0, 0, 0);
}

/** True once the user pressed Ctrl+C. */
static inline int terminate_requested(void) {
    return (int)syscall(SYS_GET_TERMINATE_FLAG, 0, 0, 0, 0, 0);
}

/** Read the CMOS real-time clock (wall-clock date/time). */
static inline int rtc_time(indevos_rtc_time_t *t) {
    return (int)indevos_syscall_result(
        (long)syscall(SYS_RTC_TIME, (uint64_t)t, 0, 0, 0, 0));
}

#endif // SYSCALLS_H
