#ifndef SYSCALL_H
#define SYSCALL_H

#include "types.h"
#include "../include/indevos_gui.h"
#include "../include/indevos_syscall_numbers.h"
#include "../include/indevos_stat.h"
#include "../include/indevos_errno.h"

// Basic types
typedef long off_t;
typedef int pid_t;
typedef uint32_t mode_t;
typedef int64_t time_t_ind;
typedef int64_t suseconds_t_ind;

#ifndef SIG_BLOCK
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2
#endif

#ifndef SA_RESTART
#define SA_RESTART  0x10000000u
#define SA_SIGINFO  0x00000004u
#endif

#ifndef PROT_READ
#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define PROT_EXEC   0x4
#define PROT_NONE   0x0
#endif

#ifndef MAP_SHARED
#define MAP_SHARED     0x01
#define MAP_PRIVATE    0x02
#define MAP_FIXED      0x10
#define MAP_ANONYMOUS  0x20
#endif

#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
#endif

typedef struct {
    time_t_ind tv_sec;
    suseconds_t_ind tv_usec;
} timeval_ind_t;

typedef struct {
    time_t_ind tv_sec;
    long       tv_nsec;
} timespec_ind_t;

typedef struct {
    uint64_t sa_handler;
    uint64_t sa_mask;
    uint32_t sa_flags;
    uint32_t sa_resv;
} sigaction_ind_t;

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

#define NSIG      32  // Number of signals

// Signal handler types
typedef void (*sighandler_t)(int);
#define SIG_DFL ((sighandler_t)0)  // Default action
#define SIG_IGN ((sighandler_t)1)  // Ignore signal
#define SIG_ERR ((sighandler_t)-1) // Error return from signal()

// Syscall numbers - keep these in sync with user space header
/* poll() event flags */
#define POLLIN      0x0001   /* There is data to read */
#define POLLOUT     0x0004   /* Writing is possible */
#define POLLERR     0x0008   /* Error condition */
#define POLLHUP     0x0010   /* Hang up */
#define POLLNVAL    0x0020   /* Invalid fd */

/* pollfd structure */
typedef struct {
    int      fd;
    int16_t  events;       /* requested events */
    int16_t  revents;      /* returned events */
} pollfd_t;

/* fd_set for select() – supports up to 64 FDs */
#define FD_SETSIZE_KERNEL 64
typedef struct {
    uint32_t bits[(FD_SETSIZE_KERNEL + 31) / 32];
} fd_set_t;

#define FD_ZERO(set)       memset((set), 0, sizeof(fd_set_t))
#define FD_SET_BIT(fd, set) ((set)->bits[(fd) / 32] |= (1U << ((fd) % 32)))
#define FD_CLR(fd, set)    ((set)->bits[(fd) / 32] &= ~(1U << ((fd) % 32)))
#define FD_ISSET(fd, set)  (((set)->bits[(fd) / 32] >> ((fd) % 32)) & 1)

/* Socket domains, types, protocols */
#define AF_INET         2
#define SOCK_STREAM     1   /* TCP */
#define SOCK_DGRAM      2   /* UDP */
#define IPPROTO_TCP     6
#define IPPROTO_UDP     17

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

// Seek whence values
#define SEEK_SET        0
#define SEEK_CUR        1
#define SEEK_END        2

// fcntl commands
#define F_DUPFD         0   // Duplicate file descriptor
#define F_GETFD         1   // Get file descriptor flags
#define F_SETFD         2   // Set file descriptor flags
#define F_GETFL         3   // Get file status flags
#define F_SETFL         4   // Set file status flags

// fcntl flags
#define FD_CLOEXEC      1   // Close on exec

// ioctl commands (generic)
#define FIONREAD        0x541B  // Get number of bytes to read
#define FIONBIO         0x5421  // Set/clear non-blocking I/O

/* st_mode type bits (Linux/glibc style, octal 0100000 / 0040000) */
#define S_IFMT          0170000
#define S_IFREG         0100000
#define S_IFDIR         0040000
#define S_IFCHR         0020000
#define S_IFLNK         0120000
#define S_IRWXU         00700
#define S_IRWXG         00070
#define S_IRWXO         00007
#define DEFAULT_ST_MODE_FILE  (S_IFREG | 0644u)
#define DEFAULT_ST_MODE_DIR   (S_IFDIR | 0755u)

// Network operations
#define NET_GET_MAC     0
#define NET_SEND_PACKET 1
#define NET_RECV_PACKET 2
#define NET_GET_STATUS  3

/** User buffer layout for SYS_READDIR (matches vfs_dirent_t fields).
 *  Defined in include/indevos_dirent.h so user programs share the same layout
 *  instead of having to include this kernel header. */
#include "../include/indevos_dirent.h"
/* Shared with user programs: the SYS_RTC_TIME result layout. */
#include "../include/indevos_rtc.h"

/* BSD socket address structures */
typedef struct {
    uint16_t sa_family;
    char     sa_data[14];
} sockaddr_t;

typedef struct {
    uint16_t sin_family;      /* AF_INET */
    uint16_t sin_port;        /* port in network byte order (big-endian) */
    uint32_t sin_addr;        /* IPv4 address in network byte order */
    uint8_t  sin_zero[8];     /* padding */
} sockaddr_in_t;

/* Socket state stored in file_handle when fd_type == FD_TYPE_SOCKET */
typedef struct kernel_socket {
    int      domain;          /* AF_INET */
    int      type;            /* SOCK_STREAM or SOCK_DGRAM */
    int      protocol;
    uint16_t local_port;
    uint16_t remote_port;
    uint8_t  remote_ip[4];
    bool     bound;
    bool     listening;
    bool     connected;
    void    *transport;       /* tcp_socket_t* or udp handle */
} kernel_socket_t;

// Maximum number of open files per process (avoid conflict with fat32.h)
#ifndef MAX_OPEN_FILES_SYSCALL
#define MAX_OPEN_FILES_SYSCALL  64
#endif

// File descriptor type
#define FD_TYPE_FILE        0   // Regular file (FAT32, etc.)
#define FD_TYPE_PIPE_READ   1   // Read end of a pipe
#define FD_TYPE_PIPE_WRITE  2   // Write end of a pipe
#define FD_TYPE_SOCKET      3   // TCP/UDP socket
#define FD_TYPE_VFS         4   // VFS-backed file (any mounted FS)
#define FD_TYPE_VFS_DIR     5   // Directory: file_handle = kalloc'd absolute path; position = readdir index
#define FD_TYPE_DEV_NULL    6   /* /dev/null */
#define FD_TYPE_DEV_TTY     7   /* /dev/tty -> controlling console */

// File descriptor structure
typedef struct {
    void *file_handle;      // Pointer to FAT32 file or pipe_t handle
    uint32_t *ref_count;    // Shared reference count for duplicated FDs
    uint32_t flags;         // Open flags
    uint32_t position;      // Current read offset, or directory entry index for FD_TYPE_VFS_DIR
    int32_t vfs_fd;         // VFS file descriptor (when fd_type == FD_TYPE_VFS)
    uint8_t fd_type;        // FD_TYPE_*
    bool in_use;           // Whether this fd is active
    bool cloexec;          // Close-on-exec (FD_CLOEXEC)
} file_descriptor_t;

/** @brief One entry in a user ELF symbol table (for crash backtraces) */
typedef struct {
    uint64_t addr;
    const char *name; /* points into process_t.user_sym_string_pool */
} user_symbol_t;

typedef struct proc_mmap_region {
    uint64_t virt;
    size_t   len;
    struct proc_mmap_region *next;
} proc_mmap_region_t;

// Process structure
typedef struct process {
    uint32_t pid;
    file_descriptor_t fds[MAX_OPEN_FILES_SYSCALL];
    void *brk_start;       // Start of heap
    void *brk_current;     // Current break point
    char cwd[256];         // Current working directory
    
    // Process memory layout
    void *memory_base;     // Base address of process memory (kernel-mapped)
    uint64_t memory_size;  // Size of process memory
    uint64_t virtual_base; // Virtual base address

    // Per-process address space (Ring 3 support)
    uint64_t *pml4;            // Per-process PML4 (virtual), NULL = kernel process
    uint8_t  *kernel_stack;    // Per-process kernel stack base (kalloc'd)
    uint64_t  kernel_stack_top; // Top of kernel stack (for TSS RSP0)
    uint64_t  user_stack_top;  // Top of user-mode stack
    
    // Process state
    struct process *parent; // Parent process
    struct process *child;  // First child (linked list)
    struct process *sibling; // Next sibling
    
    // Process status
    int exit_status;
    bool is_zombie;        // Exited but not reaped
    bool is_running;       // Currently executing
    bool is_usermode;      // true if this process runs in Ring 3
    
    // Signal handling
    sighandler_t signal_handlers[NSIG];  // Signal handler functions
    uint32_t signal_sa_flags[NSIG];
    uint32_t pending_signals;           // Bitmask of pending signals
    uint32_t blocked_signals;           // Bitmask of blocked signals
    uint32_t pgid;                      // Process group ID
    uint32_t sid;                       // Session ID
    /* Saved user context for signal delivery / rt_sigreturn.
     * Full GPR set from the live syscall/IRQ frame — never a global scratch. */
    uint64_t sig_saved_r15, sig_saved_r14, sig_saved_r13, sig_saved_r12;
    uint64_t sig_saved_rbp, sig_saved_rbx;
    uint64_t sig_saved_r9, sig_saved_r8, sig_saved_r10; /* a6,a5,a4 */
    uint64_t sig_saved_rip, sig_saved_rsp, sig_saved_rflags;
    uint64_t sig_saved_rax, sig_saved_rdi, sig_saved_rsi, sig_saved_rdx;
    bool     sig_in_handler;
    /* Handler entered from an IRQ (preempted ring-3 frame): rcx/r11 are live
     * user values, so rt_sigreturn must iretq with a full GPR restore. */
    bool     sig_from_irq;
    bool     sig_iret_pending;
    uint64_t sig_saved_rcx, sig_saved_r11;
    uint64_t sig_restorer[NSIG];

    mode_t file_umask;                  // process umask (default 022)
    uint64_t mmap_anon_next;            // hint for MAP_ANONYMOUS placement
    proc_mmap_region_t *mmap_regions;   // anon mmap list for munmap

    // User/group identity (set from current session at creation time)
    uint32_t uid;                       // User ID of process owner
    uint32_t gid;                       // Group ID of process owner

    /* Controlling virtual console: stdout/printf from this process goes here when
     * that VC is not visible (mirrors Unix session TTY). */
    int8_t cont_tty;

    /* ELF .symtab / .dynsym (optional): resolved at exec time for stack traces */
    user_symbol_t *user_symtab;
    int            user_symtab_count;
    char          *user_sym_string_pool;

    /* vfork/fork support: when non-NULL the child runs on the parent's
     * thread; longjmp back to fork_jmpbuf returns child_pid to parent. */
    struct process *fork_parent;
    uint64_t fork_jmpbuf[8]; /* same layout as exec_jmpbuf_t */
    /* The fork/clone user context (RIP/RSP/RFLAGS/GPRs) lives only in the
     * forking thread's sc_* stash (thread_t), copied by value at fork. */
    /* SMP: newest fork child held BLOCKED past fork SYSRET. */
    void *fork_pending_release; /* thread_t * */
    /* Prior held child: kept BLOCKED until wait/exit (with newest).
     * Both pipeline sides become runnable together. */
    void *fork_release_at_exit; /* thread_t * */
    /* 1 => skip releasing fork_pending_release on this syscall exit. */
    int fork_defer_release;

    /* Non-NULL when spawned via exec_spawn (preemptively scheduled Ring-3).
     * sys_exit uses thread-based cleanup instead of longjmp. */
    void *sched_thread;

    /* GUI events for SYS_DESKTOP_POLL_EVENT (BoredOS-style poll loop). */
    gui_event_queue_t gui_queue;

    /* Linux ABI layer (linux_syscall.c).  linux_abi is set the first time the
     * process enters the kernel through the SYSCALL instruction. */
    bool     linux_abi;
    bool     clone_shared_vm;           /* CLONE_VM: do not destroy shared pml4 */
    uint64_t linux_clear_child_tid;     /* set_tid_address() pointer */
    /* Saved termios flags for Linux TCGETS/TCSETS (ash raw mode). */
    uint32_t linux_ti_iflag;
    uint32_t linux_ti_oflag;
    uint32_t linux_ti_cflag;
    uint32_t linux_ti_lflag;
    uint8_t  linux_ti_cc[19];
    bool     linux_ti_valid;
    char     linux_exe_path[256];       /* absolute path of current image */

    /* In-place execve: after exec_replace(), LX_execve rewrites the SYSCALL
     * frame to jump to exec_entry_rip/rsp instead of nesting exec_file. */
    bool     exec_replaced;
    uint64_t exec_entry_rip;
    uint64_t exec_entry_rsp;

    /* Asynchronous kill request (remote kill / zeus `kill` / desktop close).
     * Non-zero = the signal that killed it.  Set only by
     * process_request_kill(); the target thread exits ITSELF at its next safe
     * point (syscall return, IRQ return to ring 3, or an interruptible sleep
     * that returns -EINTR first) -- it is never torn down from outside. */
    volatile int kill_sig;

    /* alarm()/setitimer(ITIMER_REAL): absolute ktime deadline in ms (0 = not
     * armed) and reload interval.  Fired from the BSP LAPIC tick by
     * process_alarm_tick(), which posts SIGALRM like kill() does. */
    volatile uint64_t alarm_deadline_ms;
    uint64_t alarm_interval_ms;

    /* rt_sigsuspend: mask to restore once the handler that woke it has run
     * (rt_sigreturn), or at the next syscall if no handler was invoked. */
    uint32_t sigsuspend_saved_mask;
    bool     sigsuspend_restore;
} process_t;

/** @brief Virtual memory layout for processes */
#define KERNEL_VIRT_BASE    0xFFFFFFFF80000000ULL  /**< Kernel virtual base */
#define USER_VIRT_BASE      0x0000000000000000ULL  /**< User virtual base */
#define USER_STACK_TOP      0x00007FFFFFFFF000ULL  /**< User stack top */
#define USER_HEAP_START     0x0000000010000000ULL  /**< User heap start */
#define USER_CODE_START     0x0000000000400000ULL  /**< User code start */

/** @brief Process states */
#define PROCESS_RUNNING     0  /**< Process is currently executing */
#define PROCESS_READY       1  /**< Process is ready to run */
#define PROCESS_BLOCKED     2  /**< Process is blocked waiting for event */
#define PROCESS_ZOMBIE      3  /**< Process has terminated but not reaped */
#define PROCESS_TERMINATED  4  /**< Process has been terminated */

/** @brief Process priorities (prefixed to avoid conflict with thread.h enum) */
#define PROC_PRIORITY_LOW        0
#define PROC_PRIORITY_NORMAL     1
#define PROC_PRIORITY_HIGH       2
#define PROC_PRIORITY_REALTIME   3

/** @brief Process time slice (in milliseconds) */
#define TIME_SLICE_MS       10

// Function declarations
/* Remote/asynchronous kill (see process_request_kill in syscall.c). */
void process_request_kill(process_t *t, int signum);
bool process_is_self(process_t *p);
bool proc_kill_pending(process_t *p);
void syscall_init(void);
void syscall_enable_cpu(uint32_t cpu_id);
uint64_t syscall_handler(uint64_t syscall_num, uint64_t arg1, uint64_t arg2,
                        uint64_t arg3, uint64_t arg4, uint64_t arg5);

// Individual syscall implementations
int sys_exit(int status);
int sys_read(int fd, void *buf, size_t count);
int sys_write(int fd, const void *buf, size_t count);
int sys_open(const char *pathname, int flags, mode_t mode);
int sys_open_kpath(const char *kpath, int flags, mode_t mode);
int syscall_stat_abspath(const char *abspath, stat_t *st);
int sys_close(int fd);
int sys_seek(int fd, off_t offset, int whence);
void* sys_brk(void *addr);
void* sys_mmap(void *addr, size_t length, int prot, int flags);
int sys_munmap(void *addr, size_t length);
int sys_getpid(void);
int sys_fork(void);
int sys_exec(const char *pathname, char *const argv[]);
int sys_exec_env(const char *pathname, char *const argv[], char *const envp[]);
int sys_wait(int *status);
int sys_poll_kernel(pollfd_t *kfds, unsigned int nfds, int timeout_ms);
int sys_wait_pid(long wpid, int *status);
sighandler_t sys_signal(int signum, sighandler_t handler);
int sys_kill(pid_t pid, int signum);
int sys_sleep(unsigned int seconds);
uint64_t sys_time(void);
int sys_print(const char *message);
int sys_getchar(void);
int sys_mkdir(const char *pathname);
int sys_rmdir(const char *pathname);
int sys_unlink(const char *pathname);
int sys_stat(const char *pathname, stat_t *statbuf);
int sys_fstat(int fd, stat_t *statbuf);
int sys_lstat(const char *pathname, stat_t *statbuf);
int sys_sigaction(int signum, const sigaction_ind_t *act, sigaction_ind_t *oldact);
int sys_sigprocmask(int how, uint32_t *set, uint32_t *oldset);
int sys_rename(const char *oldpath, const char *newpath);
int sys_link(const char *oldpath, const char *newpath);
int sys_symlink(const char *target, const char *linkpath);
int sys_readlink(const char *pathname, char *buf, size_t bufsiz);
int sys_chmod(const char *pathname, mode_t mode);
int sys_fchmod(int fd, mode_t mode);
mode_t sys_umask(mode_t mask);
int sys_clock_gettime(int clockid, timespec_ind_t *tp);
int sys_nanosleep(const timespec_ind_t *req, timespec_ind_t *rem);
int sys_getppid(void);
int sys_gettimeofday(timeval_ind_t *tv, void *tz);
int sys_rtc_time(indevos_rtc_time_t *user);
int sys_select(int nfds, fd_set_t *readfds, fd_set_t *writefds, fd_set_t *exceptfds, void *timeout_tv);
int sys_poll(pollfd_t *fds, unsigned int nfds, int timeout_ms);
char* sys_getcwd(char *buf, size_t size);
int sys_chdir(const char *path);
int sys_mount(const char *device, const char *mountpoint, const char *fstype);
int sys_unmount(const char *mountpoint);
int sys_readdir(int fd, void *dirp, size_t count);
int sys_network(int operation, void *arg1, void *arg2, void *arg3);
int sys_pipe(int pipefd[2]);
int sys_dup(int oldfd);
int sys_dup2(int oldfd, int newfd);
int sys_fcntl(int fd, int cmd, int arg);
int sys_ioctl(int fd, unsigned long request, void *arg);
int sys_bind(int fd, const sockaddr_in_t *addr, uint32_t addrlen);
int sys_listen(int fd, int backlog);
int sys_accept(int fd, sockaddr_in_t *addr, uint32_t *addrlen);
int sys_setsockopt(int fd, int level, int optname, const void *optval, uint32_t optlen);
int sys_getsockopt(int fd, int level, int optname, void *optval, uint32_t *optlen);
int sys_shutdown(int fd, int how);
int sys_setuid(uint32_t uid);
int sys_setgid(uint32_t gid);
int sys_getuid(void);
int sys_getgid(void);

// Process management functions
process_t* get_current_process(void);
process_t* get_kernel_process(void);
void set_current_process(process_t *proc);
void process_set_cwd(process_t *proc, const char *path);
void init_process_system(void);
int allocate_fd(process_t *process);
void free_fd(process_t *process, int fd);
process_t* process_alloc(void);
void process_cleanup(process_t *proc);
bool process_pml4_shared_with_others(uint64_t *pml4, process_t *self);
void process_inherit_fds(process_t *dst, process_t *src);
void process_gui_enqueue_by_pid(uint32_t pid, const gui_event_t *ev);

// Memory mapping functions
void syscall_set_program_mapping(void *real_base, uint64_t virtual_base, uint64_t size);

// Per-CPU kernel stack for SYSCALL fast path (updated on process switch)
void syscall_set_kernel_stack(uint64_t rsp);

/**
 * @brief Enter user mode (Ring 3) at the given entry point.
 *
 * Sets up the iretq frame to switch from Ring 0 to Ring 3.
 * This function does NOT return - it jumps directly to user code.
 *
 * GDT layout (syscall/sysret compatible):
 *   0x1B = User data segment selector (0x18 | RPL=3)
 *   0x23 = User code segment selector (0x20 | RPL=3)
 *
 * @param entry_rip  User-mode instruction pointer
 * @param user_rsp   User-mode stack pointer
 */
__attribute__((noreturn))
void enter_usermode(uint64_t entry_rip, uint64_t user_rsp);


int sys_mprotect(void *addr, size_t length, int prot);
int sys_clone(uint64_t flags, uint64_t newsp, uint64_t parent_tidptr,
              uint64_t child_tidptr, uint64_t tls);
int sys_getpgid(pid_t pid);
int sys_setpgid(pid_t pid, pid_t pgid);
int sys_setsid(void);

void fork_release_pending(process_t *p);
/* sys_mmap() internal flag: MAP_SHARED anonymous (shared across fork). */
#define SYS_MMAP_KSHARED 0x40000000
void fork_release_after_syscall(process_t *p);
void fork_child_zombie_exit(int status);
int process_deliver_signals(process_t *proc, void *linux_frame);
void process_kill_pgid(uint32_t pgid, int signum);
void process_set_fg_pgid(uint32_t pgid);
uint32_t process_get_fg_pgid(void);

#endif // SYSCALL_H
