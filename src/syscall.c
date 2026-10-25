#include "drm.h"
#include "registers.h"
#include "syscall.h"
#include "syscall_internal.h"
#include "idt.h"
#include "utils.h"
#include "gpu_hal.h"
#include "vfs.h"
#include "vfs_fat32.h"
#include "vfs_ext4.h"
#include "ext4.h"
#include "mem.h"
#include "string.h"
#include "serial.h"
#include "hpet.h"
#include "uaccess.h"
#include "keyboard.h"
#include "exec.h"
#include "storage.h"
#include "net_device.h"
#include "net_tcp.h"
#include "net_udp.h"
#include "net_ethernet.h"
#include "gdt.h"
#include "msr.h"
#include "smp.h"
#include "../include/SDL.h"
#include "../include/sdl_kernel_log.h"
#include "mouse.h"
#include "atomic.h"
#include "term.h"    // clear_screen()
#include "utils.h"   // fillrect(), fb_get_width/height(), get_terminate_flag()
#include "exec.h"    // exec_notify_exit()
#include "desktop.h"
#include "users.h"   // user_current_uid(), user_current_gid()
#include "pipe.h"
#include "../include/indevos_errno.h"
#include "users.h"
#include "debug_config.h"
#include "thread.h"
#include "profiler.h"
#include "rtc.h"             // rtc_read() for SYS_RTC_TIME
#include "kernel_config.h"   // MAX_PROCESSES

// External assembly functions
extern void syscall_asm_handler(void);
extern void syscall_entry(void);

// MSR addresses for SYSCALL/SYSRET infrastructure
#define MSR_EFER            0xC0000080  // Extended Feature Enable Register
#define MSR_STAR            0xC0000081  // Segment selectors for syscall/sysret
#define MSR_LSTAR           0xC0000082  // Long-mode syscall entry RIP
#define MSR_SFMASK          0xC0000084  // RFLAGS bits to clear on syscall entry
#define MSR_GS_BASE         0xC0000101  // User-visible GS base
#define MSR_KERNEL_GS_BASE  0xC0000102  // Kernel GS base (swapgs target)
#define EFER_SCE            (1 << 0)    // System Call Extensions enable

/*
 * Per-CPU layout expected by syscall_entry (syscallasm.asm):
 *   gs:0  = kernel_rsp  (kernel stack top for fast syscall path)
 *   gs:8  = user_rsp    (scratch: holds user RSP during syscall)
 *
 * KERNEL_GS_BASE MSR points here; swapgs makes GS = &bsp_gs in kernel.
 */
typedef struct {
    uint64_t kernel_rsp;
    uint64_t user_rsp;
} cpu_gs_t;

static cpu_gs_t cpu_gs[MAX_CPUS];
#define bsp_gs cpu_gs[0]

static inline uint64_t ret_errno(int e) {
    return (uint64_t)(int64_t)(-e);
}

static int resolve_abspath(process_t *proc, const char *kpath, char *abspath, size_t absmax);
static int kernel_path_is_directory(process_t *proc, const char *kpath, char *abspath, size_t absmax);

// SDL syscall function declarations
int sys_sdl_init(uint32_t flags);
void sys_sdl_quit(void);
SDL_Window* sys_sdl_create_window(const char* title, int x, int y, int w, int h);
void sys_sdl_destroy_window(SDL_Window* window);
SDL_Renderer* sys_sdl_create_renderer(SDL_Window* window, int index, uint32_t flags);
void sys_sdl_destroy_renderer(SDL_Renderer* renderer);
int sys_sdl_set_render_draw_color(SDL_Renderer* renderer, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
int sys_sdl_render_clear(SDL_Renderer* renderer);
void sys_sdl_render_present(SDL_Renderer* renderer);
int sys_sdl_poll_event(void *user_event);
uint32_t sys_sdl_get_ticks(void);
void sys_sdl_delay(uint32_t ms);
int sys_sdl_render_draw_rect(SDL_Renderer* renderer, const SDL_Rect* rect);
int sys_sdl_render_fill_rect(SDL_Renderer* renderer, const SDL_Rect* rect);
const char* sys_sdl_get_error(void);
int sys_sdl_copy_pixels(SDL_Renderer *renderer, const void *user_pixels, uint32_t width, uint32_t height);
int sys_sdl_get_keyboard_state(void *user_buf, uint64_t n);
uint64_t sys_sdl_get_mouse_rel_accum(void);
uint64_t sys_sdl_get_mouse_buttons_u64(void);
int sys_desktop_embed_info(void *user_info);
int sys_desktop_composite(const void *user_pixels, uint32_t w, uint32_t h);

/** Ensure user page tables are loaded before memcpy to/from user pointers (IRQ may leave kernel CR3). */
void syscall_uaccess_ensure_user_cr3(void) {
    /* Prefer get_current_process(): concurrent fork children have linux_abi
     * and their own pml4, but fork_parent==NULL and exec_ring3_active_proc
     * may still name the parent. Switching to the parent AS here made
     * sys_exec copy_from_user see the wrong argv (argc=0) while LX_execve
     * diag ? which ran before this switch ? still saw busybox/wc/-c.
     * That produced busybox applet-list spam under wait-only hold. */
    process_t *p = get_current_process();
    if (p && p->is_usermode && p->pml4) {
        vmm_switch_address_space(p->pml4);
        return;
    }
    p = exec_user_as_process();
    if (p && p->is_usermode && p->pml4)
        vmm_switch_address_space(p->pml4);
}

// Socket syscalls – forward declarations
int sys_socket(int domain, int type, int protocol);
int sys_connect(int fd, sockaddr_in_t *addr, uint32_t addrlen);
int sys_send(int fd, const void *buf, uint32_t len, int flags);
int sys_recv(int fd, void *buf, uint32_t len, int flags);
int sys_sendto(int fd, const void *buf, uint32_t len, int flags,
               const sockaddr_in_t *dest);
int sys_recvfrom(int fd, void *buf, uint32_t len, int flags,
                 sockaddr_in_t *src);
int sys_bind(int fd, const sockaddr_in_t *addr, uint32_t addrlen);
int sys_listen(int fd, int backlog);
int sys_accept(int fd, sockaddr_in_t *addr, uint32_t *addrlen);
int sys_setsockopt(int fd, int level, int optname, const void *optval, uint32_t optlen);
int sys_getsockopt(int fd, int level, int optname, void *optval, uint32_t *optlen);
int sys_shutdown(int fd, int how);

// Process management (MAX_PROCESSES comes from kernel_config.h)
static process_t processes[MAX_PROCESSES];
static spinlock_t gui_queue_lock = {0};
static void fork_release_thread(thread_t *thr);
void fork_child_zombie_exit(int status);
void process_fault_kill_current(int signum);


static process_t *current_process = NULL;
static uint32_t next_pid = 1;

// Track current program's memory mapping for user-space access
static void *current_program_base = NULL;
static uint64_t current_program_vbase = 0;
static uint64_t current_program_size = 0;

/* FS holds the kernel data selector after gdt load.  User TLS lives in the
 * FS_BASE MSR (arch_prctl), and loading/implicitly nulling a selector can
 * reset that base on some CPUs, so park FS on the null selector (like Linux)
 * before any CPU first drops to Ring 3.  The kernel never uses FS. */
static void syscall_clear_fs_selector(void) {
    __asm__ volatile("mov %0, %%fs" :: "r"((uint32_t)0) : "memory");
}

void syscall_init(void) {
    DEBUG_SERIAL("Initializing syscall system\n");
    syscall_clear_fs_selector();

    pipe_init();
    init_process_system();

    // Legacy INT 0x80 path (DPL=3 so user code can trigger it)
    setIdtGate(128, (void*)syscall_asm_handler, 0xEE);

    // ---- Fast SYSCALL/SYSRET path ----------------------------------------
    // Enable SCE (System Call Extensions) in EFER
    uint32_t efer_lo, efer_hi;
    cpu_get_msr(MSR_EFER, &efer_lo, &efer_hi);
    efer_lo |= EFER_SCE;
    cpu_set_msr(MSR_EFER, efer_lo, efer_hi);

    /*
     * STAR layout (64 bits):
     *   [63:48] = sysret_cs_base (N): sysret loads CS = N+16|3, SS = N+8|3
     *   [47:32] = syscall_cs    (M): syscall loads CS = M,       SS = M+8
     *   [31:0]  = unused (zero)
     *
     * GDT (gdt.c): 0x08=kernel CS, 0x10=kernel DS, 0x18=user DS, 0x20=user CS
     *   syscall: M=0x08  → kernel CS=0x08, kernel SS=0x10
     *   sysret:  N=0x10  → user CS=0x20|3=0x23, user SS=0x18|3=0x1B
     */
    cpu_set_msr(MSR_STAR, 0, 0x00100008);

    // LSTAR = entry point for 64-bit SYSCALL instruction
    uint64_t lstar = (uint64_t)syscall_entry;
    cpu_set_msr(MSR_LSTAR, (uint32_t)(lstar & 0xFFFFFFFF), (uint32_t)(lstar >> 32));

    // SFMASK: on syscall entry clear TF (8), IF (9), DF (10) and AC (18) so the
    // kernel starts with interrupts off, forward string ops and SMAP enforced.
    cpu_set_msr(MSR_SFMASK, (1u << 8) | (1u << 9) | (1u << 10) | (1u << 18), 0);

    // Point KERNEL_GS_BASE at our per-CPU struct so swapgs works
    bsp_gs.kernel_rsp = gdt_get_kernel_stack();
    bsp_gs.user_rsp   = 0;
    uint64_t gs_addr = (uint64_t)&bsp_gs;
    cpu_set_msr(MSR_KERNEL_GS_BASE,
                (uint32_t)(gs_addr & 0xFFFFFFFF),
                (uint32_t)(gs_addr >> 32));

    DEBUG_SERIAL("Syscall system initialized (INT 0x80 + SYSCALL/SYSRET)\n");
}

void syscall_enable_cpu(uint32_t cpu_id) {
    if (cpu_id >= MAX_CPUS)
        return;
    syscall_clear_fs_selector();

    uint32_t efer_lo, efer_hi;
    cpu_get_msr(MSR_EFER, &efer_lo, &efer_hi);
    efer_lo |= EFER_SCE;
    cpu_set_msr(MSR_EFER, efer_lo, efer_hi);
    cpu_set_msr(MSR_STAR, 0, 0x00100008);
    uint64_t lstar = (uint64_t)syscall_entry;
    cpu_set_msr(MSR_LSTAR, (uint32_t)(lstar & 0xFFFFFFFF), (uint32_t)(lstar >> 32));
    cpu_set_msr(MSR_SFMASK, (1u << 8) | (1u << 9) | (1u << 10) | (1u << 18), 0);

    cpu_gs[cpu_id].kernel_rsp = gdt_get_kernel_stack();
    cpu_gs[cpu_id].user_rsp = 0;
    uint64_t gs_addr = (uint64_t)&cpu_gs[cpu_id];
    cpu_set_msr(MSR_KERNEL_GS_BASE,
                (uint32_t)(gs_addr & 0xFFFFFFFF),
                (uint32_t)(gs_addr >> 32));
}

void init_process_system(void) {
    memset(processes, 0, sizeof(processes));
    
    current_process = &processes[0];
    current_process->pid = next_pid++;
    strcpy(current_process->cwd, vfs_exists("/home/user") ? "/home/user" : "/");
    current_process->is_running = true;
    current_process->parent = NULL;
    current_process->child = NULL;
    current_process->sibling = NULL;
    
    for (int i = 0; i < NSIG; i++) {
        current_process->signal_handlers[i] = SIG_DFL;
        current_process->signal_sa_flags[i] = 0;
    }
    current_process->pending_signals = 0;
    current_process->blocked_signals = 0;
    
    current_process->fds[STDIN_FILENO].in_use = true;
    current_process->fds[STDOUT_FILENO].in_use = true;
    current_process->fds[STDERR_FILENO].in_use = true;
    
    current_process->uid = user_current_uid();
    current_process->gid = user_current_gid();
    
    current_process->brk_start = (void*)0x400000;
    current_process->brk_current = current_process->brk_start;
    current_process->cont_tty = 0;
    current_process->file_umask = 022;
    current_process->mmap_anon_next = 0x20000000ULL;
}

process_t* get_current_process(void) {
    /* Prefer the thread running on this CPU.  current_process is a single
     * global that the scheduler overwrites on every core, so an AP running
     * Doom would otherwise see the BSP's kernel/zeus process and fail
     * focus/pid checks (dead keyboard, composite into the wrong window). */
    thread_t *t = scheduler_get_current_thread();
    if (t && t->owner_process) {
        process_t *op = (process_t *)t->owner_process;
        if (op && !op->is_zombie)
            return op;
    }
    /* Skip zombie globals — fork_child_zombie_exit used to leave parent here. */
    if (current_process && !current_process->is_zombie)
        return current_process;
    return current_process; /* may be NULL/kernel; callers check */
}

/* The root/kernel process (slot 0) — used as fallback owner/current. */
process_t* get_kernel_process(void) {
    return &processes[0];
}

void process_set_cwd(process_t *proc, const char *path) {
    if (!proc)
        return;
    if (!path || !path[0])
        path = "/";
    size_t n = strlen(path);
    if (n >= sizeof(proc->cwd))
        n = sizeof(proc->cwd) - 1;
    memcpy(proc->cwd, path, n);
    proc->cwd[n] = '\0';
}

int allocate_fd(process_t *process) {
    /* POSIX: lowest free fd.  Linux-ABI processes may close 0..2 and expect
     * open() to reuse them (ash `cmd &`: close(0); open("/dev/null")).
     * Native inDevOS apps keep the old "skip stdio" behaviour. */
    for (int i = process->linux_abi ? 0 : 3; i < MAX_OPEN_FILES_SYSCALL; i++) {
        if (!process->fds[i].in_use) {
            memset(&process->fds[i], 0, sizeof(file_descriptor_t));
            process->fds[i].in_use = true;
            return i;
        }
    }
    return -EMFILE;  // No free file descriptors
}

void free_fd(process_t *process, int fd) {
    if (fd >= 0 && fd < MAX_OPEN_FILES_SYSCALL) {
        process->fds[fd].in_use = false;
        process->fds[fd].file_handle = NULL;
        process->fds[fd].ref_count = NULL;
        process->fds[fd].flags = 0;
        process->fds[fd].position = 0;
        process->fds[fd].vfs_fd = -1;
        process->fds[fd].fd_type = FD_TYPE_FILE;
    }
}

uint32_t *fd_refcount_create(void) {
    uint32_t *rc = (uint32_t*)kalloc(sizeof(uint32_t));
    if (rc) *rc = 1;
    return rc;
}

static void process_signal_terminate(process_t *proc, int signum);
static void handle_pending_signals(process_t *proc);
static void process_orphan_children(process_t *proc);
bool proc_signal_interrupts(process_t *p);
void process_post_signal(process_t *t, int signum);
int sys_getchar_intr(process_t *p);

int process_deliver_signals(process_t *proc, void *linux_frame) {
    if (!proc || !linux_frame || proc->sig_in_handler) return 0;
    typedef struct {
        uint64_t r15, r14, r13, r12, rbp, rbx;
        uint64_t a6, a5, a4, a3, a2, a1;
        uint64_t nr, user_rflags, user_rip, user_rsp;
    } lx_frame_t;
    lx_frame_t *fr = (lx_frame_t *)linux_frame;
    for (int signum = 1; signum < NSIG; signum++) {
        uint32_t bit = 1u << signum;
        if (!(proc->pending_signals & bit)) continue;
        if (proc->blocked_signals & bit) continue;
        sighandler_t handler = proc->signal_handlers[signum];
        if (handler == SIG_IGN || handler == SIG_ERR) {
            proc->pending_signals &= ~bit;
            continue;
        }
        if (handler == SIG_DFL) {
            /* POSIX defaults: SIGCHLD/CONT ignored — never fatal via deliver. */
            if (signum == SIGCHLD || signum == SIGCONT || signum == 28 /*WINCH*/ ||
                signum == 23 /*URG*/) {
                proc->pending_signals &= ~bit;
                continue;
            }
            handle_pending_signals(proc);
            return 1;
        }
        /* Reject non-canonical handlers (e.g. -1 / kernel addrs). */
        {
            uint64_t ha = (uint64_t)(uintptr_t)handler;
            if (ha < 0x10000ULL || (ha >> 47) != 0) {
                serial_printf("deliver: drop bad handler sig=%d ha=0x%llx\n",
                              signum, (unsigned long long)ha);
                proc->pending_signals &= ~bit;
                if (signum == SIGCHLD || signum == SIGCONT)
                    continue;
                process_signal_terminate(proc, signum);
                return 1;
            }
        }
        /* Require a real SA_RESTORER trampoline — without it, ret from the
         * handler lands on a fake address and can #PF (RIP=-1 observed). */
        if (!proc->sig_restorer[signum]) {
            serial_printf("deliver: defer sig=%d handler=0x%llx (no restorer)\n",
                          signum, (unsigned long long)(uintptr_t)handler);
            if (signum == SIGCHLD || signum == SIGCONT) {
                proc->pending_signals &= ~bit;
                continue;
            }
            /* Leave pending for a later syscall once restorer is installed;
             * do not invent a restorer from user_rip. */
            return 0;
        }
        proc->pending_signals &= ~bit;

        /* Ensure CR3 is this process before pushing the restorer. */
        if (proc->pml4) {
            extern void vmm_switch_address_space(uint64_t *);
            vmm_switch_address_space(proc->pml4);
        }

        /* BusyBox ash SIGCHLD G-null skip removed — retest PIPEOK/SIGCHLD. */

        /* Snapshot the LIVE frame (this thread's kstack), not a stale stash. */
        proc->sig_saved_r15 = fr->r15;
        proc->sig_saved_r14 = fr->r14;
        proc->sig_saved_r13 = fr->r13;
        proc->sig_saved_r12 = fr->r12;
        proc->sig_saved_rbp = fr->rbp;
        proc->sig_saved_rbx = fr->rbx;
        proc->sig_saved_r9  = fr->a6;
        proc->sig_saved_r8  = fr->a5;
        proc->sig_saved_r10 = fr->a4;
        proc->sig_saved_rdx = fr->a3;
        proc->sig_saved_rsi = fr->a2;
        proc->sig_saved_rdi = fr->a1;
        /* sig_saved_rax: set by linux_syscall_dispatch to the syscall return. */
        proc->sig_saved_rip = fr->user_rip;
        proc->sig_saved_rsp = fr->user_rsp;
        proc->sig_saved_rflags = fr->user_rflags;
        proc->sig_in_handler = true;

        uint64_t restorer = proc->sig_restorer[signum];
        uint64_t rsp = fr->user_rsp & ~0xFULL;
        rsp -= 8;
        {
            if (copy_to_user((void *)(uintptr_t)rsp, &restorer, sizeof(restorer)) != 0) {
                proc->sig_in_handler = false;
                process_signal_terminate(proc, signum);
                return 1;
            }
        }
        fr->user_rsp = rsp;
        fr->user_rip = (uint64_t)(uintptr_t)handler;
        /* Handler args: always rdi=signum. For non-SIGINFO clear rsi/rdx so
         * leftover clone/wait args cannot be mistaken for sa_sigaction ptrs. */
        fr->a1 = (uint64_t)signum;
        if (!(proc->signal_sa_flags[signum] & 0x4)) { /* SA_SIGINFO */
            fr->a2 = 0; /* rsi */
            fr->a3 = 0; /* rdx */
        }
        fr->user_rflags = 0x202;
        return 1;
    }
    return 0;
}

static int fd_close_entry(process_t *proc, int fd) {
    if (!proc || fd < 0 || fd >= MAX_OPEN_FILES_SYSCALL || !proc->fds[fd].in_use) {
        return -EBADF;
    }

    file_descriptor_t *fde = &proc->fds[fd];
    uint8_t ty = fde->fd_type;

    /*
     * Pipe endpoints: each open FD is one readers/writers credit (fork +
     * dup both call pipe_fork_dup_*). Always release that credit on close,
     * even when the fd ref_count is still shared with another process or
     * dup'd slot. Gating pipe_close on ref_count==1 left writers>0 after
     * ash pipelines, so wc blocked forever on read (PIPEOK hang).
     */
    if (ty == FD_TYPE_PIPE_READ) {
        pipe_close_read((pipe_t *)fde->file_handle);
    } else if (ty == FD_TYPE_PIPE_WRITE) {
        pipe_close_write((pipe_t *)fde->file_handle);
    }

    bool should_free_object = true;
    void *last_rc_key = NULL;
    if (fde->ref_count) {
        if (*fde->ref_count > 1) {
            (*fde->ref_count)--;
            should_free_object = false;
        } else {
            if (ty == FD_TYPE_VFS) { extern void flk_release(void *); flk_release(fde->ref_count); }
            last_rc_key = fde->ref_count;       /* compared only, never dereferenced */
            kfree(fde->ref_count);
        }
    }
    fde->ref_count = NULL;

    if (should_free_object && ty != FD_TYPE_PIPE_READ && ty != FD_TYPE_PIPE_WRITE) {
        if (ty == 8 /* FD_TYPE_EVENTFD */ || ty == 13 /* FD_TYPE_EPOLL */ ||
            ty == 14 /* FD_TYPE_TIMERFD */ || ty == 15 /* FD_TYPE_SIGNALFD */) {
            if (fde->file_handle) kfree(fde->file_handle);
        } else if (ty == 16 /* FD_TYPE_INOTIFY */) {
            extern void inotify_release(void *h);
            inotify_release(fde->file_handle);
            fde->file_handle = NULL;
        } else if (ty == 10 /* FD_TYPE_DEV_INPUT */) {
            extern void evdev_release(void *handle);
            evdev_release(fde->file_handle);
            fde->file_handle = NULL;
        } else if (ty == 11 /* FD_TYPE_DEV_UINPUT */) {
            extern void uinput_release(void *handle);
            uinput_release(fde->file_handle);   /* destroys the device */
            fde->file_handle = NULL;
        } else if (ty == 12 /* FD_TYPE_DEV_WM */) {
            extern void wm_dev_release(void *handle);
            wm_dev_release(fde->file_handle);   /* destroys its windows */
            fde->file_handle = NULL;
        } else if (ty == FD_TYPE_SOCKET) {
            kernel_socket_t *sock = (kernel_socket_t *)fde->file_handle;
            if (sock) {
                kernel_socket_destroy(sock);
            }
        } else if (ty == FD_TYPE_VFS) {
            if (fde->vfs_fd >= 0) {
                vfs_close(fde->vfs_fd);
            }
            if (last_rc_key) { extern void memfd_release(void *); memfd_release(last_rc_key); }
        } else if (ty == FD_TYPE_VFS_DIR) {
            if (fde->file_handle) {
                vfs_dircache_drop(fde->file_handle);
                kfree(fde->file_handle);
            }
        }
    }

    free_fd(proc, fd);
    return 0;
}


/* Thread reaper: true while any process slot still names @t as its thread,
 * or still holds it as a not-yet-released fork newborn (a killed held child
 * can exit before its parent's fork_release_pending() looks at it). */
bool process_refs_thread(const void *t) {
    if (!t) return false;
    for (int i = 0; i < MAX_PROCESSES; i++)
        if (processes[i].sched_thread == t ||
            processes[i].fork_pending_release == t ||
            processes[i].fork_release_at_exit == t)
            return true;
    return false;
}

process_t* process_find_by_pml4(uint64_t *pml4) {
    if (!pml4) return NULL;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (!processes[i].pid) continue;
        if (!processes[i].is_usermode) continue;
        if (processes[i].pml4 == pml4)
            return &processes[i];
    }
    return NULL;
}

void set_current_process(process_t *proc) {
    current_process = proc;
}

process_t* process_alloc(void) {
    for (int i = 1; i < MAX_PROCESSES; i++) {
        /* A slot is free only when the process is neither running NOR a
         * zombie. Zombies stay linked in their parent's child list until
         * sys_wait() reaps them; reusing such a slot while the parent still
         * holds a pointer to it is a use-after-free. */
        if (!processes[i].is_running && !processes[i].is_zombie) {
            /* Inherit the caller's cwd. Hardcoding /home/user made every
             * exec'd program (including bare `ls`) list the home directory
             * even after the shell had changed directory. */
            process_t *cur = get_current_process();
            char inherited[256];
            const char *src = "/";
            if (cur && cur != &processes[i] && cur->cwd[0])
                src = cur->cwd;
            size_t in = strlen(src);
            if (in >= sizeof(inherited))
                in = sizeof(inherited) - 1;
            memcpy(inherited, src, in);
            inherited[in] = '\0';

            memset(&processes[i], 0, sizeof(process_t));
            processes[i].pid = next_pid++;
            processes[i].pgid = processes[i].pid;
            processes[i].sid  = processes[i].pid;
            memcpy(processes[i].cwd, inherited, in + 1);
            processes[i].is_running = true;
            for (int j = 0; j < NSIG; j++) {
                processes[i].signal_handlers[j] = SIG_DFL;
            }
            processes[i].fds[STDIN_FILENO].in_use = true;
            processes[i].fds[STDOUT_FILENO].in_use = true;
            processes[i].fds[STDERR_FILENO].in_use = true;
            processes[i].uid = user_current_uid();
            processes[i].gid = user_current_gid();
            processes[i].cont_tty = -1;
            processes[i].file_umask = 022;
            processes[i].mmap_anon_next = 0x20000000ULL;
            return &processes[i];
        }
    }
    return NULL;
}

/* Remove proc from its parent's child/sibling chain (if linked). */
static void process_unlink_from_parent(process_t *proc) {
    process_t *parent = proc->parent;
    if (!parent) return;
    if (parent->child == proc) {
        parent->child = proc->sibling;
    } else {
        process_t *prev = parent->child;
        while (prev && prev->sibling != proc)
            prev = prev->sibling;
        if (prev)
            prev->sibling = proc->sibling;
    }
    proc->parent = NULL;
    proc->sibling = NULL;
}

/* Final teardown for a process that nobody will wait() on.
 * Closes fds, unlinks from the parent's child list, detaches any children,
 * and clears the slot so process_alloc() can safely reuse it. */

/* Copy non-CLOEXEC fds from @src into @dst (Linux execve semantics).
 * Used so a vfork child that dup2'd a pipe onto stdin/stdout keeps those
 * redirections when exec_run_elf allocates a fresh process slot. */
void process_inherit_fds(process_t *dst, process_t *src) {
    if (!dst || !src || dst == src)
        return;
    /* Only stdio that was redirected (pipe / VFS file). Copying every
     * non-CLOEXEC fd double-freed path strings and panicked after ash
     * pipelines. */
    for (int i = 0; i <= 2; i++) {
        if (!src->fds[i].in_use || src->fds[i].cloexec)
            continue;
        uint8_t ty = src->fds[i].fd_type;
        if (ty != FD_TYPE_PIPE_READ && ty != FD_TYPE_PIPE_WRITE &&
            ty != FD_TYPE_VFS)
            continue;
        if (dst->fds[i].in_use)
            fd_close_entry(dst, i);
        dst->fds[i] = src->fds[i];
        dst->fds[i].in_use = true;
        if (dst->fds[i].ref_count)
            (*dst->fds[i].ref_count)++;
        else
            dst->fds[i].ref_count = fd_refcount_create();
        if (ty == FD_TYPE_PIPE_READ)
            pipe_fork_dup_reader((pipe_t *)dst->fds[i].file_handle);
        else if (ty == FD_TYPE_PIPE_WRITE)
            pipe_fork_dup_writer((pipe_t *)dst->fds[i].file_handle);
    }
}


/* Linux clone(2) flags we honor (subset). */
#ifndef CLONE_VM
#define CLONE_VM             0x00000100ULL
#define CLONE_VFORK          0x00004000ULL
#define CLONE_SETTLS         0x00080000ULL
#define CLONE_PARENT_SETTID  0x00100000ULL
#define CLONE_CHILD_CLEARTID 0x00200000ULL
#define CLONE_CHILD_SETTID   0x01000000ULL
#endif

/* Address-space ownership: a PML4 is owned jointly by every live process
 * slot that points at it (CLONE_VM / vfork share one table).  The LAST
 * holder to drop it destroys it.  Decide on live sharers only -- the
 * clone_shared_vm flag is sticky on the vfork parent (it stays set after the
 * child execs or exits), so using it here leaked the parent's whole AS. */
bool process_pml4_shared_with_others(uint64_t *pml4, process_t *self) {
    if (!pml4) return false;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (&processes[i] == self) continue;
        if (processes[i].pid && processes[i].pml4 == pml4)
            return true;
    }
    return false;
}

void process_cleanup(process_t *proc) {
    if (!proc) return;
    if (proc->pml4) {
        extern uint64_t *vmm_get_kernel_pml4(void);
        extern void vmm_switch_address_space(uint64_t *);
        extern void vmm_destroy_address_space(uint64_t *);
        if (process_pml4_shared_with_others(proc->pml4, proc)) {
            proc->pml4 = NULL; /* other sharer still owns the AS */
        } else {
            vmm_switch_address_space(vmm_get_kernel_pml4());
            vmm_destroy_address_space(proc->pml4);
            proc->pml4 = NULL;
        }
    }

    for (int i = 0; i < MAX_OPEN_FILES_SYSCALL; i++) {
        if (proc->fds[i].in_use)
            fd_close_entry(proc, i);
    }

    process_unlink_from_parent(proc);

    /* Detach children: their parent pointer would dangle once this slot is
     * reused. Zombie children will never be reaped now, so free them too. */
    for (int i = 1; i < MAX_PROCESSES; i++) {
        if (&processes[i] != proc && processes[i].parent == proc) {
            processes[i].parent = NULL;
            processes[i].sibling = NULL;
            if (processes[i].is_zombie) {
                if (processes[i].pml4) {
                    if (!process_pml4_shared_with_others(processes[i].pml4, &processes[i]))
                        vmm_destroy_address_space(processes[i].pml4);
                    processes[i].pml4 = NULL;
                }
                memset(&processes[i], 0, sizeof(process_t));
            }
        }
    }

    memset(proc, 0, sizeof(process_t));
}

void process_gui_enqueue_by_pid(uint32_t pid, const gui_event_t *ev) {
    if (!ev || pid == 0) return;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (processes[i].pid == pid && processes[i].is_running) {
            gui_event_queue_t *q = &processes[i].gui_queue;
            spinlock_lock(&gui_queue_lock);
            uint32_t next = (q->tail + 1) % GUI_EVENT_QUEUE_CAP;
            if (next != q->head) {
                q->q[q->tail] = *ev;
                q->tail = next;
            }
            spinlock_unlock(&gui_queue_lock);
            return;
        }
    }
}

int sys_desktop_poll_event(void *user_ev) {
    process_t *proc = get_current_process();
    if (!proc || !user_ev) return -EINVAL;
    if (!desktop_is_running()) return -ENODEV;

    gui_event_t ev;
    spinlock_lock(&gui_queue_lock);
    gui_event_queue_t *q = &proc->gui_queue;
    if (q->head == q->tail) {
        spinlock_unlock(&gui_queue_lock);
        return 0;
    }
    ev = q->q[q->head];
    q->head = (q->head + 1) % GUI_EVENT_QUEUE_CAP;
    spinlock_unlock(&gui_queue_lock);

    syscall_uaccess_ensure_user_cr3();
    if (copy_to_user(user_ev, &ev, sizeof(ev)) != 0)
        return -14; /* EFAULT */
    return 1;
}

void syscall_set_kernel_stack(uint64_t rsp) {
    gdt_set_kernel_stack(rsp);
    per_cpu_t *cpu = smp_get_current_cpu();
    uint32_t id = (cpu && cpu->cpu_id < MAX_CPUS) ? cpu->cpu_id : 0;
    cpu_gs[id].kernel_rsp = rsp;
}

/**
 * @brief Main system call dispatcher with security validation
 *
 * Handles all system calls with proper parameter validation and security checks.
 * Validates syscall numbers, pointer arguments, and buffer sizes to prevent
 * common security vulnerabilities.
 *
 * @param syscall_num System call number
 * @param arg1 First argument
 * @param arg2 Second argument
 * @param arg3 Third argument
 * @param arg4 Fourth argument (reserved)
 * @param arg5 Fifth argument (reserved)
 * @return System call result or error code
 */

/* INT 0x80 leaves iretq frame at *iframe: RIP, CS, RFLAGS, RSP, SS.
 * Stash for SYS_FORK so /bin/sh.exe (native ABI) can concurrent-fork. */
void syscall_stash_int80_fork_frame(uint64_t *iframe) {
    thread_t *thr = scheduler_get_current_thread();
    if (!iframe) return;
    /* Per-thread stash is the only fork snapshot (nested fork / SMP-safe).
     * INT 0x80 has no GPR frame here; sc_r* keep their last values. */
    if (thr) {
        thr->sc_user_rip = iframe[0];
        thr->sc_user_rflags = iframe[2];
        thr->sc_user_rsp = iframe[3];
        thr->sc_frame_valid = 1;
    }
}

static uint64_t syscall_dispatch(uint64_t syscall_num, uint64_t arg1, uint64_t arg2,
                               uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    /* Ctrl+C: interrupt the foreground process.
     * First press: if the process registered a SIGINT handler, deliver it
     *              and give the process a chance to shut down gracefully.
     * Second press (force_kill_flag): skip the handler and kill immediately. */
    if (get_terminate_flag() && get_program_running()) {
        process_t *proc = get_current_process();
        bool force = get_force_kill_flag();

        if (!force && proc &&
            proc->signal_handlers[SIGINT] != SIG_DFL &&
            proc->signal_handlers[SIGINT] != SIG_IGN) {
            set_terminate_flag(false);
            proc->pending_signals |= (1 << SIGINT);
            return syscall_num == SYS_EXIT ? 0 : (uint64_t)-1;
        }

        set_terminate_flag(false);
        exec_notify_exit(128 + SIGINT);
    }

    // Security: Validate syscall number range
    if (syscall_num >= SYS_MAX) {
        serial_puts("syscall_handler: Invalid syscall number ");
        serial_printf("%d", (int)syscall_num);
        serial_puts("\n");
        return ret_errno(EINVAL);
    }

    // Log syscall for debugging (only in debug builds)
    DEBUG_SERIAL("syscall_handler: syscall_num=");
    DEBUG_SERIAL("%d", (int)syscall_num);
    DEBUG_SERIAL(" arg1=0x%llx", arg1);
    DEBUG_SERIAL("\n");

    switch (syscall_num) {
        case SYS_EXIT:
            return sys_exit((int)arg1);
        case SYS_READ:
            return sys_read((int)arg1, (void*)arg2, (size_t)arg3);
        case SYS_WRITE:
            return sys_write((int)arg1, (const void*)arg2, (size_t)arg3);
        case SYS_OPEN:
            return (uint64_t)(int64_t)sys_open((const char*)arg1, (int)arg2, (mode_t)arg3);
        case SYS_CLOSE:
            return sys_close((int)arg1);
        case SYS_SEEK:
            return sys_seek((int)arg1, (off_t)arg2, (int)arg3);
        case SYS_BRK:
            return (uint64_t)sys_brk((void*)arg1);
        case SYS_MMAP:
            return (uint64_t)sys_mmap((void*)arg1, (size_t)arg2, (int)arg3, (int)arg4);
        case SYS_MUNMAP:
            return sys_munmap((void*)arg1, (size_t)arg2);
        case SYS_GETPID:
            return sys_getpid();
        case SYS_FORK:
            get_current_process()->fork_defer_release = 0;
            return sys_fork();
        case SYS_EXEC:
            return sys_exec((const char*)arg1, (char*const*)arg2);
        case SYS_SLEEP:
            return sys_sleep((unsigned int)arg1);
        case SYS_TIME:
            return sys_time();
        case SYS_PRINT:
            return sys_print((const char*)arg1);
        case SYS_GETCHAR:
            return sys_getchar();
        case SYS_MKDIR:
            return sys_mkdir((const char*)arg1);
        case SYS_RMDIR:
            return sys_rmdir((const char*)arg1);
        case SYS_UNLINK:
            return sys_unlink((const char*)arg1);
        case SYS_STAT:
            return sys_stat((const char*)arg1, (stat_t*)arg2);
        case SYS_GETCWD:
            return (uint64_t)sys_getcwd((char*)arg1, (size_t)arg2);
        case SYS_CHDIR:
            return sys_chdir((const char*)arg1);
        case SYS_MOUNT:
            return sys_mount((const char*)arg1, (const char*)arg2, (const char*)arg3);
        case SYS_UNMOUNT:
            return sys_unmount((const char*)arg1);
        case SYS_READDIR:
            return sys_readdir((int)arg1, (void*)arg2, (size_t)arg3);
        case SYS_NETWORK:
            return sys_network((int)arg1, (void*)arg2, (void*)arg3, (void*)arg4);
        case SYS_WAIT:
            return sys_wait((int*)arg1);
        case SYS_SIGNAL:
            return (uint64_t)sys_signal((int)arg1, (sighandler_t)arg2);
        case SYS_KILL:
            return sys_kill((pid_t)arg1, (int)arg2);

        /* ---- Framebuffer / Keyboard / Control syscalls (63-69) ---- */
        case SYS_FB_CLEAR:
            fb_app_clear();
            return 0;
        case SYS_FB_FILLRECT:
            /* arg1=color, arg2=x, arg3=y, arg4=w, arg5=h */
            fb_app_fillrect((uint32_t)arg1, arg2, arg3, arg4, arg5);
            return 0;
        case SYS_FB_GET_WIDTH:
            return fb_get_width();
        case SYS_FB_GET_HEIGHT:
            return fb_get_height();
        case SYS_KB_READ: {
            /* arg1 = user buf ptr, arg2 = max bytes to read (non-blocking) */
            char *ubuf = (char *)arg1;
            uint64_t max = arg2;
            if (!ubuf || max == 0) return 0;
            if (!access_ok_user_range((uint64_t)ubuf, (size_t)max)) return -EFAULT;
            uint64_t n = 0;
            while (n < max) {
                int c = keyboard_consume_char();
                if (c < 0) break;
                char ch = (char)c;
                if (copy_to_user(ubuf + n, &ch, 1) != 0) {
                    return (n > 0) ? (int64_t)n : -EFAULT;
                }
                n++;
            }
            return n;
        }
        case SYS_KB_CLEAR:
            keyboard_clear_buffer();
            return 0;
        case SYS_GET_TERMINATE_FLAG:
            return get_terminate_flag() ? 1 : 0;
        case SYS_RTC_TIME:
            return sys_rtc_time((indevos_rtc_time_t *)arg1);

        case SYS_SETUID:
            return sys_setuid((uint32_t)arg1);
        case SYS_SETGID:
            return sys_setgid((uint32_t)arg1);
        case SYS_GETUID:
            return sys_getuid();
        case SYS_GETGID:
            return sys_getgid();

        // SDL syscalls
        case SYS_SDL_INIT:
            return sys_sdl_init((uint32_t)arg1);
        case SYS_SDL_QUIT:
            sys_sdl_quit();
            return 0;
        case SYS_SDL_CREATE_WINDOW:
            return (uint64_t)sys_sdl_create_window((const char*)arg1, (int)arg2, (int)arg3, (int)arg4, (int)arg5);
        case SYS_SDL_DESTROY_WINDOW:
            sys_sdl_destroy_window((SDL_Window*)arg1);
            return 0;
        case SYS_SDL_CREATE_RENDERER:
            return (uint64_t)sys_sdl_create_renderer((SDL_Window*)arg1, (int)arg2, (uint32_t)arg3);
        case SYS_SDL_DESTROY_RENDERER:
            sys_sdl_destroy_renderer((SDL_Renderer*)arg1);
            return 0;
        case SYS_SDL_SET_DRAW_COLOR:
            return sys_sdl_set_render_draw_color((SDL_Renderer*)arg1, (uint8_t)arg2, (uint8_t)arg3, (uint8_t)arg4, (uint8_t)arg5);
        case SYS_SDL_RENDER_CLEAR:
            return sys_sdl_render_clear((SDL_Renderer*)arg1);
        case SYS_SDL_RENDER_PRESENT:
            sys_sdl_render_present((SDL_Renderer*)arg1);
            return 0;
        case SYS_SDL_POLL_EVENT:
            return sys_sdl_poll_event((void *)arg1);
        case SYS_SDL_GET_TICKS:
            return sys_sdl_get_ticks();
        case SYS_SDL_DELAY:
            sys_sdl_delay((uint32_t)arg1);
            return 0;
        case SYS_SDL_RENDER_DRAW_RECT:
            return sys_sdl_render_draw_rect((SDL_Renderer*)arg1, (const SDL_Rect*)arg2);
        case SYS_SDL_RENDER_FILL_RECT:
            return sys_sdl_render_fill_rect((SDL_Renderer*)arg1, (const SDL_Rect*)arg2);
        case SYS_SDL_GET_ERROR:
            return (uint64_t)sys_sdl_get_error();
        case SYS_SDL_COPY_PIXELS:
            return (uint64_t)(int64_t)sys_sdl_copy_pixels((SDL_Renderer *)arg1, (const void *)arg2,
                                                          (uint32_t)arg3, (uint32_t)arg4);
        case SYS_SDL_GET_KEYBOARD_STATE:
            return (uint64_t)(int64_t)sys_sdl_get_keyboard_state((void *)arg1, arg2);
        case SYS_SDL_GET_MOUSE_REL_ACCUM:
            return sys_sdl_get_mouse_rel_accum();
        case SYS_SDL_GET_MOUSE_BUTTONS:
            return sys_sdl_get_mouse_buttons_u64();
        case SYS_SDL_GET_DISPLAY_SIZE: {
            return ((uint64_t)fb_get_width() << 32) | (uint64_t)fb_get_height();
        }
        case SYS_DESKTOP_EMBED_INFO:
            return (uint64_t)(int64_t)sys_desktop_embed_info((void *)arg1);
        case SYS_DESKTOP_COMPOSITE:
            return (uint64_t)(int64_t)sys_desktop_composite((const void *)arg1, (uint32_t)arg2, (uint32_t)arg3);

        /* ── BSD socket syscalls ── */
        case SYS_SOCKET:
            return (uint64_t)sys_socket((int)arg1, (int)arg2, (int)arg3);
        case SYS_CONNECT:
            return (uint64_t)sys_connect((int)arg1, (sockaddr_in_t *)arg2, (uint32_t)arg3);
        case SYS_SEND:
            return (uint64_t)sys_send((int)arg1, (const void *)arg2, (uint32_t)arg3, (int)arg4);
        case SYS_RECV:
            return (uint64_t)sys_recv((int)arg1, (void *)arg2, (uint32_t)arg3, (int)arg4);
        case SYS_SENDTO:
            return (uint64_t)sys_sendto((int)arg1, (const void *)arg2, (uint32_t)arg3,
                                        (int)arg4, (const sockaddr_in_t *)arg5);
        case SYS_RECVFROM:
            return (uint64_t)sys_recvfrom((int)arg1, (void *)arg2, (uint32_t)arg3,
                                          (int)arg4, (sockaddr_in_t *)arg5);
        case SYS_BIND:
            return (uint64_t)sys_bind((int)arg1, (sockaddr_in_t *)arg2, (uint32_t)arg3);
        case SYS_LISTEN:
            return (uint64_t)sys_listen((int)arg1, (int)arg2);
        case SYS_ACCEPT:
            return (uint64_t)sys_accept((int)arg1, (sockaddr_in_t *)arg2, (uint32_t *)arg3);
        case SYS_SETSOCKOPT:
            return (uint64_t)sys_setsockopt((int)arg1, (int)arg2, (int)arg3, (void *)arg4, (uint32_t)arg5);
        case SYS_GETSOCKOPT:
            return (uint64_t)sys_getsockopt((int)arg1, (int)arg2, (int)arg3, (void *)arg4, (uint32_t *)arg5);
        case SYS_SHUTDOWN:
            return (uint64_t)sys_shutdown((int)arg1, (int)arg2);
        case SYS_GETTIMEOFDAY:
            return (uint64_t)sys_gettimeofday((timeval_ind_t *)arg1, (void *)arg2);
        case SYS_SELECT:
            return (uint64_t)sys_select((int)arg1, (fd_set_t *)arg2, (fd_set_t *)arg3, (fd_set_t *)arg4, (void *)arg5);
        case SYS_POLL:
            return (uint64_t)sys_poll((pollfd_t *)arg1, (unsigned int)arg2, (int)arg3);

        case SYS_PIPE:
            return (uint64_t)sys_pipe((int *)arg1);
        case SYS_DUP:
            return (uint64_t)sys_dup((int)arg1);
        case SYS_DUP2:
            return (uint64_t)sys_dup2((int)arg1, (int)arg2);
        case SYS_FCNTL:
            return (uint64_t)sys_fcntl((int)arg1, (int)arg2, (int)arg3);
        case SYS_IOCTL:
            return (uint64_t)sys_ioctl((int)arg1, (unsigned long)arg2, (void *)arg3);
        case SYS_SIGACTION:
            return (uint64_t)sys_sigaction((int)arg1, (const sigaction_ind_t *)arg2, (sigaction_ind_t *)arg3);
        case SYS_SIGPROCMASK:
            return (uint64_t)sys_sigprocmask((int)arg1, (uint32_t *)arg2, (uint32_t *)arg3);
        case SYS_FSTAT:
            return (uint64_t)sys_fstat((int)arg1, (stat_t *)arg2);
        case SYS_LSTAT:
            return (uint64_t)sys_lstat((const char *)arg1, (stat_t *)arg2);
        case SYS_RENAME:
            return (uint64_t)sys_rename((const char *)arg1, (const char *)arg2);
        case SYS_LINK:
            return (uint64_t)sys_link((const char *)arg1, (const char *)arg2);
        case SYS_SYMLINK:
            return (uint64_t)sys_symlink((const char *)arg1, (const char *)arg2);
        case SYS_READLINK:
            return (uint64_t)sys_readlink((const char *)arg1, (char *)arg2, (size_t)arg3);
        case SYS_CHMOD:
            return (uint64_t)sys_chmod((const char *)arg1, (mode_t)arg2);
        case SYS_FCHMOD:
            return (uint64_t)sys_fchmod((int)arg1, (mode_t)arg2);
        case SYS_UMASK:
            return (uint64_t)sys_umask((mode_t)arg1);
        case SYS_CLOCK_GETTIME:
            return (uint64_t)sys_clock_gettime((int)arg1, (timespec_ind_t *)arg2);
        case SYS_NANOSLEEP:
            return (uint64_t)sys_nanosleep((const timespec_ind_t *)arg1, (timespec_ind_t *)arg2);
        case SYS_GETPPID:
            return (uint64_t)sys_getppid();
        case SYS_DESKTOP_POLL_EVENT:
            return (uint64_t)(int64_t)sys_desktop_poll_event((void *)arg1);

        default:
            // Security: Log invalid syscall attempts
            serial_puts("SECURITY: Invalid syscall attempt - number ");
            serial_printf("%d", (int)syscall_num);
            serial_puts(" from process ");
            serial_printf("%d", get_current_process()->pid);
            serial_puts("\n");
            return ret_errno(ENOSYS);
    }
}

uint64_t syscall_handler(uint64_t syscall_num, uint64_t arg1, uint64_t arg2,
                        uint64_t arg3, uint64_t arg4, uint64_t arg5)
{
    profiler_syscall_enter(syscall_num);
    uint64_t ret = syscall_dispatch(syscall_num, arg1, arg2, arg3, arg4, arg5);
    profiler_syscall_leave(syscall_num, ret);
    {
        process_t *p = get_current_process();
        /* Safe point: a remote kill takes effect here, after the syscall has
         * unwound and released whatever it held. */
        if (p && p->is_running && __atomic_load_n(&p->kill_sig, __ATOMIC_ACQUIRE) &&
            process_is_self(p)) {
            int ks = p->kill_sig;
            serial_printf("signal: PID %u exits on kill (sig %d) at syscall return\n",
                          p->pid, ks);
            sys_exit(128 + ks);
        }
        fork_release_after_syscall(p);
    }
    return ret;
}

// Syscall implementations
int sys_exit(int status) {
    SERIAL_DEBUG_PRINT(SYSCALL, "sys_exit called with status %d\n", status);

    process_t *proc = get_current_process();
    if (proc)
        fork_release_pending(proc);
    if (proc && !proc->fork_parent)
        process_orphan_children(proc);   /* legacy vfork child: parent borrows;
                                          * clone-vfork: done in zombie exit */
    if (proc) {
        proc->alarm_deadline_ms = 0;
        proc->exit_status = status;
        proc->is_running  = false;
    }

    /* If this is a LEGACY vfork child (running on the parent's thread after
     * the parent's setjmp), return to the parent's fork() call instead of
     * longjmping to exec_run_elf.  A clone(CLONE_VFORK) child owns its own
     * thread and never had fork_jmpbuf set: it exits as a zombie below
     * (fork_child_zombie_exit wakes the parent waiting in sys_clone). */
    bool own_thread_vfork = proc && proc->fork_parent && proc->sched_thread &&
        (thread_t *)proc->sched_thread == scheduler_get_current_thread() &&
        proc->sched_thread != proc->fork_parent->sched_thread;
    if (proc && proc->fork_parent && !own_thread_vfork) {
        proc->is_zombie = true;
        process_t *parent = proc->fork_parent;
        int child_pid = (int)proc->pid;
        proc->fork_parent = NULL;
        current_process = parent;
        exec_set_ring3_active(parent);


        exec_longjmp((exec_jmpbuf_t *)parent->fork_jmpbuf, child_pid);
        __builtin_unreachable();
    }

    /* Foreground sync program (ash launched from Zeus) whose boot thread got
     * pinned as sched_thread: return to Zeus via longjmp, never thread-exit
     * the Zeus thread (that left Zeus deaf to input after ash exited). */
    {
        extern bool exec_current_is_sync_program(void);
        extern process_t *exec_get_sync_proc(void);
        extern void process_set_fg_pgid(uint32_t pgid);
        extern thread_t *exec_get_sync_thread_for(process_t *p);
        /* Identify the sync program by process + thread, not only by the
         * per-VC program_running flag, which other paths may clear. */
        bool is_sync = proc && proc == exec_get_sync_proc() &&
                       (exec_current_is_sync_program() ||
                        (exec_get_sync_thread_for(proc) &&
                         exec_get_sync_thread_for(proc) == scheduler_get_current_thread()));
        if (is_sync) {
            thread_t *st = (thread_t *)proc->sched_thread;
            if (st && st->owner_process == (struct process *)proc)
                st->owner_process = NULL;
            proc->sched_thread = NULL;
            process_set_fg_pgid(0);
            serial_printf("sys_exit: sync PID %u -> back to Zeus\n", proc->pid);
            exec_notify_exit(status);
            __builtin_unreachable();
        }
    }

    /* Concurrent fork child: become a waitable zombie. */
    if (proc && proc->sched_thread && proc->parent &&
        proc->parent != &processes[0]) {
        fork_child_zombie_exit(status);
        __builtin_unreachable();
    }

    /* Spawned (preemptively-scheduled) desktop/shell process: thread exit. */
    if (proc && proc->sched_thread) {
        exec_spawned_exit(status);
        __builtin_unreachable();
    }

    /* Regular exit: return to kernel shell via longjmp saved in exec_run_elf. */
    exec_notify_exit(status);
    __builtin_unreachable();
}

int sys_read(int fd, void *buf, size_t count) {
    process_t *proc = get_current_process();

    // Security: Validate file descriptor bounds
    if (fd < 0 || fd >= MAX_OPEN_FILES_SYSCALL || !proc->fds[fd].in_use) {
        return -EBADF;
    }

    /* POSIX: read(fd, buf, 0) returns 0. TCC full_read() relies on this
     * after a complete transfer (it issues a trailing zero-length read). */
    if (count == 0)
        return 0;

    if (!buf || !access_ok_user_range((uint64_t)buf, count)) {
        return -EFAULT;
    }

    if (count > 1048576) {
        return -EINVAL;
    }
    
    /* Honour dup2(pipe,0): only use the console stdin path when fd 0 is
     * still a TTY-like slot, not a pipe/file. */
    if (fd == STDIN_FILENO &&
        proc->fds[fd].fd_type != FD_TYPE_PIPE_READ &&
        proc->fds[fd].fd_type != FD_TYPE_VFS &&
        proc->fds[fd].fd_type != FD_TYPE_PIPE_WRITE) {
        char *kbuf = (char *)kalloc(count);
        if (!kbuf) return -ENOMEM;
        size_t got = 0;
        for (size_t i = 0; i < count; i++) {
            int ci = sys_getchar_intr(proc);
            if (ci < 0) {
                if (got == 0) { kfree(kbuf); return -EINTR; }
                break;
            }
            char ch = (char)ci;
            kbuf[got++] = ch;
            if (ch == '\n')
                break;
        }
        if (copy_to_user(buf, kbuf, got) != 0) {
            kfree(kbuf);
            return -EFAULT;
        }
        kfree(kbuf);
        return (int)got;
    } else if (proc->fds[fd].fd_type == FD_TYPE_VFS) {
        void *kbuf = kalloc(count);
        if (!kbuf) return -ENOMEM;
        extern void shmf_before_read(int vfs_fd);
        shmf_before_read(proc->fds[fd].vfs_fd);   /* dirty MAP_SHARED pages first */
        int bytes_read = vfs_read(proc->fds[fd].vfs_fd, kbuf, count);
        if (bytes_read >= 0) {
            if (copy_to_user(buf, kbuf, (size_t)bytes_read) != 0) {                kfree(kbuf);
                return -EFAULT;
            }
            proc->fds[fd].position += bytes_read;
        }
        kfree(kbuf);
        return bytes_read;
    } else if (proc->fds[fd].fd_type == FD_TYPE_PIPE_READ) {
        pipe_t *p = (pipe_t *)proc->fds[fd].file_handle;
        if (!p) return -EBADF;
        void *kbuf = kalloc(count);
        if (!kbuf) return -ENOMEM;
        for (;;) {
            int n = pipe_read(p, kbuf, count);
            if (n == -EAGAIN) {
                kfree(kbuf);
                if (proc->fds[fd].flags & O_NONBLOCK)
                    return -EAGAIN;
                /* About to block: a held fork newborn (often the writer) must
                 * run now or this read never completes. */
                fork_release_pending(proc);
                if (global_scheduler.scheduler_running) {
                    extern thread_t *scheduler_get_current_thread(void);
                    thread_t *self = scheduler_get_current_thread();
                    if (self)
                        pipe_set_waiting_reader(p, self);
                    thread_sleep(1);
                    if (proc_signal_interrupts(get_current_process()))
                        return -EINTR;
                } else
                    return 0;
                kbuf = kalloc(count);
                if (!kbuf) return -ENOMEM;
                continue;
            }
            if (n > 0 && copy_to_user(buf, kbuf, (size_t)n) != 0) {
                kfree(kbuf);
                return -EFAULT;
            }
            kfree(kbuf);
            return n;
        }
    } else if (proc->fds[fd].fd_type == FD_TYPE_VFS_DIR) {
        return -EISDIR;
    }
    return -EBADF;
}


int sys_write(int fd, const void *buf, size_t count) {
    process_t *proc = get_current_process();

    if (fd < 0 || fd >= MAX_OPEN_FILES_SYSCALL || !proc->fds[fd].in_use) {
        return -EBADF;
    }

    if (count == 0)
        return 0;

    if (!buf || !access_ok_user_range((uint64_t)buf, count)) {
        return -EFAULT;
    }

    if (count > 1048576) {
        return -EINVAL;
    }

    /* Pipe/file must win over the fd==1/2 console shortcut so dup2(pipe,1)
     * actually redirects (ash pipelines, `> file` via busybox). */
    if (proc->fds[fd].fd_type == FD_TYPE_PIPE_WRITE) {
        pipe_t *pp = (pipe_t *)proc->fds[fd].file_handle;
        if (!pp) return -EBADF;
        void *kbuf = kalloc(count);
        if (!kbuf) return -ENOMEM;
        if (copy_from_user(kbuf, buf, count) != 0) {
            kfree(kbuf);
            return -EFAULT;
        }
        for (;;) {
            int n = pipe_write(pp, kbuf, count);
            if (n == -EAGAIN) {
                if (proc->fds[fd].flags & O_NONBLOCK) {
                    kfree(kbuf);
                    return -EAGAIN;
                }
                fork_release_pending(proc);   /* the reader may be a held newborn */
                if (global_scheduler.scheduler_running) {
                    extern thread_t *scheduler_get_current_thread(void);
                    thread_t *self = scheduler_get_current_thread();
                    if (self)
                        pipe_set_waiting_writer(pp, self);
                    thread_sleep(1);
                    /* Killed while blocked on a full pipe: unwind. */
                    if (proc_kill_pending(proc)) {
                        kfree(kbuf);
                        return -EINTR;
                    }
                } else {
                    kfree(kbuf);
                    return -EAGAIN;
                }
                continue;
            }
            kfree(kbuf);
            if (n == -EPIPE || n < 0) {
                /* Broken pipe: queue SIGPIPE for delivery on syscall return
                 * (process_deliver_signals). handle_pending_signals only for
                 * default terminate — calling it for custom handlers raced
                 * with SIGCHLD and could skip deliver. */
                if (!(proc->blocked_signals & (1u << SIGPIPE)) &&
                    proc->signal_handlers[SIGPIPE] != SIG_IGN) {
                    proc->pending_signals |= (1u << SIGPIPE);
                    sighandler_t sh = proc->signal_handlers[SIGPIPE];
                    if (sh == SIG_DFL || sh == SIG_ERR || !sh)
                        handle_pending_signals(proc);
                }
                return -EPIPE;
            }
            return n;
        }
    } else if (proc->fds[fd].fd_type == FD_TYPE_VFS) {
        void *kbuf = kalloc(count);
        if (!kbuf) return -ENOMEM;
        if (copy_from_user(kbuf, buf, count) != 0) {
            kfree(kbuf);
            return -EFAULT;
        }
        if (proc->fds[fd].flags & O_APPEND) {
            int endpos = vfs_seek(proc->fds[fd].vfs_fd, 0, VFS_SEEK_END);
            if (endpos >= 0) proc->fds[fd].position = (uint32_t)endpos;
        }
        extern volatile int shmf_count;
        extern void shmf_after_write(int vfs_fd, uint64_t pos, const void *kbuf, uint32_t n);
        int wpos = shmf_count ? vfs_seek(proc->fds[fd].vfs_fd, 0, VFS_SEEK_CUR) : -1;
        int bytes_written = vfs_write(proc->fds[fd].vfs_fd, kbuf, count);
        if (bytes_written == -1)
            bytes_written = -EIO;   /* fs drivers return bare -1; that is -EPERM to user */
        if (bytes_written >= 0) {
            proc->fds[fd].position += bytes_written;
            if (wpos >= 0 && bytes_written > 0)   /* keep MAP_SHARED mappings coherent */
                shmf_after_write(proc->fds[fd].vfs_fd, (uint64_t)wpos, kbuf, (uint32_t)bytes_written);
        }
        kfree(kbuf);
        return bytes_written;
    } else if (fd == STDOUT_FILENO || fd == STDERR_FILENO ||
               proc->fds[fd].fd_type == FD_TYPE_DEV_TTY) {
        char *str = (char*)kalloc(count + 1);
        if (!str) return -ENOMEM;
        if (copy_from_user(str, buf, count) != 0) {
            kfree(str);
            return -EFAULT;
        }
        str[count] = '\0';
        extern int term_write_buf(const char *buf, size_t len);
        term_write_buf(str, count);
        kfree(str);
        return (int)count;
    } else if (proc->fds[fd].fd_type == FD_TYPE_VFS_DIR) {
        return -EISDIR;
    }
    return -EBADF;
}

static int open_directory_fd(process_t *proc, const char *abspath, int flags) {
    size_t plen = strlen(abspath) + 1;
    char *copy = (char *)kalloc(plen);
    if (!copy)
        return -ENOMEM;
    memcpy(copy, abspath, plen);

    int fd = allocate_fd(proc);
    if (fd < 0) {
        kfree(copy);
        return -EMFILE;
    }
    proc->fds[fd].file_handle = copy;
    proc->fds[fd].vfs_fd = -1;
    proc->fds[fd].fd_type = FD_TYPE_VFS_DIR;
    proc->fds[fd].ref_count = fd_refcount_create();
    if (!proc->fds[fd].ref_count) {
        kfree(copy);
        free_fd(proc, fd);
        return -ENOMEM;
    }
    proc->fds[fd].flags = flags;
    proc->fds[fd].position = 0;
    proc->fds[fd].cloexec = false;
    return fd;
}

int sys_open(const char *pathname, int flags, mode_t mode) {
    if (!pathname || !is_user_address((uint64_t)pathname)) {
        return -EFAULT;
    }

    char kpath[256];
    if (!strncpy_from_user(kpath, pathname, sizeof(kpath))) {
        return -EFAULT;
    }
    return sys_open_kpath(kpath, flags, mode);
}

/* sys_open on a path that is already in kernel memory (used by the Linux ABI
 * layer, which resolves openat()/dirfd paths itself).  Flags are inDevOS O_*. */
int sys_open_kpath(const char *kpath, int flags, mode_t mode) {
    if (!kpath) {
        return -EFAULT;
    }

    if (kpath[0] == '\0') {
        return -ENOENT;
    }

    if (strstr(kpath, "..") != NULL) {
        return -EINVAL;
    }

    if (flags & ~(O_RDONLY | O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_APPEND | O_NONBLOCK)) {
        return -EINVAL;
    }

    process_t *proc = get_current_process();

    /* Default creat mode if unspecified */
    if (!(flags & O_CREAT))
        mode = 0;
    else if (mode == 0)
        mode = 0666;

    // Resolve path to absolute using process cwd
    char abspath[256];
    if (kpath[0] == '/') {
        size_t kl = strlen(kpath);
        if (kl >= sizeof(abspath)) kl = sizeof(abspath) - 1;
        memmove(abspath, kpath, kl);
        abspath[kl] = '\0';
    } else {
        size_t cwdl = strlen(proc->cwd);
        size_t kl = strlen(kpath);
        if (cwdl + 1 + kl >= sizeof(abspath)) return -ENAMETOOLONG;
        memmove(abspath, proc->cwd, cwdl);
        if (cwdl > 0 && proc->cwd[cwdl - 1] != '/') {
            abspath[cwdl] = '/';
            cwdl++;
        }
        memmove(abspath + cwdl, kpath, kl);
        abspath[cwdl + kl] = '\0';
    }

    // Try VFS first
    if (vfs_has_root_mount()) {
        int existed = vfs_exists(abspath);
        uint32_t vfs_flags = VFS_O_READ;
        if (flags & O_WRONLY) vfs_flags = VFS_O_WRITE;
        else if (flags & O_RDWR)   vfs_flags = VFS_O_READ | VFS_O_WRITE;
        if (flags & O_CREAT)  vfs_flags |= VFS_O_CREATE;
        if (flags & O_TRUNC)  vfs_flags |= VFS_O_TRUNC;
        if (flags & O_APPEND) vfs_flags |= VFS_O_APPEND;

        int vfd = vfs_open(abspath, vfs_flags);
        if (vfd >= 0) {
            int fd = allocate_fd(proc);
            if (fd < 0) {
                vfs_close(vfd);
                return -EMFILE;
            }
            proc->fds[fd].file_handle = NULL;
            proc->fds[fd].vfs_fd = vfd;
            proc->fds[fd].fd_type = FD_TYPE_VFS;
            proc->fds[fd].ref_count = fd_refcount_create();
            if (!proc->fds[fd].ref_count) {
                vfs_close(vfd);
                free_fd(proc, fd);
                return -ENOMEM;
            }
            proc->fds[fd].flags = flags;
            proc->fds[fd].position = 0;
            proc->fds[fd].cloexec = false;
            if ((flags & O_CREAT) && !existed) {
                mode_t eff = (mode & 0777) & ~proc->file_umask;
                perm_set(abspath, proc->uid, proc->gid, (uint16_t)eff);
            }
            return fd;
        }
        vfs_dirent_t dprobe[1];
        if (vfs_readdir(abspath, dprobe, 1) >= 0) {
            if (flags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_APPEND))
                return -EISDIR;
            return open_directory_fd(proc, abspath, flags);
        }
        return vfd == -EROFS ? -EROFS : -ENOENT;
    }

    /* All file access goes through the VFS; mounting a filesystem registers
     * it with the VFS (see mount_filesystem_device / vfs_fat32_register). */
    return -ENOENT;
}

/* Linux close(): fds 0..2 are ordinary descriptors (sys_close keeps the
 * native-app rule of refusing them). */
int sys_close_linux(int fd) {
    process_t *proc = get_current_process();
    if (fd < 0 || fd >= MAX_OPEN_FILES_SYSCALL || !proc->fds[fd].in_use)
        return -EBADF;
    if (fd <= 2 && proc->fds[fd].fd_type == FD_TYPE_FILE &&
        proc->fds[fd].file_handle == NULL) {
        /* Console stdio placeholder: nothing to release. */
        proc->fds[fd].in_use = false;
        proc->fds[fd].fd_type = 0;
        proc->fds[fd].cloexec = false;
        return 0;
    }
    return fd_close_entry(proc, fd);
}

int sys_close(int fd) {
    process_t *proc = get_current_process();
    
    if (fd < 3 || fd >= MAX_OPEN_FILES_SYSCALL || !proc->fds[fd].in_use) {
        return -EBADF;
    }
    return fd_close_entry(proc, fd);
}

int sys_seek(int fd, off_t offset, int whence) {
    process_t *proc = get_current_process();
    
    if (fd < 0 || fd >= MAX_OPEN_FILES_SYSCALL || !proc->fds[fd].in_use) {
        return -EBADF;
    }

    if (proc->fds[fd].fd_type == FD_TYPE_VFS_DIR) {
        return -EISDIR;
    }

    if (proc->fds[fd].fd_type == FD_TYPE_VFS) {
        int vfs_whence = whence;
        int result = vfs_seek(proc->fds[fd].vfs_fd, (int32_t)offset, vfs_whence);
        if (result >= 0) {
            proc->fds[fd].position = (uint32_t)result;
        }
        return result;
    }

    return -EBADF;
}

/* brk/mmap/munmap change (and now free) the caller's pages, so they must act
 * on the address space the calling user code actually runs in: the PML4
 * live in CR3 at syscall entry.  exec_user_as_process() is a heuristic and
 * under SMP can name another process (e.g. the last pipeline member that
 * exec'd); freeing that process's frames/page tables would corrupt it.
 * If the heuristic disagrees with CR3, use CR3's owner (as the #PF handler
 * does).  Kernel/non-usermode callers are returned unchanged. */
static process_t *mm_syscall_owner(process_t *proc) {
    if (!proc || !proc->is_usermode || !proc->pml4)
        return proc;
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    uint64_t *active = (uint64_t *)phys_to_virt(cr3 & PTE_ADDR_MASK);
    if (proc->pml4 == active)
        return proc;
    process_t *owner = process_find_by_pml4(active);
    static int mm_owner_logs;
    if (mm_owner_logs < 8) {
        mm_owner_logs++;
        serial_printf("mm: syscall named PID %u but CR3 belongs to PID %d - using CR3 owner\n",
                      proc->pid, owner ? (int)owner->pid : -1);
    }
    return owner ? owner : proc;
}

void* sys_brk(void *addr) {
    process_t *proc = exec_user_as_process();
    proc = mm_syscall_owner(proc);

    if (addr == NULL) {
        return proc->brk_current;
    }

    if (addr < proc->brk_start) {
        return proc->brk_current;
    }

    void *const old_brk = proc->brk_current;

    /*
     * Ring-3 heap: back every page in [old_brk, addr) with a PTE.
     * Demand paging in idt.c can miss edge cases when brk spans many pages;
     * eager mapping matches Linux brk semantics and fixes DOOM's multi-MiB zone.
     */
    if (addr > old_brk && proc->is_usermode && proc->pml4) {
        uint64_t const hi = (uint64_t)addr;
        for (uint64_t pg = (uint64_t)old_brk & ~(uint64_t)(PAGE_SIZE - 1);
             pg < hi; pg += PAGE_SIZE) {
            if (vmm_is_mapped_in(proc->pml4, pg))
                continue;
            uint64_t frame = vmm_alloc_frame();
            if (!frame) {
                serial_puts("sys_brk: out of physical frames for heap\n");
                return old_brk;
            }
            memset(phys_to_virt(frame), 0, PAGE_SIZE);
            if (vmm_map_page_in(proc->pml4, pg, frame,
                                 VMM_PRESENT | VMM_USER | VMM_WRITABLE | VMM_NX) != 0) {
                serial_puts("sys_brk: vmm_map_page_in failed\n");
                return old_brk;
            }
        }
    }

    /* Shrink: unmap and release the heap pages wholly above the new break
     * (Linux brk semantics).  Without this a brk-down kept every frame
     * mapped until exit. */
    if (addr < old_brk && proc->is_usermode && proc->pml4) {
        uint64_t lo = ((uint64_t)addr + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
        uint64_t hi = ((uint64_t)old_brk + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
        if (hi > lo)
            vmm_unmap_range_release_in(proc->pml4, lo, (hi - lo) / PAGE_SIZE);
    }

    proc->brk_current = addr;
    return addr;
}

static int mmap_region_prepend(process_t *proc, uint64_t v, size_t len) {
    proc_mmap_region_t *r = (proc_mmap_region_t *)kalloc(sizeof(proc_mmap_region_t));
    if (!r)
        return -ENOMEM;
    r->virt = v;
    r->len = len;
    r->next = proc->mmap_regions;
    proc->mmap_regions = r;
    return 0;
}

void *sys_mmap(void *addr, size_t length, int prot, int flags) {
#define MMAP_ERR(e) ((void *)(uintptr_t)((uint64_t)(int64_t)(-(e))))
    process_t *proc = exec_user_as_process();
    proc = mm_syscall_owner(proc);
    if (length == 0 || length > (256ull << 20))
        return MMAP_ERR(EINVAL);
    /* Internal: Linux MAP_SHARED|MAP_ANONYMOUS (lx_mmap) -- pages stay
     * shared across fork (VMM_SHARED) instead of COW. */
    bool kshared = (flags & SYS_MMAP_KSHARED) != 0;
    flags &= ~SYS_MMAP_KSHARED;
    if (!(flags & MAP_ANONYMOUS))
        return MMAP_ERR(EOPNOTSUPP);
    if ((flags & MAP_SHARED))
        return MMAP_ERR(EOPNOTSUPP);

    if (!proc->pml4 || !proc->is_usermode) {
        void *mem = kalloc(length);
        if (!mem)
            return MMAP_ERR(ENOMEM);
        memset(mem, 0, length);
        return mem;
    }

    size_t pages = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t v0;
    if (addr && (flags & MAP_FIXED)) {
        v0 = (uint64_t)addr & ~(uint64_t)(PAGE_SIZE - 1);
        if (!is_user_address(v0) || !is_user_address(v0 + pages * PAGE_SIZE - 1))
            return MMAP_ERR(EINVAL);
        /* MAP_FIXED replaces whatever is mapped there: release the old
         * frames first (vmm_map_page_in would just overwrite the PTEs and
         * leak them). */
        extern void shmf_range(uint64_t *pml4, uint64_t va, uint64_t len, bool unmap);
        shmf_range(proc->pml4, v0, pages * PAGE_SIZE, true);
        vmm_unmap_range_release_in(proc->pml4, v0, pages);
    } else {
        if (proc->mmap_anon_next == 0)
            proc->mmap_anon_next = 0x20000000ULL;
        v0 = (proc->mmap_anon_next + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
        proc->mmap_anon_next = v0 + pages * PAGE_SIZE;
    }

    uint64_t pte_flags = VMM_PRESENT | VMM_USER;
    if (prot & PROT_WRITE)
        pte_flags |= VMM_WRITABLE;
    if (!(prot & PROT_EXEC))
        pte_flags |= VMM_NX;
    if (kshared)
        pte_flags |= VMM_SHARED;
    if (!(prot & PROT_WRITE))
        pte_flags |= VMM_USER_RO;

    for (size_t p = 0; p < pages; p++) {
        uint64_t va = v0 + p * PAGE_SIZE;
        uint64_t frame = vmm_alloc_frame();
        if (!frame) {
            /* Undo the pages already mapped by this call. */
            vmm_unmap_range_release_in(proc->pml4, v0, p);
            return MMAP_ERR(ENOMEM);
        }
        memset(phys_to_virt(frame), 0, PAGE_SIZE);
        if (vmm_map_page_in(proc->pml4, va, frame, pte_flags) != 0) {
            vmm_free_frame(frame);
            vmm_unmap_range_release_in(proc->pml4, v0, p);
            return MMAP_ERR(ENOMEM);
        }
    }

    if (mmap_region_prepend(proc, v0, pages * PAGE_SIZE) != 0) {
        vmm_unmap_range_release_in(proc->pml4, v0, pages);
        return MMAP_ERR(ENOMEM);
    }

    return (void *)v0;
#undef MMAP_ERR
}

/* Map physical device memory (the framebuffer for /dev/fb0) into the
 * caller.  PTEs carry VMM_DEVICE so munmap/exit/fork never free or COW the
 * frames; pte_extra selects the memory type (fb_wc_pte_flags()). */
long sys_mmap_device(uint64_t addr, uint64_t length, int prot, int fixed,
                     uint64_t phys, uint64_t pte_extra) {
    process_t *proc = exec_user_as_process();
    proc = mm_syscall_owner(proc);
    if (!proc || !proc->pml4 || !proc->is_usermode) return -EINVAL;
    if (length == 0 || length > (256ull << 20) || (phys & (PAGE_SIZE - 1))) return -EINVAL;
    size_t pages = (length + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t v0;
    if (addr && fixed) {
        v0 = addr & ~(uint64_t)(PAGE_SIZE - 1);
        if (!is_user_address(v0) || !is_user_address(v0 + pages * PAGE_SIZE - 1))
            return -EINVAL;
        vmm_unmap_range_release_in(proc->pml4, v0, pages);
    } else {
        if (proc->mmap_anon_next == 0)
            proc->mmap_anon_next = 0x20000000ULL;
        v0 = (proc->mmap_anon_next + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
        proc->mmap_anon_next = v0 + pages * PAGE_SIZE;
    }
    uint64_t f = VMM_PRESENT | VMM_USER | VMM_DEVICE | VMM_NX | pte_extra;
    if (prot & PROT_WRITE) f |= VMM_WRITABLE;
    for (size_t i = 0; i < pages; i++) {
        if (vmm_map_page_in(proc->pml4, v0 + i * PAGE_SIZE, phys + i * PAGE_SIZE, f) != 0) {
            vmm_unmap_range_release_in(proc->pml4, v0, i);
            return -ENOMEM;
        }
        __asm__ volatile("invlpg (%0)" : : "r"(v0 + i * PAGE_SIZE) : "memory");
    }
    if (mmap_region_prepend(proc, v0, pages * PAGE_SIZE) != 0) {
        vmm_unmap_range_release_in(proc->pml4, v0, pages);
        return -ENOMEM;
    }
    return (long)v0;
}

/* Change protection bits on an existing user mapping (needed by tcc -run). */
int sys_mprotect(void *addr, size_t length, int prot) {
    process_t *proc = exec_user_as_process();
    if (!proc || !proc->pml4 || !proc->is_usermode)
        return 0;
    if (length == 0)
        return 0;
    uint64_t start = (uint64_t)addr & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t end = ((uint64_t)addr + length + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    if (!is_user_address(start) || !is_user_address(end - 1))
        return -ENOMEM;

    for (uint64_t va = start; va < end; va += PAGE_SIZE) {
        uint64_t *pte = vmm_get_pte_in(proc->pml4, va);
        if (!pte || !(*pte & VMM_PRESENT))
            continue; /* demand-paged later with updated brk/mmap window */
        uint64_t phys = *pte & PTE_ADDR_MASK;
        uint64_t flags = VMM_PRESENT | VMM_USER;
        if (prot & PROT_WRITE)
            flags |= VMM_WRITABLE;
        if (!(prot & PROT_EXEC))
            flags |= VMM_NX;
        /* Device mappings keep their marker and memory type (PAT/PCD/PWT). */
        if (*pte & VMM_DEVICE)
            flags |= *pte & (VMM_DEVICE | VMM_WRITETHRU | VMM_NOCACHE | (1ULL << 7));
        /* Preserve COW bit if set so fork semantics stay intact. */
        if (*pte & VMM_COW)
            flags = (flags & ~VMM_WRITABLE) | VMM_COW;
        flags |= *pte & VMM_SHARED;          /* MAP_SHARED stays shared */
        if (!(prot & PROT_WRITE))
            flags |= VMM_USER_RO;            /* a write must SIGSEGV */
        *pte = phys | flags;
        __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
    }
    return 0;
}


int sys_munmap(void *addr, size_t length) {
    process_t *proc = exec_user_as_process();
    proc = mm_syscall_owner(proc);
    if (!addr || length == 0)
        return -EINVAL;

    if (!proc->pml4 || !proc->is_usermode) {
        kfree(addr);
        return 0;
    }

    uint64_t v = (uint64_t)addr & ~(uint64_t)(PAGE_SIZE - 1);
    size_t pages = (length + ((uint64_t)addr - v) + PAGE_SIZE - 1) / PAGE_SIZE;
    /* Never let a user munmap clear PTEs in the shared kernel half. */
    if (!is_user_address(v) || !is_user_address(v + pages * PAGE_SIZE - 1))
        return -EINVAL;

    /* Clear the PTEs, shoot down every CPU's TLB, then release the frames
     * (refcount-aware: sole owner frees, COW peers just drop a ref). */
    extern void shmf_range(uint64_t *pml4, uint64_t va, uint64_t len, bool unmap);
    shmf_range(proc->pml4, v, pages * PAGE_SIZE, true);   /* MAP_SHARED file write-back */
    vmm_unmap_range_release_in(proc->pml4, v, pages);

    proc_mmap_region_t **pp = &proc->mmap_regions;
    while (*pp) {
        proc_mmap_region_t *r = *pp;
        if (r->virt == v && r->len >= pages * PAGE_SIZE) {
            *pp = r->next;
            kfree(r);
            return 0;
        }
        pp = &r->next;
    }
    return 0;
}

int sys_getpid(void) {
    return get_current_process()->pid;
}

// Find a free process slot
static process_t* allocate_process(void) {
    for (int i = 1; i < MAX_PROCESSES; i++) { // Skip slot 0 (kernel process)
        if (processes[i].pid == 0) {
            /* Full clear — recycled slots must not keep stale fds/pipe credits
             * or clone_shared_vm from a prior occupant. */
            memset(&processes[i], 0, sizeof(process_t));
            return &processes[i];
        }
    }
    return NULL;
}

#define INDEVOS_KSTACK_BYTES (16 * 1024)

static void process_release(process_t *proc) {
    if (!proc) return;
    for (int i = 0; i < MAX_OPEN_FILES_SYSCALL; i++) {
        if (proc->fds[i].in_use)
            fd_close_entry(proc, i);
    }
    if (proc->kernel_stack) {
        kfree(proc->kernel_stack);
        proc->kernel_stack = NULL;
    }
    if (proc->pml4 && proc->is_usermode) {
        if (process_pml4_shared_with_others(proc->pml4, proc)) {
            proc->pml4 = NULL;
        } else {
            vmm_destroy_address_space(proc->pml4);
            proc->pml4 = NULL;
        }
    }
    proc_mmap_region_t *r = proc->mmap_regions;
    while (r) {
        proc_mmap_region_t *next = r->next;
        kfree(r);
        r = next;
    }
    proc->mmap_regions = NULL;
    if (proc->user_symtab) {
        kfree(proc->user_symtab);
        proc->user_symtab = NULL;
    }
    if (proc->user_sym_string_pool) {
        kfree(proc->user_sym_string_pool);
        proc->user_sym_string_pool = NULL;
    }
}


/* ---- concurrent fork helpers ---- */
#define FORK_USER_CS  0x23
#define FORK_USER_SS  0x1B

/* By-value copy of the forking thread's sc_* stash, taken once at the top
 * of fork_spawn_child().  Never a pointer into the per-CPU syscall kstack. */
typedef struct {
    uint64_t rip, rsp, rflags;
    uint64_t r15, r14, r13, r12, rbp, rbx;
    uint64_t r9, r8, r10, rdx, rsi, rdi;
    int      gprs_valid;
} fork_snap_t;

static int fork_queue_child_thread(process_t *parent, process_t *child,
                                   const fork_snap_t *snap) {
    extern void thread_exit(void);
    if (!snap->rip) {
        serial_printf("fork: no saved user RIP — cannot concurrent-fork\n");
        return -EINVAL;
    }
    thread_t *thr = thread_create("fork-child", (void (*)(void *))thread_exit,
                                  NULL, 0, PRIORITY_NORMAL);
    if (!thr)
        return -ENOMEM;

    memset(&thr->saved_regs, 0, sizeof(thread_regs_t));
    thr->saved_regs.rip    = snap->rip;
    thr->saved_regs.rsp    = snap->rsp;
    thr->saved_regs.rflags = snap->rflags | 0x200ULL;
    thr->saved_regs.cs     = FORK_USER_CS;
    thr->saved_regs.ss     = FORK_USER_SS;
    thr->saved_regs.rax    = 0; /* fork child return */
    /* Linux fork preserves all GPRs except rax. Use the by-value snapshot
     * taken at clone entry — never a pointer into the per-CPU kstack. */
    if (snap->gprs_valid) {
        thr->saved_regs.r15 = snap->r15;
        thr->saved_regs.r14 = snap->r14;
        thr->saved_regs.r13 = snap->r13;
        thr->saved_regs.r12 = snap->r12;
        thr->saved_regs.rbp = snap->rbp;
        thr->saved_regs.rbx = snap->rbx;
        thr->saved_regs.r9  = snap->r9;
        thr->saved_regs.r8  = snap->r8;
        thr->saved_regs.r10 = snap->r10;
        thr->saved_regs.rdx = snap->rdx;
        thr->saved_regs.rsi = snap->rsi;
        thr->saved_regs.rdi = snap->rdi;
    }
    thr->has_saved_context = true;
    thr->owner_process     = (struct process *)child;
    /* musl __post_Fork / __errno_location need a live FS base (pthread self).
     * Prefer HW FS (still parent TLS during this syscall); fall back to the
     * parent's thread_t.fs_base (kept in sync by ARCH_SET_FS). */
    {
        thread_t *pt = scheduler_get_current_thread();
        uint64_t fs = thread_rdfsbase();
        if (!fs && pt)
            fs = pt->fs_base;
        thr->fs_base = fs;
        if (!fs)
            serial_printf("fork: WARNING child PID %d fs_base=0 (TLS missing)\n", (int)child->pid);
    }

    child->sched_thread = thr;
    child->fork_parent  = NULL;

    /* Hold BLOCKED until parent blocking-wait/exit. Prior held child moves
     * to fork_release_at_exit so writer+reader start together at wait.
     * Never wake early here (that raced PIPEOK / bad busybox argv). */
    if (parent->fork_pending_release) {
        if (parent->fork_release_at_exit) {
            /* Rare 3rd concurrent hold: wake oldest so we do not leak. */
            serial_printf("fork: 3rd hold ? releasing oldest pending\n");
            fork_release_thread((thread_t *)parent->fork_release_at_exit);
        }
        parent->fork_release_at_exit = parent->fork_pending_release;
        parent->fork_pending_release = NULL;
    }
    thr->state = THREAD_BLOCKED;
    parent->fork_pending_release = thr;
    parent->fork_defer_release = 1;
    return 0;
}



static void fork_release_thread(thread_t *thr) {
    if (!thr || thr->state != THREAD_BLOCKED)
        return;
    thr->state = THREAD_READY;
    scheduler_add_thread(thr);
}

void fork_release_pending(process_t *p) {
    if (!p)
        return;
    if (p->fork_release_at_exit) {
        thread_t *prev = (thread_t *)p->fork_release_at_exit;
        p->fork_release_at_exit = NULL;
        fork_release_thread(prev);
    }
    if (!p->fork_pending_release)
        return;
    thread_t *thr = (thread_t *)p->fork_pending_release;
    p->fork_pending_release = NULL;
    p->fork_defer_release = 0;
    if (!thr || thr->state == THREAD_TERMINATED)
        return;
    fork_release_thread(thr);
}

/* Wait-only hold: never wake held kids on fork syscall exit.
 * Pair sits in fork_release_at_exit + fork_pending_release until parent
 * blocking wait / WNOHANG-with-zombie (lx_wait4) or exit. Releasing the
 * pair at fork2 return let kids run before ash closed pipe ends — wc saw
 * empty stdin and ash raced into a second pipe + GPF. */
void fork_release_after_syscall(process_t *p) {
    if (!p)
        return;
    /* Burn defer counter only; do not wake. */
    if (p->fork_pending_release && p->fork_defer_release > 0)
        p->fork_defer_release--;
}

void process_fault_kill_current(int signum) {
    process_t *proc = get_current_process();
    thread_t *thr = proc ? (thread_t *)proc->sched_thread : NULL;
    int st = 128 + (signum & 0x7f);
    if (!proc) return;
    /*
     * Concurrent fork child: full zombie path (closes pipe ends, wakes parent).
     * Never returns — that path hlt-loops after detaching the thread.
     */
    if (thr && proc->parent && proc->parent != &processes[0]) {
        fork_child_zombie_exit(st);
        __builtin_unreachable();
    }
    /* Sync/exec program on boot thread: close fds then mark zombie
     * (same close-before-zombie rule as fork_child_zombie_exit). */
    proc->exit_status = st;
    proc->is_running = false;
    for (int i = 0; i < MAX_OPEN_FILES_SYSCALL; i++) {
        if (proc->fds[i].in_use)
            fd_close_entry(proc, i);
    }
    proc->is_zombie = true;
    if (proc->parent && proc->parent->sched_thread)
        thread_wake((thread_t *)proc->parent->sched_thread);
    if (proc->parent)
        proc->parent->pending_signals |= (1u << SIGCHLD);
}

/* Exit-time child cleanup.  Zombie children of an exiting process were never
 * reaped (nobody would ever wait for them), so every fork child that exited
 * without waiting leaked process slots (+ page tables) until fork returned
 * -ENOMEM (ostest proc.many_children -> zombies/vfork/sigint_pgrp cascade).
 * Reap zombie children now; hand still-running ones to our parent (Linux
 * reparents to init) so their zombie is collected by its wait(). */
static void process_orphan_children(process_t *proc) {
    extern process_t *get_kernel_process(void);
    if (!proc || !proc->child) return;
    process_t *np = proc->parent;
    bool np_ok = np && np != get_kernel_process() && np != proc &&
                 np->is_running && !np->is_zombie;
    uint64_t f = 0;
    scheduler_irq_lock(&f);
    process_t *list = proc->child;
    proc->child = NULL;
    scheduler_irq_unlock(f);
    int reaped = 0, moved = 0;
    while (list) {
        process_t *c = list;
        list = c->sibling;
        c->sibling = NULL;
        if (c->is_zombie) {
            process_release(c);
            memset(c, 0, sizeof(process_t));
            reaped++;
            continue;
        }
        scheduler_irq_lock(&f);
        if (np_ok) {
            c->parent = np;
            c->sibling = np->child;
            np->child = c;
        } else {
            c->parent = NULL;
        }
        scheduler_irq_unlock(f);
        moved++;
    }
    if (reaped || moved)
        serial_printf("exit: PID %u reaped %d zombie children, reparented %d to %u\n",
                      proc->pid, reaped, moved, np_ok ? np->pid : 0);
}

void fork_child_zombie_exit(int status) {
    /* SMP: never use global current_process — APs overwrite it mid-exit
     * and we would close the sibling's fds (PIPEOK: PID4 exit zombied PID5). */
    process_t *proc = get_current_process();
    thread_t *thr = proc ? (thread_t *)proc->sched_thread : NULL;
    bool exit_nopreempt = false;

    /* If we held a newborn, release it so it is not stuck BLOCKED forever. */
    if (proc)
        fork_release_pending(proc);

    serial_printf("fork-child-exit: PID %d status=%d -> zombie\n",
                  proc ? (int)proc->pid : -1, status);
    process_orphan_children(proc);

    if (proc) {
        proc->alarm_deadline_ms = 0;
        proc->exit_status = status;
        proc->is_running  = false;
        /* Close FDs BEFORE is_zombie. Otherwise waitpid on another CPU can
         * reap and resume the parent while pipe reader/writer credits are
         * still held — pipe_sigpipe second write then succeeds (n=1 got=0)
         * instead of EPIPE/SIGPIPE. */
        for (int i = 0; i < MAX_OPEN_FILES_SYSCALL; i++) {
            if (proc->fds[i].in_use)
                fd_close_entry(proc, i);
        }
        /* Do NOT destroy the user AS here. COW pages are still shared with
         * the parent (and sibling fork children). Freeing now unrefs those
         * frames and causes parent/sibling stack #PF + kfree double-free
         * spam. wait()/process_cleanup reaps the AS later. */
        /* From here until the hlt loop this thread must not be switched
         * out: once owner_process is cleared it is preemptible, and a
         * TERMINATED thread is never resumed -- the rest of this exit path
         * (zombie publish, parent wake) would be lost.  (The old code only
         * survived this because detach aliased the CPU's idle thread onto
         * the dying context.)  Interrupts stay on so TLB IPIs are answered. */
        preempt_disable();
        exit_nopreempt = true;
        if (thr) {
            thr->state = THREAD_TERMINATED;
            thr->owner_process = NULL;
            scheduler_detach_thread(thr);
            proc->sched_thread = NULL;
        }
        /* This CPU is running on proc->kernel_stack and hlt-loops on it until
         * the timer switches it away, but once is_zombie is published the
         * parent may reap the slot on another CPU -- process_release() used to
         * kfree this stack right under us.  Hand the stack and the thread to
         * the deferred reaper instead (the thread itself was never freed). */
        {
            void *ks = proc->kernel_stack;
            proc->kernel_stack = NULL;
            thread_mark_dead(thr, ks);
        }
        if (proc->fork_parent) {
            process_t *vp = proc->fork_parent;
            proc->fork_parent = NULL;
            if (vp->sched_thread)
                thread_wake((thread_t *)vp->sched_thread);
        }
        /* Publish is_zombie + wake BLOCKED waiter under scheduler_lock —
         * same lock sys_wait uses for block-before-scan (lost-wakeup safe).
         * Wake is no-op unless parent is BLOCKED/SLEEPING (never re-add READY). */
        {
            process_t *par = proc->parent;
            thread_t *pt = (par && par->sched_thread)
                               ? (thread_t *)par->sched_thread : NULL;
            uint64_t zf = 0;
            scheduler_irq_lock(&zf);
            proc->is_zombie = true;
            if (pt)
                thread_wake_blocked_unlocked(pt);
            scheduler_irq_unlock(zf);
            if (pt) {
                scheduler_kick_thread(pt);
            }
        }
        if (proc->parent)
            proc->parent->pending_signals |= (1u << SIGCHLD);
    }

    /* This CPU hlt-loops — never SYSRET. Do NOT set_current_process(parent):
     * ash/zeus often run on the BSP boot thread with owner_process==NULL, so
     * get_current_process() falls back to the global. Pointing it at parent
     * made nest_fork's exiting grandchild poison the global to nest_fork
     * (PID 5); ash's next fork then logged "parent 5" with ash RIP 0x4b0e09
     * and #PF (page not present). Use the kernel process + kernel PML4. */
    {
        extern process_t *get_kernel_process(void);
        extern uint64_t *vmm_get_kernel_pml4(void);
        process_t *fallback = get_kernel_process();
        set_current_process(fallback);
        vmm_switch_address_space(vmm_get_kernel_pml4());
        exec_set_ring3_active(NULL);
    }

    /* Done: the next timer tick may now switch this dead thread out. */
    if (exit_nopreempt)
        preempt_enable();
    __asm__ volatile("sti");
    for (;;)
        __asm__ volatile("hlt");
}



/*
 * Shared fork/clone body.
 * share_vm: CLONE_VM — share parent pml4 (no COW).
 * newsp: child user stack (clone newsp); 0 = same RSP as the parent.
 * Everything else (fds memset+copy, pipe_fork_dup_*, signals, queue,
 * stash clear) matches classic sys_fork.
 * On success returns child pid and sets *out_child; on failure negative errno.
 */
static int fork_spawn_child(bool share_vm, uint64_t newsp, process_t **out_child) {
    /* Must use get_current_process() — the current_process global is
     * overwritten by every CPU's scheduler (see get_current_process). */
    process_t *parent = get_current_process();
    if (!parent)
        return -EINVAL;
    /* Copy THIS thread's sc_* entry stash by value before any long work
     * (vmm_clone).  It is the single source of truth for the child's user
     * context: written on every SYSCALL / INT 0x80 entry (and refreshed by
     * fork_snapshot_frame), never touched by another thread. */
    thread_t *pthr = scheduler_get_current_thread();
    fork_snap_t snap;
    memset(&snap, 0, sizeof(snap));
    if (pthr && pthr->sc_frame_valid) {
        snap.rip = pthr->sc_user_rip;
        snap.rsp = pthr->sc_user_rsp;
        snap.rflags = pthr->sc_user_rflags;
        snap.r15 = pthr->sc_r15; snap.r14 = pthr->sc_r14;
        snap.r13 = pthr->sc_r13; snap.r12 = pthr->sc_r12;
        snap.rbp = pthr->sc_rbp; snap.rbx = pthr->sc_rbx;
        snap.r9 = pthr->sc_r9; snap.r8 = pthr->sc_r8;
        snap.r10 = pthr->sc_r10; snap.rdx = pthr->sc_rdx;
        snap.rsi = pthr->sc_rsi; snap.rdi = pthr->sc_rdi;
        snap.gprs_valid = 1;
    }
    /* Child stack: honor clone newsp (musl __clone pushes fn/arg there). */
    if (newsp && snap.rip)
        snap.rsp = newsp;
    process_t *child = allocate_process();

    if (out_child)
        *out_child = NULL;
    if (!child)
        return -ENOMEM;

    child->pid = next_pid++;
    strcpy(child->cwd, parent->cwd);
    child->brk_start = parent->brk_start;
    child->brk_current = parent->brk_current;
    child->memory_base = parent->memory_base;
    child->memory_size = parent->memory_size;
    child->virtual_base = parent->virtual_base;
    child->is_running = true;
    child->is_zombie = false;
    child->is_usermode = parent->is_usermode;
    child->exit_status = 0;
    child->parent = parent;
    child->child = NULL;
    child->sibling = parent->child;
    parent->child = child;
    child->uid = parent->uid;
    child->gid = parent->gid;
    child->file_umask = parent->file_umask;
    child->cont_tty = parent->cont_tty;
    child->user_stack_top = parent->user_stack_top;
    child->mmap_anon_next = parent->mmap_anon_next;
    child->linux_abi = parent->linux_abi;
    child->fork_pending_release = NULL;
    child->fork_release_at_exit = NULL;
    child->fork_defer_release = 0;
    child->linux_ti_valid = false;
    child->linux_clear_child_tid = 0;
    child->clone_shared_vm = false;
    kstrncpy(child->linux_exe_path, parent->linux_exe_path,
             sizeof(child->linux_exe_path));
    child->mmap_regions = NULL;
    child->pending_signals = 0;
    child->alarm_deadline_ms = 0;   /* POSIX: pending alarms are not inherited */
    child->alarm_interval_ms = 0;
    child->pgid = parent->pgid ? parent->pgid : parent->pid;
    child->sid  = parent->sid  ? parent->sid  : parent->pid;
    child->sig_in_handler = false;
    child->blocked_signals = parent->blocked_signals;
    for (int i = 0; i < NSIG; i++) {
        child->signal_handlers[i] = parent->signal_handlers[i];
        child->signal_sa_flags[i] = parent->signal_sa_flags[i];
        child->sig_restorer[i] = parent->sig_restorer[i];
    }
    child->user_symtab = NULL;
    child->user_symtab_count = 0;
    child->user_sym_string_pool = NULL;

    if (parent->pml4 && parent->is_usermode) {
        uint8_t *ks = (uint8_t *)kalloc(INDEVOS_KSTACK_BYTES);
        if (!ks) {
            child->pid = 0;
            parent->child = child->sibling;
            memset(child, 0, sizeof(process_t));
            return -ENOMEM;
        }
        child->kernel_stack = ks;
        child->kernel_stack_top = (uint64_t)ks + INDEVOS_KSTACK_BYTES;
        if (share_vm) {
            child->pml4 = parent->pml4;
            child->clone_shared_vm = true;
            parent->clone_shared_vm = true;
        } else {
            uint64_t *newpml4 = vmm_clone_address_space(parent->pml4);
            if (!newpml4) {
                kfree(ks);
                child->kernel_stack = NULL;
                child->kernel_stack_top = 0;
                child->pid = 0;
                parent->child = child->sibling;
                memset(child, 0, sizeof(process_t));
                return -ENOMEM;
            }
            child->pml4 = newpml4;
        }
    } else if (parent->kernel_stack) {
        uint8_t *ks = (uint8_t *)kalloc(INDEVOS_KSTACK_BYTES);
        if (!ks) {
            child->pid = 0;
            parent->child = child->sibling;
            memset(child, 0, sizeof(process_t));
            return -ENOMEM;
        }
        child->kernel_stack = ks;
        child->kernel_stack_top = (uint64_t)ks + INDEVOS_KSTACK_BYTES;
    }

    if (!share_vm) {
        for (proc_mmap_region_t *r = parent->mmap_regions; r; r = r->next) {
            proc_mmap_region_t *nr = (proc_mmap_region_t *)kalloc(sizeof(proc_mmap_region_t));
            if (!nr)
                continue;
            nr->virt = r->virt;
            nr->len = r->len;
            nr->next = child->mmap_regions;
            child->mmap_regions = nr;
        }
    } else {
        child->mmap_regions = NULL;
    }

    /* Clear slot leftovers — stale in_use pipe fds from a recycled
     * process_t would otherwise survive (fork only overwrites parent-open
     * indices) and skew pipe readers/writers credits. */
    memset(child->fds, 0, sizeof(child->fds));
    for (int i = 0; i < MAX_OPEN_FILES_SYSCALL; i++) {
        if (!parent->fds[i].in_use)
            continue;
        child->fds[i] = parent->fds[i];
        child->fds[i].in_use = true;
        if (child->fds[i].ref_count)
            (*child->fds[i].ref_count)++;
        if (child->fds[i].fd_type == FD_TYPE_PIPE_READ) {
            pipe_t *pp = (pipe_t *)child->fds[i].file_handle;
            pipe_fork_dup_reader(pp);
        } else if (child->fds[i].fd_type == FD_TYPE_PIPE_WRITE) {
            pipe_t *pp = (pipe_t *)child->fds[i].file_handle;
            pipe_fork_dup_writer(pp);
        }
    }

    child->fork_parent = NULL;

    if (!snap.rip) {
        /* Linux ABI must always snapshot the live syscall frame before fork.
         * Taking the setjmp path with rip=0 yields User-mode fault at RIP 0
         * (PIPEOK ash pipelines). Native INT80 can still use setjmp. */
        if (parent->linux_abi) {
            serial_printf("fork: linux_abi missing saved user RIP (pid=%d) — EINVAL\n",
                          (int)parent->pid);
            for (int i = 0; i < MAX_OPEN_FILES_SYSCALL; i++) {
                if (child->fds[i].in_use)
                    fd_close_entry(child, i);
            }
            if (child->pml4 && !child->clone_shared_vm) {
                vmm_destroy_address_space(child->pml4);
                child->pml4 = NULL;
            } else {
                child->pml4 = NULL;
            }
            if (child->kernel_stack) {
                kfree(child->kernel_stack);
                child->kernel_stack = NULL;
                child->kernel_stack_top = 0;
            }
            if (parent->child == child)
                parent->child = child->sibling;
            memset(child, 0, sizeof(process_t));
            return -EINVAL;
        }
        serial_printf("fork: legacy vfork path (no saved user RIP)\n");
        int resumed = exec_setjmp((exec_jmpbuf_t *)parent->fork_jmpbuf);
        if (resumed != 0) {
            if (parent->kernel_stack_top)
                syscall_set_kernel_stack(parent->kernel_stack_top);
            if (parent->pml4)
                vmm_switch_address_space(parent->pml4);
            exec_set_ring3_active(parent);
            /* Legacy vfork path: GPR snapshot already held by-value. */
            return resumed;
        }
        child->fork_parent = parent;
        current_process = child;
        exec_set_ring3_active(child);
        if (!child->kernel_stack_top ||
            child->kernel_stack_top == parent->kernel_stack_top) {
            current_process = parent;
            exec_set_ring3_active(parent);
            child->fork_parent = NULL;
            memset(child, 0, sizeof(process_t));
            return -ENOMEM;
        }
        syscall_set_kernel_stack(child->kernel_stack_top);
        if (child->pml4)
            vmm_switch_address_space(child->pml4);
        if (out_child)
            *out_child = child;
        return 0;
    }

    {
        int qr = fork_queue_child_thread(parent, child, &snap);
        if (qr < 0) {
            serial_printf("fork: queue child failed %d\n", qr);
            /* Undo pipe credits before dropping the slot. */
            for (int i = 0; i < MAX_OPEN_FILES_SYSCALL; i++) {
                if (child->fds[i].in_use)
                    fd_close_entry(child, i);
            }
            if (child->pml4 && !child->clone_shared_vm) {
                vmm_destroy_address_space(child->pml4);
                child->pml4 = NULL;
            } else {
                child->pml4 = NULL;
            }
            if (child->kernel_stack) {
                kfree(child->kernel_stack);
                child->kernel_stack = NULL;
                child->kernel_stack_top = 0;
            }
            if (parent->child == child)
                parent->child = child->sibling;
            memset(child, 0, sizeof(process_t));
            return qr;
        }
    }

    /* Drop THIS thread's entry stash so the next fork in this thread must
     * re-capture on its own SYSCALL entry. Never touch another thread's
     * sc_* fields. */
    if (pthr) {
        pthr->sc_frame_valid = 0;
        pthr->sc_user_rip = 0;
        pthr->sc_user_rsp = 0;
        pthr->sc_user_rflags = 0;
    }

    if (out_child)
        *out_child = child;
    return (int)child->pid;
}

int sys_fork(void) {
    /* Plain COW fork — same helper clone(SIGCHLD) uses without CLONE_VM. */
    return fork_spawn_child(false, 0, NULL);
}

int sys_clone(uint64_t flags, uint64_t newsp, uint64_t parent_tidptr,
              uint64_t child_tidptr, uint64_t tls) {
    process_t *parent = get_current_process();
    process_t *child = NULL;
    bool share_vm = (flags & CLONE_VM) != 0;
    bool do_vfork = (flags & CLONE_VFORK) != 0;


    int pid = fork_spawn_child(share_vm, newsp, &child);
    if (pid < 0)
        return pid;
    /* Legacy setjmp path returns 0 in the child with child==NULL out — done. */
    if (!child)
        return pid;

    if (flags & CLONE_CHILD_CLEARTID)
        child->linux_clear_child_tid = child_tidptr;

    if (flags & CLONE_SETTLS) {
        thread_t *thr = (thread_t *)child->sched_thread;
        if (thr)
            thr->fs_base = tls;
    }

    if ((flags & CLONE_PARENT_SETTID) && parent_tidptr) {
        int tid = (int)child->pid;
        (void)copy_to_user((void *)parent_tidptr, &tid, sizeof(tid));
    }
    if ((flags & CLONE_CHILD_SETTID) && child_tidptr) {
        int tid = (int)child->pid;
        (void)copy_to_user((void *)child_tidptr, &tid, sizeof(tid));
    }

    /* Threads (pthread_create: CLONE_VM | CLONE_THREAD, no CLONE_VFORK) must
     * start now: the hold-until-wait policy for fork children would leave a
     * new thread BLOCKED forever (curl's threaded resolver never ran). */
    if (share_vm && !do_vfork && (flags & 0x00010000ULL /* CLONE_THREAD */))
        fork_release_pending(parent);

    if (do_vfork) {
        /* Child was queued BLOCKED with defer; must run while we wait
         * in-kernel -- release now or this loops forever. */
        child->fork_parent = parent;
        fork_release_pending(parent);
        while (child->fork_parent == parent) {
            if (child->is_zombie) {
                child->fork_parent = NULL;
                break;
            }
            /* Linux waits TASK_KILLABLE here: a fatal signal (Ctrl-C with
             * default action, remote kill) ends the wait and the parent dies
             * at the syscall-return safe point. Handled signals stay pending
             * until the child execs or exits, as on Linux. */
            if (proc_kill_pending(parent)) {
                serial_printf("vfork: parent %u wait interrupted by signal\n", parent->pid);
                child->fork_parent = NULL;
                break;
            }
            thread_sleep(1);
        }
    }

    return pid;
}





/* waitpid() pid selector: >0 that child, -1 any, 0 caller's pgrp, <-1 pgrp. */
static bool wait_match(process_t *parent, process_t *c, long wpid) {
    if (wpid > 0)  return c->pid == (uint32_t)wpid;
    if (wpid == 0) return c->pgid == parent->pgid;
    if (wpid < -1) return c->pgid == (uint32_t)(-wpid);
    return true;
}
static bool wait_has_match(process_t *parent, long wpid) {
    for (process_t *c = parent->child; c; c = c->sibling)
        if (wait_match(parent, c, wpid)) return true;
    return false;
}

int sys_wait(int *status) { return sys_wait_pid(-1, status); }

/* Blocking wait for a child selected by @wpid (sys_wait used to reap the
 * first zombie whatever pid waitpid() asked for). */
int sys_wait_pid(long wpid, int *status) {
    /* SMP: never use the global current_process — APs overwrite it. */
    process_t *parent = get_current_process();
    /* Wake held newborns before blocking. */
    if (parent)
        fork_release_pending(parent);

    for (;;) {
        if (!wait_has_match(parent, wpid))
            return -ECHILD;

        /* Lost-wakeup protocol: under scheduler_lock, set BLOCKED THEN scan
         * for zombies. Child marks is_zombie + wake under the same lock.
         * If a zombie is found, restore RUNNING and reap; else unlock and
         * yield (stay BLOCKED until wake: BLOCKED->READY only). */
        process_t *zombie = NULL;
        uint64_t wf = 0;
        scheduler_irq_lock(&wf);
        thread_block_current_unlocked();
        for (process_t *c = parent->child; c; c = c->sibling) {
            if (c->is_zombie && wait_match(parent, c, wpid)) {
                zombie = c;
                break;
            }
        }
        if (zombie || !wait_has_match(parent, wpid)) {
            thread_unblock_current_running_unlocked();
            scheduler_irq_unlock(wf);
            if (!zombie)
                return -ECHILD;
        } else if (proc_signal_interrupts(parent)) {
            thread_unblock_current_running_unlocked();
            scheduler_irq_unlock(wf);
            return -EINTR;
        } else {
            scheduler_irq_unlock(wf);
            if (!global_scheduler.scheduler_running)
                return -ECHILD;
            /* Park: state is BLOCKED so yield will not re-queue us. */
            thread_yield();
            continue;
        }

        if (status) {
            if (copy_to_user(status, &zombie->exit_status, sizeof(zombie->exit_status)) != 0)
                return -EFAULT;
        }

        pid_t child_pid = zombie->pid;

        if (parent->child == zombie) {
            parent->child = zombie->sibling;
        } else {
            process_t *prev = parent->child;
            while (prev && prev->sibling != zombie)
                prev = prev->sibling;
            if (prev)
                prev->sibling = zombie->sibling;
        }

        process_release(zombie);
        memset(zombie, 0, sizeof(process_t));
        return child_pid;
    }
}

/* Terminate a process from the signal path.
 * - Called on the process's OWN thread (self kill, default fatal signal being
 *   delivered on its syscall return): just mark it; the caller's return path
 *   exits through sys_exit (fork child -> zombie, spawned -> exec_spawned_exit,
 *   sync program -> back to Zeus).
 * - Anything else is an asynchronous kill.  It used to tear the target down
 *   right here (thread TERMINATED + detached, and for kernel-parented
 *   processes process_release()/process_cleanup() freed its address space,
 *   fds and slot) while that thread could be running on another CPU in the
 *   middle of a syscall -- holding a klock (VFS/FAT/net), pipe credits or a
 *   half-built kernel object, or still using the page tables being freed.  It
 *   now only posts the kill (process_request_kill); the target unwinds and
 *   exits itself at a safe point. */
static void process_signal_terminate(process_t *proc, int signum) {
    if (!proc) return;
    if (!process_is_self(proc)) {
        process_request_kill(proc, signum);
        return;
    }
    if (proc->kill_sig)
        signum = proc->kill_sig;    /* report the signal that killed it */
    proc->exit_status = 128 + signum;
    proc->is_running  = false;
    /* Linux ABI: do NOT publish is_zombie while still running -- a parent on
     * another CPU could reap (memset) us.  The syscall return path exits via
     * sys_exit (fork_child_zombie_exit). */
    if (!proc->linux_abi) proc->is_zombie = true;
}

// Handle pending signals for a process
static void handle_pending_signals(process_t *proc) {
    for (int signum = 1; signum < NSIG; signum++) {
        /* A blocked signal stays pending (kill() to self with the signal
         * blocked must not run its default action; signalfd/sigtimedwait
         * dequeue it). KILL/STOP can never be in blocked_signals. */
        if (proc->blocked_signals & (1u << signum)) continue;
        if (proc->pending_signals & (1 << signum)) {
            // Clear the pending signal
            proc->pending_signals &= ~(1 << signum);
            
            sighandler_t handler = proc->signal_handlers[signum];
            
            if (handler == SIG_IGN) {
                /* ignore */
            } else if (handler == SIG_DFL) {
                // Default action
                switch (signum) {
                    case SIGTERM:
                    case SIGINT:
                    case SIGQUIT:
                    case SIGKILL:
                    case SIGPIPE:
                        // Terminate process
                        process_signal_terminate(proc, signum);
                        return; /* proc may be reaped — stop iterating */
                    case SIGCHLD:
                    case 28: /* SIGWINCH */
                    case 23: /* SIGURG */
                        // Ignore by default
                        break;
                    default:
                        // Most signals terminate by default
                        process_signal_terminate(proc, signum);
                        return;
                }
            } else {
                /* Custom handler: leave bit set; deliver on return-to-user
                 * (see process_deliver_signals). Re-set pending for that path. */
                proc->pending_signals |= (1u << signum);
                return;
            }
        }
    }
}

// Signal handling functions
sighandler_t sys_signal(int signum, sighandler_t handler) {
    if (signum < 1 || signum >= NSIG) {
        return SIG_ERR;
    }
    
    process_t *proc = get_current_process();
    sighandler_t old_handler = proc->signal_handlers[signum];
    
    // Some signals cannot be caught or ignored
    if (signum == SIGKILL || signum == SIGSTOP) {
        return SIG_ERR;
    }

    proc->signal_handlers[signum] = handler;
    return old_handler;
}

int sys_kill(pid_t pid, int signum) {
    if (signum < 1 || signum >= NSIG) {
        return -EINVAL;
    }
    
    process_t *sender = get_current_process();
    
    process_t *target = NULL;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (processes[i].pid == (uint32_t)pid && processes[i].is_running) {
            target = &processes[i];
            break;
        }
    }
    
    if (!target) {
        return -ESRCH;
    }
    
    if (sender->uid != 0 && sender->uid != target->uid) {
        return -EPERM;
    }
    
    if (target == sender) {
        target->pending_signals |= (1 << signum);
        handle_pending_signals(target);
    } else {
        process_post_signal(target, signum);
    }
    
    return 0;
}


int sys_setpgid(pid_t pid, pid_t pgid) {
    process_t *cur = get_current_process();
    if (!cur) return -EINVAL;
    if (pid == 0) pid = (pid_t)cur->pid;
    if (pgid == 0) pgid = pid;
    if (pgid < 0) return -EINVAL;
    process_t *t = NULL;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (processes[i].pid == (uint32_t)pid &&
            (processes[i].is_running || processes[i].is_zombie)) {
            t = &processes[i];
            break;
        }
    }
    if (!t) return -ESRCH;
    /* Only self or child. */
    if (t != cur && t->parent != cur) return -EPERM;
    t->pgid = (uint32_t)pgid;
    return 0;
}
int sys_getpgid(pid_t pid) {
    process_t *cur = get_current_process();
    if (pid == 0) return cur ? (int)cur->pgid : -ESRCH;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (processes[i].pid == (uint32_t)pid &&
            (processes[i].is_running || processes[i].is_zombie))
            return (int)processes[i].pgid;
    }
    return -ESRCH;
}
int sys_setsid(void) {
    process_t *cur = get_current_process();
    if (!cur) return -EINVAL;
    /* Fail if already a leader of a group with others — simplified OK. */
    cur->sid = cur->pid;
    cur->pgid = cur->pid;
    return (int)cur->pid;
}

/* Foreground process group for the console (Ctrl+C target). */
static uint32_t console_fg_pgid = 0;
void process_set_fg_pgid(uint32_t pgid) { console_fg_pgid = pgid; }
extern process_t *exec_get_sync_proc(void);
extern thread_t *exec_get_sync_thread_for(process_t *p);
uint32_t process_get_fg_pgid(void) {
    if (console_fg_pgid) {
        for (int i = 0; i < MAX_PROCESSES; i++)
            if (processes[i].is_running && processes[i].pgid == console_fg_pgid)
                return console_fg_pgid;
    }
    process_t *sp = exec_get_sync_proc();
    if (sp && sp->is_running)
        return sp->pgid ? sp->pgid : sp->pid;
    process_t *c = get_current_process();
    return c ? c->pgid : 0;
}

/* Signals that interrupt a blocking syscall (-EINTR): pending, unblocked,
 * not ignored. SIGCHLD/SIGCONT/SIGWINCH/SIGURG never interrupt here (keeps
 * ash's wait/SIGCHLD flow unchanged). */
bool proc_signal_interrupts(process_t *p) {
    if (!p) return false;
    if (__atomic_load_n(&p->kill_sig, __ATOMIC_ACQUIRE))
        return true;                /* remote kill: always interrupts */
    uint32_t pend = __atomic_load_n(&p->pending_signals, __ATOMIC_ACQUIRE) &
                    ~p->blocked_signals;
    if (!pend) return false;
    for (int s = 1; s < NSIG; s++) {
        if (!(pend & (1u << s))) continue;
        if (s == SIGCHLD || s == SIGCONT || s == 28 /*WINCH*/ || s == 23 /*URG*/)
            continue;
        if (p->signal_handlers[s] == SIG_IGN && s != SIGKILL)
            continue;
        return true;
    }
    return false;
}

/* Pending fatal signal with default action (for ring-3 kill from the timer). */
int proc_fatal_default_pending(process_t *p) {
    if (!p || !p->is_running) return 0;
    int ks = __atomic_load_n(&p->kill_sig, __ATOMIC_ACQUIRE);
    if (ks)
        return ks;
    uint32_t pend = p->pending_signals & ~p->blocked_signals;
    if (!pend) return 0;
    static const int fatal[] = { SIGKILL, SIGINT, SIGQUIT, SIGTERM, SIGHUP, SIGALRM };
    for (unsigned i = 0; i < sizeof(fatal) / sizeof(fatal[0]); i++) {
        int s = fatal[i];
        if (!(pend & (1u << s))) continue;
        if (s == SIGKILL || p->signal_handlers[s] == SIG_DFL)
            return s;
    }
    return 0;
}

/* Post a signal to another (or any) process without running its default
 * action from the sender's context: mark pending and wake it so a blocking
 * syscall returns -EINTR and delivery happens on its own return path. */
/* The thread that executes @t (its own thread, or the boot thread for the
 * foreground sync program). */
static thread_t *process_exec_thread(process_t *t) {
    thread_t *thr = (thread_t *)t->sched_thread;
    if (!thr)
        thr = exec_get_sync_thread_for(t);
    return thr;
}

/* Make @thr notice a just-posted signal promptly: wake it if it sleeps
 * interruptibly (BLOCKED/SLEEPING -> READY, the syscall then sees the signal
 * and returns -EINTR), and IPI any other CPU it is running on so the
 * reschedule IPI's IRQ-return-to-user check (tty_timer_hook) acts at once
 * instead of on the next tick. */
static void process_kick_thread(thread_t *thr) {
    if (!thr) return;
    thread_wake(thr);
    uint32_t me = smp_cpu_index_fast();
    for (uint32_t c = 0; c < MAX_CPUS; c++) {
        if (c == me || !cpu_data[c].online) continue;
        if (cpu_data[c].current_thread == thr)
            smp_reschedule_cpu(c);
    }
}

void process_post_signal(process_t *t, int signum) {
    if (!t || signum < 1 || signum >= NSIG) return;
    __atomic_or_fetch(&t->pending_signals, 1u << signum, __ATOMIC_SEQ_CST);
    process_kick_thread(process_exec_thread(t));
}

/* ---- alarm()/setitimer(ITIMER_REAL) ----
 * Each process keeps an absolute deadline.  The BSP's LAPIC tick scans the
 * table once any alarm was ever armed and, on expiry, posts SIGALRM through
 * process_post_signal(): pending bit + wake, exactly like kill(), so a thread
 * blocked in waitpid/read/poll/nanosleep sees it and returns -EINTR, and a
 * ring-3 busy loop gets it on the IRQ-return path. */
static volatile int alarm_ever_armed;

/* Arm (value_ms > 0) or disarm (0).  Returns the previous remaining ms and
 * reload interval through the optional out pointers. */
void process_alarm_set(process_t *p, uint64_t value_ms, uint64_t interval_ms,
                       uint64_t *old_value_ms, uint64_t *old_interval_ms) {
    extern uint64_t ktime_now_ms(void);
    if (!p) return;
    uint64_t now = ktime_now_ms();
    uint64_t dl = p->alarm_deadline_ms;
    if (old_value_ms)
        *old_value_ms = dl ? (dl > now ? dl - now : 1) : 0;
    if (old_interval_ms)
        *old_interval_ms = p->alarm_interval_ms;
    p->alarm_interval_ms = value_ms ? interval_ms : 0;
    __atomic_store_n(&p->alarm_deadline_ms, value_ms ? now + value_ms : 0,
                     __ATOMIC_SEQ_CST);
    if (value_ms)
        __atomic_store_n(&alarm_ever_armed, 1, __ATOMIC_SEQ_CST);
}

void process_alarm_get(process_t *p, uint64_t *value_ms, uint64_t *interval_ms) {
    extern uint64_t ktime_now_ms(void);
    uint64_t now = ktime_now_ms();
    uint64_t dl = p ? p->alarm_deadline_ms : 0;
    if (value_ms) *value_ms = dl ? (dl > now ? dl - now : 1) : 0;
    if (interval_ms) *interval_ms = p ? p->alarm_interval_ms : 0;
}

/* Called from tty_timer_hook() on every LAPIC tick (IRQ context). */
void process_alarm_tick(void) {
    extern uint64_t ktime_now_ms(void);
    if (!__atomic_load_n(&alarm_ever_armed, __ATOMIC_ACQUIRE))
        return;
    if (smp_cpu_index_fast() != 0)
        return;                     /* one scanner: the BSP */
    uint64_t now = ktime_now_ms();
    for (int i = 1; i < MAX_PROCESSES; i++) {
        process_t *t = &processes[i];
        uint64_t dl = __atomic_load_n(&t->alarm_deadline_ms, __ATOMIC_ACQUIRE);
        if (!dl || now < dl)
            continue;
        if (!t->is_running || t->is_zombie) {
            t->alarm_deadline_ms = 0;
            continue;
        }
        t->alarm_deadline_ms = t->alarm_interval_ms ? now + t->alarm_interval_ms : 0;
        process_post_signal(t, SIGALRM);
    }
}

/* True when @p is executing on the calling context, i.e. this CPU's current
 * thread is the thread that runs @p.  get_current_process() alone is a
 * heuristic (boot-thread ash has owner_process == NULL) and must not decide
 * whether it is safe to finish a process synchronously. */
bool process_is_self(process_t *p) {
    if (!p) return false;
    thread_t *thr = process_exec_thread(p);
    if (thr)
        return thr == scheduler_get_current_thread();
    return p == get_current_process();
}

/* Asynchronous kill (Linux model): mark the target killed with a pending
 * SIGKILL, wake it out of an interruptible sleep and IPI its CPU.  Nothing
 * of the target is torn down here: it may be anywhere in the kernel on
 * another CPU.  It terminates itself at its next safe point -- syscall
 * return (linux_syscall_dispatch / syscall_handler), IRQ return to ring 3
 * (tty_timer_hook), or immediately after a -EINTR from a blocking call --
 * through its normal exit path (fork child -> zombie for wait4 + reaper,
 * spawned -> exec_spawned_exit, sync program -> back to Zeus). */
void process_request_kill(process_t *t, int signum) {
    if (!t || signum < 1 || signum >= NSIG) return;
    if (!t->is_running || t->is_zombie || t == &processes[0]) return;
    int zero = 0;
    __atomic_compare_exchange_n(&t->kill_sig, &zero, signum, false,
                                __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    __atomic_or_fetch(&t->pending_signals, 1u << SIGKILL, __ATOMIC_SEQ_CST);
    thread_t *thr = process_exec_thread(t);
    if (!thr)
        serial_printf("signal: kill PID %u (sig %d) has no thread yet; "
                      "it exits at its next syscall\n", t->pid, signum);
    process_kick_thread(thr);
}

/* Fatal signal pending for @p: used by blocking loops that used to ignore
 * signals entirely.  A remote kill always counts.  A default-fatal pending
 * signal counts only for Linux-ABI processes, whose syscall return path
 * delivers it; native (INT 0x80) programs only act on kill requests there,
 * so returning -EINTR for anything else would just make them spin. */
bool proc_kill_pending(process_t *p) {
    if (!p) return false;
    if (__atomic_load_n(&p->kill_sig, __ATOMIC_ACQUIRE))
        return true;
    return p->linux_abi && proc_fatal_default_pending(p) != 0;
}

/* Deliver a pending signal that has a handler on the IRQ-return-to-user path,
 * rewriting the LIVE interrupt frame (never a stash). Called from the LAPIC
 * tick when the interrupted context is ring 3 and owned by p. */
int process_irq_deliver(process_t *p, registers_t *r) {
    if (!p || !r || (r->cs & 3) != 3) return 0;
    if (!p->is_running || p->is_zombie || !p->linux_abi || p->sig_in_handler)
        return 0;
    uint32_t pend = p->pending_signals & ~p->blocked_signals;
    if (!pend) return 0;
    static const int ok[] = { SIGINT, SIGQUIT, SIGTERM, SIGHUP, SIGUSR1, SIGUSR2, SIGALRM };
    for (unsigned i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) {
        int s = ok[i];
        uint32_t bit = 1u << s;
        if (!(pend & bit)) continue;
        sighandler_t h = p->signal_handlers[s];
        if (h == SIG_DFL || h == SIG_IGN || h == SIG_ERR) continue;
        uint64_t ha = (uint64_t)(uintptr_t)h;
        uint64_t restorer = p->sig_restorer[s];
        if (ha < 0x10000ULL || (ha >> 47) || !restorer || (restorer >> 47)) continue;
        if (p->signal_sa_flags[s] & 0x4) continue; /* SA_SIGINFO: syscall path only */
        if (r->rip >> 47) return 0;
        /* Skip the 128-byte red zone; handler entry sees rsp == 8 mod 16. */
        uint64_t rsp = ((r->rsp - 128) & ~0xFULL) - 8;
        set_current_process(p);
        if (copy_to_user((void *)(uintptr_t)rsp, &restorer, sizeof(restorer)) != 0)
            return 0; /* stack page not writable yet: try again next syscall/tick */
        __atomic_and_fetch(&p->pending_signals, ~bit, __ATOMIC_SEQ_CST);
        p->sig_saved_r15 = r->r15; p->sig_saved_r14 = r->r14;
        p->sig_saved_r13 = r->r13; p->sig_saved_r12 = r->r12;
        p->sig_saved_rbp = r->rbp; p->sig_saved_rbx = r->rbx;
        p->sig_saved_r9  = r->r9;  p->sig_saved_r8  = r->r8;
        p->sig_saved_r10 = r->r10; p->sig_saved_rdx = r->rdx;
        p->sig_saved_rsi = r->rsi; p->sig_saved_rdi = r->rdi;
        p->sig_saved_rax = r->rax; p->sig_saved_rcx = r->rcx;
        p->sig_saved_r11 = r->r11;
        p->sig_saved_rip = r->rip; p->sig_saved_rsp = r->rsp;
        p->sig_saved_rflags = r->rflags;
        p->sig_from_irq = true;
        p->sig_in_handler = true;
        r->rip = ha;
        r->rsp = rsp;
        r->rdi = (uint64_t)s;
        r->rsi = 0;
        r->rdx = 0;
        r->rax = 0;
        r->rflags = 0x202;
        return s;
    }
    return 0;
}

void process_kill_pgid(uint32_t pgid, int signum) {
    if (!pgid) return;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t *t = &processes[i];
        if (!t->is_running || t->pgid != pgid) continue;
        process_post_signal(t, signum);
    }
}

int sys_getsid(pid_t pid) {
    process_t *cur = get_current_process();
    if (pid == 0) return cur ? (int)cur->sid : -ESRCH;
    for (int i = 0; i < MAX_PROCESSES; i++)
        if (processes[i].pid == (uint32_t)pid &&
            (processes[i].is_running || processes[i].is_zombie))
            return (int)processes[i].sid;
    return -ESRCH;
}

/* ---- tty job-control signals (Ctrl+C / Ctrl+\) ----
 * Keyboard ISR only posts; a timer tick dispatches to the fg pgrp. */
static volatile int tty_pending_sig = 0;
void tty_post_signal(int sig) {
    __atomic_store_n(&tty_pending_sig, sig, __ATOMIC_SEQ_CST);
}
void tty_dispatch_pending(void) {
    if (!__atomic_load_n(&tty_pending_sig, __ATOMIC_RELAXED)) return;
    int sig = __atomic_exchange_n(&tty_pending_sig, 0, __ATOMIC_SEQ_CST);
    if (!sig) return;
    uint32_t fg = process_get_fg_pgid();
    serial_printf("tty: signal %d -> fg pgid %u\n", sig, fg);
    process_kill_pgid(fg, sig);
}
bool tty_linux_foreground(void) {
    process_t *sp = exec_get_sync_proc();   /* exec_fg_sync_proc only */
    return sp && sp->is_running && sp->linux_abi;
}

/* Blocking console char read that gives up on an interrupting signal. */
int sys_getchar_intr(process_t *p) {
    int c = -1;
    while (c < 0) {
        c = keyboard_consume_char();
        if (c < 0) {
            if (proc_signal_interrupts(p))
                return -EINTR;
            if (global_scheduler.scheduler_running)
                thread_sleep(1);
            else
                __asm__ volatile("hlt");
        }
    }
    keyboard_clear_execute();
    return c;
}


/* Shell helper: list every live entry in the process table (zeus `ps`). */
void process_list_all(void) {
    printf("  PID  STATE    MODE    UID  CWD\n");
    int count = 0;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t *p = &processes[i];
        if (!p->is_running && !p->is_zombie) continue;
        printf("%5d  %-7s  %-6s %4d  %s\n",
               (int)p->pid,
               p->is_zombie ? "zombie" : "running",
               p->is_usermode ? "user" : "kernel",
               (int)p->uid,
               p->cwd[0] ? p->cwd : "/");
        count++;
    }
    printf("%d process%s\n", count, count == 1 ? "" : "es");
}

/* Shell helper: force-terminate a process by PID (zeus `kill`).
 * Returns 0 on success, -1 if no such live process. */
int process_terminate(uint32_t pid, int exit_code) {
    (void)exit_code;
    for (int i = 1; i < MAX_PROCESSES; i++) {
        if (processes[i].pid == pid && processes[i].is_running) {
            process_signal_terminate(&processes[i], SIGKILL);
            return 0;
        }
    }
    return -1;
}

extern int snprintf(char *str, size_t size, const char *fmt, ...);

/* Resolve a user exec path the same way Zeus does: absolute as-is,
 * otherwise cwd, /bin, /, /apps, /os/apps, each with an optional .exe suffix. */
/* Lexically normalise an absolute path in place: collapse "//", drop "."
 * components and resolve ".." (VFS mounts expect clean absolute paths, so
 * "/tmp/./hello" would otherwise not be found). */
static void exec_norm_path(char *path) {
    char out[256];
    size_t o = 0;
    const char *s = path;
    while (*s) {
        while (*s == '/') s++;
        if (!*s) break;
        const char *e = s;
        while (*e && *e != '/') e++;
        size_t n = (size_t)(e - s);
        if (n == 1 && s[0] == '.') {
            /* skip */
        } else if (n == 2 && s[0] == '.' && s[1] == '.') {
            while (o > 0 && out[o - 1] != '/') o--;
            if (o > 0) o--;
        } else {
            if (o + 1 + n >= sizeof(out)) break;
            out[o++] = '/';
            memcpy(out + o, s, n);
            o += n;
        }
        s = e;
    }
    if (o == 0) out[o++] = '/';
    out[o] = 0;
    memcpy(path, out, o + 1);
}

static int exec_resolve_path(process_t *proc, const char *name, char *out, size_t outsz) {
    if (!name || !out || outsz < 4) return -EINVAL;
    /* A name containing '/' is never searched for (POSIX): resolve it against
     * the cwd and normalise, so "./hello" and "sub/../x" run from the cwd. */
    if (proc && name[0] != '/' && strchr(name, '/') && vfs_has_root_mount()) {
        char cand[256];
        const char *cwd = proc->cwd[0] ? proc->cwd : "/";
        snprintf(cand, sizeof(cand), "%s/%s", cwd, name);
        exec_norm_path(cand);
        if (!vfs_exists(cand)) {
            char ex[256];
            snprintf(ex, sizeof(ex), "%s.exe", cand);
            if (vfs_exists(ex)) kstrncpy(cand, ex, sizeof(cand));
        }
        kstrncpy(out, cand, outsz);
        return 0;
    }
    /* busybox ash re-execs applets via /proc/self/exe */
    if (proc && (kstrcmp(name, "/proc/self/exe") == 0 ||
                 kstrcmp(name, "/proc/self/exe/") == 0)) {
        const char *exe = proc->linux_exe_path[0] ? proc->linux_exe_path
                                                  : "/bin/busybox.exe";
        kstrncpy(out, exe, outsz);
        return 0;
    }
    if (!vfs_has_root_mount()) {
        kstrncpy(out, name, outsz);
        return 0;
    }
    if (vfs_exists(name)) {
        kstrncpy(out, name, outsz);
        return 0;
    }

    char cand[256];
    if (name[0] == '/' && (strstr(name, "/./") || strstr(name, "/../") || strstr(name, "//"))) {
        kstrncpy(cand, name, sizeof(cand));
        exec_norm_path(cand);
        if (vfs_exists(cand)) { kstrncpy(out, cand, outsz); return 0; }
        name = out;
        kstrncpy(out, cand, outsz);
    }
    if (name[0] == '/') {
        snprintf(cand, sizeof(cand), "%s.exe", name);
        if (vfs_exists(cand)) {
            kstrncpy(out, cand, outsz);
            return 0;
        }
        kstrncpy(out, name, outsz);
        return 0; /* let exec_file report the failure */
    }

    const char *cwd = (proc && proc->cwd[0]) ? proc->cwd : "/";
    const char *dirs[] = { cwd, "/bin", "/", "/apps", "/os/apps", NULL };
    for (int d = 0; dirs[d]; d++) {
        const char *dir = dirs[d];
        int need_slash = !(dir[0] == '/' && dir[1] == '\0') &&
                         dir[strlen(dir) - 1] != '/';
        snprintf(cand, sizeof(cand), need_slash ? "%s/%s" : "%s%s", dir, name);
        if (vfs_exists(cand)) {
            kstrncpy(out, cand, outsz);
            return 0;
        }
        snprintf(cand, sizeof(cand), need_slash ? "%s/%s.exe" : "%s%s.exe",
                 dir, name);
        if (vfs_exists(cand)) {
            kstrncpy(out, cand, outsz);
            return 0;
        }
    }
    kstrncpy(out, name, outsz);
    return 0;
}

int sys_exec(const char *pathname, char *const argv[]) {
    return sys_exec_env(pathname, argv, NULL);
}

/* execve with an environment: envp (user pointer, may be NULL) becomes the
 * new image's environment instead of the built-in defaults. */
#define EXEC_ARGV_MAX 1024
#define EXEC_ENVP_MAX 1024
#define EXEC_ARG_BLK  (128 * 1024)   /* pointer tables + strings (Linux ARG_MAX-ish) */

/* Copy one NUL-terminated user string into [dst, end); returns bytes used
 * (incl. NUL) or -E2BIG / -EFAULT. */
static long exec_copy_ustr(char *dst, char *end, const char *uptr) {
    long room = (long)(end - dst);
    for (long i = 0; i < room; i++) {
        char c;
        if (copy_from_user(&c, uptr + i, 1) != 0) return -EFAULT;
        dst[i] = c;
        if (!c) return i + 1;
    }
    return -E2BIG;
}

/* #! detection.  Returns 1 (interp only), 2 (interp + one arg), 0 (not a
 * script / Lua script handled by the kernel runtime), <0 on error. */
static int exec_shebang(const char *path, char *interp, size_t isz, char *arg, size_t asz) {
    char head[256];
    int fd = vfs_open(path, VFS_O_READ);
    if (fd < 0) return 0;
    int n = vfs_read(fd, head, sizeof(head) - 1);
    vfs_close(fd);
    if (n < 3 || head[0] != '#' || head[1] != '!') return 0;
    head[n] = 0;
    char *s = head + 2;
    char *nl = s;
    while (*nl && *nl != '\n') nl++;
    *nl = 0;
    while (*s == ' ' || *s == '\t') s++;
    char *e = s;
    while (*e && *e != ' ' && *e != '\t') e++;
    if (e == s) return -ENOEXEC;
    char save = *e; *e = 0;
    kstrncpy(interp, s, isz);
    if (strstr(interp, "lua")) return 0;    /* kernel Lua runtime keeps these */
    *e = save;
    s = e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) e--;
    *e = 0;
    if (!*s) { arg[0] = 0; return 1; }
    kstrncpy(arg, s, asz);
    return 2;
}
/* Linux programs exec /usr/bin/env, /bin/sed, ... which are BusyBox applets
 * here (no symlinks on FAT32/ISO).  If such a path does not exist, run
 * /bin/busybox.exe with argv[0] = applet name. Returns the applet name or NULL. */
static const char *exec_busybox_applet(process_t *p, const char *resolved) {
    static const char *const applets[] = {
        "env", "sh", "ash", "sed", "awk", "grep", "egrep", "fgrep", "cat", "ls", "cp", "mv",
        "rm", "rmdir", "mkdir", "test", "[", "expr", "tr", "sort", "uniq", "head", "tail",
        "wc", "find", "xargs", "basename", "dirname", "printf", "echo", "true", "false",
        "touch", "chmod", "ln", "uname", "date", "sleep", "cut", "tee", "diff", "cmp",
        "install", "readlink", "realpath", "mktemp", "od", "gzip", "gunzip", "tar", "patch",
        "md5sum", "sha256sum", "stat", "id", "whoami", "pwd", "seq", "yes", "which", "kill",
        "ps", "du", "df", "hexdump", "xxd", "strings", "nproc", "getconf", "sync", "less", "more",
        "vi", "unzip", "bzip2", "xz", NULL };
    if (!p || !p->linux_abi || !resolved) return NULL;
    const char *bn;
    if (!kstrncmp(resolved, "/bin/", 5)) bn = resolved + 5;
    else if (!kstrncmp(resolved, "/usr/bin/", 9)) bn = resolved + 9;
    else if (!kstrncmp(resolved, "/sbin/", 6)) bn = resolved + 6;
    else if (!kstrncmp(resolved, "/usr/sbin/", 10)) bn = resolved + 10;
    else return NULL;
    for (const char *q = bn; *q; q++) if (*q == '/') return NULL;
    if (vfs_exists(resolved) || !vfs_exists("/bin/busybox.exe")) return NULL;
    for (int i = 0; applets[i]; i++)
        if (kstrcmp(bn, applets[i]) == 0) return applets[i];
    return NULL;
}

int sys_exec_env(const char *pathname, char *const argv[], char *const envp[]) {
    process_t *p = get_current_process();

    /* Copy the path out of user memory before dereferencing it: pathname is
     * an unvalidated user pointer (kernel would otherwise fault or read
     * arbitrary kernel memory on a hostile pointer). */
    if (!pathname || !is_user_address((uint64_t)pathname))
        return -EFAULT;
    char kpath[256];
    syscall_uaccess_ensure_user_cr3();
    if (!strncpy_from_user(kpath, pathname, sizeof(kpath)))
        return -EFAULT;
    if (kpath[0] == '\0')
        return -ENOENT;

    /* Copy argv and envp out of user memory into one kernel block BEFORE
     * anything else: a CLONE_VM/vfork child's argv/envp live in the parent's
     * (shared) address space, and the old fixed 16x128 / 15x256 tables
     * silently truncated long recipes and make's environment. */
    char *argblk = (char *)kalloc(EXEC_ARG_BLK);
    if (!argblk) return -ENOMEM;
    char **kargv = (char **)argblk;                          /* [EXEC_ARGV_MAX+1+3] */
    const char **kenvp = (const char **)(argblk + (EXEC_ARGV_MAX + 4) * sizeof(char *));
    char *strs = argblk + (EXEC_ARGV_MAX + 4 + EXEC_ENVP_MAX + 1) * sizeof(char *);
    char *strs_end = argblk + EXEC_ARG_BLK;
    int argc = 0, envc = 0;
    long cerr = 0;
    if (argv && is_user_address((uint64_t)argv)) {
        for (;; argc++) {
            char *uptr = NULL;
            if (copy_from_user(&uptr, &argv[argc], sizeof(uptr)) != 0) { cerr = -EFAULT; break; }
            if (!uptr) break;
            if (argc >= EXEC_ARGV_MAX) { cerr = -E2BIG; break; }
            if (!is_user_address((uint64_t)uptr)) { cerr = -EFAULT; break; }
            long n = exec_copy_ustr(strs, strs_end, uptr);
            if (n < 0) { cerr = n; break; }
            kargv[argc] = strs;
            strs += n;
        }
    }
    kargv[argc] = NULL;
    bool have_env = envp && is_user_address((uint64_t)envp);
    if (!cerr && have_env) {
        for (;; envc++) {
            char *uptr = NULL;
            if (copy_from_user(&uptr, &envp[envc], sizeof(uptr)) != 0) { cerr = -EFAULT; break; }
            if (!uptr) break;
            if (envc >= EXEC_ENVP_MAX) { cerr = -E2BIG; break; }
            if (!is_user_address((uint64_t)uptr)) { cerr = -EFAULT; break; }
            long n = exec_copy_ustr(strs, strs_end, uptr);
            if (n < 0) { cerr = n; break; }
            kenvp[envc] = strs;
            strs += n;
        }
    }
    kenvp[envc] = NULL;
    if (cerr) { kfree(argblk); return (int)cerr; }
    if (!have_env) kenvp = NULL;
    char resolved[256];
    exec_resolve_path(p, kpath, resolved, sizeof(resolved));

    /*
     * In-place image replacement for an already-usermode process (concurrent
     * fork children, ash, busybox applets). Keeps pid/fds/kstack; SYSRET into
     * the new entry via proc->exec_replaced (see LX_execve).
     */
    if (p->is_usermode && p->pml4) {
        extern int exec_replace_env(process_t *proc, const char *filename, char *const argv[],
                                    const char *const envp[]);
        /* #! scripts (Linux binfmt_script): argv becomes
         * interp [optarg] <path as given> argv[1..].  Up to 4 levels. */
        char sb_interp[128], sb_arg[128], sb_path[256];
        for (int depth = 0; depth < 4; depth++) {
            int sr = exec_shebang(resolved, sb_interp, sizeof(sb_interp), sb_arg, sizeof(sb_arg));
            if (sr <= 0) break;
            if (argc + 3 >= EXEC_ARGV_MAX + 4) { kfree(argblk); return -E2BIG; }
            kstrncpy(sb_path, depth == 0 ? kpath : resolved, sizeof(sb_path));
            int shift = sr == 2 ? 2 : 1;            /* interp [+ arg], path replaces argv[0] */
            int old_argc = argc ? argc : 1;
            if (!argc) { kargv[0] = sb_path; kargv[1] = NULL; }
            for (int i = old_argc; i >= 1; i--)     /* incl. NULL terminator */
                kargv[i + shift] = kargv[i];
            /* strings for interp/arg/path must outlive this loop: carve from strs */
            long n1 = (long)strlen(sb_interp) + 1, n2 = (long)strlen(sb_arg) + 1, n3 = (long)strlen(sb_path) + 1;
            if (strs + n1 + n2 + n3 > strs_end) { kfree(argblk); return -E2BIG; }
            memcpy(strs, sb_interp, n1); kargv[0] = strs; strs += n1;
            if (shift == 2) { memcpy(strs, sb_arg, n2); kargv[1] = strs; strs += n2; }
            memcpy(strs, sb_path, n3); kargv[shift] = strs; strs += n3;
            argc = old_argc + shift;
            exec_resolve_path(p, sb_interp, resolved, sizeof(resolved));
            /* "#!/usr/bin/env prog" and "#!/usr/bin/foo": fall back to
             * /bin/<basename> when the interpreter path does not exist. */
            if (!vfs_exists(resolved)) {
                const char *bn = sb_interp;
                for (const char *q = sb_interp; *q; q++)
                    if (*q == '/') bn = q + 1;
                char alt[160];
                snprintf(alt, sizeof(alt), "/bin/%s", bn);
                exec_resolve_path(p, alt, resolved, sizeof(resolved));
            }
        }
        {
            const char *ap = exec_busybox_applet(p, resolved);
            if (ap) {
                kstrncpy(resolved, "/bin/busybox.exe", sizeof(resolved));
                if (argc == 0) { kargv[1] = NULL; argc = 1; }
                kargv[0] = (char *)ap;          /* busybox dispatches on argv[0] */
            }
        }
        /* /bin/sh resolves to the native inDevOS shell (/bin/sh.exe), which
         * is not a POSIX sh: Linux programs (make's SHELL, system(), popen(),
         * posix_spawn, #!/bin/sh scripts) get BusyBox ash instead. */
        if (p->linux_abi &&
            (kstrcmp(resolved, "/bin/sh.exe") == 0 || kstrcmp(resolved, "/bin/sh") == 0 ||
             kstrcmp(resolved, "/bin/ash") == 0 || kstrcmp(resolved, "/bin/ash.exe") == 0) &&
            vfs_exists("/bin/busybox.exe")) {
            kstrncpy(resolved, "/bin/busybox.exe", sizeof(resolved));
            if (argc == 0) { kargv[0] = "sh"; kargv[1] = NULL; argc = 1; }
        }
        int ir = exec_replace_env(p, resolved, argc ? kargv : NULL, kenvp);
        kfree(argblk);
        if (ir < 0)
            return ir;   /* failed execve leaves fds untouched (posix_spawn error pipe) */
        /* Success: close CLOEXEC fds now (Linux closes them only on success). */
        for (int i = 0; i < MAX_OPEN_FILES_SYSCALL; i++) {
            if (p->fds[i].in_use && p->fds[i].cloexec)
                fd_close_entry(p, i);
        }
        /* Success: do not nest. Caller rewrites the SYSCALL frame. */
        return 0;
    }

    /* Nested (kernel/shell first launch) path: legacy ordering. */
    for (int i = 0; i < MAX_OPEN_FILES_SYSCALL; i++) {
        if (p->fds[i].in_use && p->fds[i].cloexec)
            fd_close_entry(p, i);
    }
    /* Kernel/shell first launch: nested blocking exec_file (unchanged). */
    bool nested_exec = get_program_running();
    if (nested_exec)
        exec_push_return_ctx();
    int ret = exec_file(resolved, argc ? kargv : NULL);
    kfree(argblk);
    if (nested_exec)
        exec_pop_return_ctx();

    if (p->fork_parent) {
        exec_did_swapgs = false;
        p->exit_status = ret;
        p->is_running  = false;
        p->is_zombie   = true;
        process_t *parent = p->fork_parent;
        int child_pid = (int)p->pid;
        p->fork_parent = NULL;
        current_process = parent;
        exec_set_ring3_active(parent);
        exec_longjmp((exec_jmpbuf_t *)parent->fork_jmpbuf, child_pid);
        __builtin_unreachable();
    }

    return ret;
}

int sys_setuid(uint32_t uid) {
    process_t *proc = get_current_process();
    if (proc->uid == 0) {
        proc->uid = uid;
        return 0;
    }
    if (uid == proc->uid) return 0;
    return -EPERM;
}

int sys_setgid(uint32_t gid) {
    process_t *proc = get_current_process();
    if (proc->uid == 0) {
        proc->gid = gid;
        return 0;
    }
    if (gid == proc->gid) return 0;
    return -EPERM;
}

int sys_getuid(void) {
    return (int)get_current_process()->uid;
}

int sys_getgid(void) {
    return (int)get_current_process()->gid;
}

int sys_sleep(unsigned int seconds) {
    extern uint64_t ktime_deadline_ms(uint64_t ms_from_now);
    extern bool ktime_expired(uint64_t deadline_ms);

    uint64_t deadline = ktime_deadline_ms((uint64_t)seconds * 1000);
    while (!ktime_expired(deadline)) {
        if (proc_kill_pending(get_current_process()))
            return -EINTR;
        if (global_scheduler.scheduler_running)
            thread_sleep(1);
        else
            __asm__ volatile("hlt");
    }
    return 0;
}

/* Socket syscalls live in syscall_net.c */

/* Wall-clock time from the CMOS RTC.  gettimeofday() deliberately stays on the
 * monotonic HPET timebase (uptime), so programs that want the calendar date
 * use this instead - it is what lets `date` exist as a real user program. */
int sys_rtc_time(indevos_rtc_time_t *user) {
    if (!user || !access_ok_user_range((uint64_t)user, sizeof(*user)))
        return -EFAULT;

    rtc_time_t now;
    rtc_read(&now);

    indevos_rtc_time_t k;
    k.year    = now.year;
    k.month   = now.month;
    k.day     = now.day;
    k.hour    = now.hour;
    k.minute  = now.minute;
    k.second  = now.second;
    k.weekday = now.weekday;

    return copy_to_user(user, &k, sizeof(k)) ? -EFAULT : 0;
}

int sys_gettimeofday(timeval_ind_t *tv, void *tz) {
    (void)tz;
    if (!tv || !access_ok_user_range((uint64_t)tv, sizeof(*tv)))
        return -EFAULT;
    uint64_t ms = sys_time();
    timeval_ind_t k;
    k.tv_sec = (time_t_ind)(ms / 1000u);
    k.tv_usec = (suseconds_t_ind)((ms % 1000u) * 1000);
    return copy_to_user(tv, &k, sizeof(k)) ? -EFAULT : 0;
}

int sys_select(int nfds, fd_set_t *readfds, fd_set_t *writefds, fd_set_t *exceptfds,
               void *timeout_tv) {
    (void)exceptfds;
    syscall_uaccess_ensure_user_cr3();
    if (nfds < 0 || nfds > FD_SETSIZE_KERNEL)
        return -EINVAL;

    fd_set_t kr, kw;
    memset(&kr, 0, sizeof(kr));
    memset(&kw, 0, sizeof(kw));
    if (readfds && copy_from_user(&kr, readfds, sizeof(kr)) != 0)
        return -EFAULT;
    if (writefds && copy_from_user(&kw, writefds, sizeof(kw)) != 0)
        return -EFAULT;

    extern uint64_t ktime_deadline_ms(uint64_t);
    extern bool     ktime_expired(uint64_t);
    uint64_t deadline = ktime_deadline_ms(5000);
    if (timeout_tv) {
        timeval_ind_t tv;
        if (copy_from_user(&tv, timeout_tv, sizeof(tv)) == 0) {
            uint64_t ms = (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
            deadline = ktime_deadline_ms(ms);
        }
    }

    process_t *proc = get_current_process();
    fd_set_t outr, outw;
    memset(&outr, 0, sizeof(outr));
    memset(&outw, 0, sizeof(outw));

    while (!ktime_expired(deadline)) {
        int n = 0;
        for (int fd = 0; fd < nfds; fd++) {
            if (FD_ISSET(fd, &kr)) {
                if (fd == STDIN_FILENO && keyboard_get_buffer_length() > 0) {
                    FD_SET_BIT(fd, &outr);
                    n++;
                } else if (fd >= 0 && fd < MAX_OPEN_FILES_SYSCALL && proc->fds[fd].in_use &&
                           proc->fds[fd].fd_type == FD_TYPE_PIPE_READ) {
                    int a = pipe_available((pipe_t *)proc->fds[fd].file_handle);
                    if (a > 0) {
                        FD_SET_BIT(fd, &outr);
                        n++;
                    }
                }
            }
            if (FD_ISSET(fd, &kw)) {
                if (fd >= 1 && fd <= 2) {
                    FD_SET_BIT(fd, &outw);
                    n++;
                } else if (fd >= 0 && fd < MAX_OPEN_FILES_SYSCALL && proc->fds[fd].in_use &&
                           proc->fds[fd].fd_type == FD_TYPE_PIPE_WRITE) {
                    FD_SET_BIT(fd, &outw);
                    n++;
                }
            }
        }
        if (n > 0) {
            if (readfds && copy_to_user(readfds, &outr, sizeof(outr)) != 0)
                return -EFAULT;
            if (writefds && copy_to_user(writefds, &outw, sizeof(outw)) != 0)
                return -EFAULT;
            return n;
        }
        if (proc_kill_pending(get_current_process()))
            return -EINTR;
        if (global_scheduler.scheduler_running)
            thread_sleep(1);
        else
            __asm__ volatile("hlt");
    }
    return 0;
}

static int sys_poll_impl(pollfd_t *fds, unsigned int nfds, int timeout_ms, bool kbuf);
int sys_poll(pollfd_t *fds, unsigned int nfds, int timeout_ms) {
    return sys_poll_impl(fds, nfds, timeout_ms, false);
}
int sys_poll_kernel(pollfd_t *kfds_in, unsigned int nfds, int timeout_ms) {
    return sys_poll_impl(kfds_in, nfds, timeout_ms, true);
}
static int poll_copy_out(pollfd_t *dst, const pollfd_t *src, unsigned int nfds, bool kbuf) {
    if (kbuf) { memcpy(dst, src, nfds * sizeof(pollfd_t)); return 0; }
    return copy_to_user(dst, src, nfds * sizeof(pollfd_t));
}
static int sys_poll_impl(pollfd_t *fds, unsigned int nfds, int timeout_ms, bool kbuf) {
    syscall_uaccess_ensure_user_cr3();
    if (!fds || nfds == 0 || nfds > 64 ||
        (!kbuf && !access_ok_user_range((uint64_t)fds, nfds * sizeof(pollfd_t))))
        return -EFAULT;

    extern uint64_t ktime_deadline_ms(uint64_t);
    extern bool     ktime_expired(uint64_t);
    uint64_t deadline;
    if (timeout_ms >= 0)
        deadline = ktime_deadline_ms((uint64_t)timeout_ms);
    else
        deadline = ktime_deadline_ms(60000);

    pollfd_t kfds[64];
    if (kbuf)
        memcpy(kfds, fds, nfds * sizeof(pollfd_t));
    else if (copy_from_user(kfds, fds, nfds * sizeof(pollfd_t)) != 0)
        return -EFAULT;

    process_t *proc = get_current_process();
    /* Always evaluate readiness at least once. timeout==0 must not skip the
     * check (ktime_deadline_ms(0)==now makes while(!expired) never enter). */
    for (;;) {
        poll_network_once();  /* advance TCP handshakes before checking readiness */
        int cnt = 0;
        for (unsigned i = 0; i < nfds; i++) {
            kfds[i].revents = 0;
            int fd = kfds[i].fd;
            if (fd < 0)
                continue;
            if ((kfds[i].events & POLLIN) && fd == STDIN_FILENO &&
                keyboard_get_buffer_length() > 0) {
                kfds[i].revents |= POLLIN;
            } else if (fd >= 0 && fd < MAX_OPEN_FILES_SYSCALL &&
                       proc->fds[fd].in_use && proc->fds[fd].fd_type == FD_TYPE_PIPE_READ) {
                pipe_t *rp = (pipe_t *)proc->fds[fd].file_handle;
                if ((kfds[i].events & POLLIN) && rp && pipe_available(rp) > 0)
                    kfds[i].revents |= POLLIN;
                /* Linux: all writers gone -> POLLHUP (reported even if not requested) */
                if (rp && rp->writers == 0)
                    kfds[i].revents |= POLLHUP;
            } else if (fd >= 0 && fd < MAX_OPEN_FILES_SYSCALL &&
                       proc->fds[fd].in_use && proc->fds[fd].fd_type == FD_TYPE_SOCKET) {
                kernel_socket_t *ks = (kernel_socket_t *)proc->fds[fd].file_handle;
                if (ks && ks->domain == 1 /* AF_UNIX */) {
                    extern int ux_poll(kernel_socket_t *s, int events);
                    kfds[i].revents |= (short)ux_poll(ks, kfds[i].events | 0x2000);
                    if (!(kfds[i].events & 0x2000)) kfds[i].revents &= ~0x2000;
                } else if (ks && ks->type == SOCK_DGRAM) {
                    udp_pcb_t *pcb = (udp_pcb_t *)ks->transport;
                    if ((kfds[i].events & POLLIN) && pcb && udp_pcb_rx_ready(pcb) > 0)
                        kfds[i].revents |= POLLIN;
                    if (kfds[i].events & (POLLOUT | 0x0100)) {
                        kfds[i].revents |= POLLOUT;
                        if (kfds[i].events & 0x0100) kfds[i].revents |= 0x0100;
                    }
                } else {
                    tcp_socket_t *ts = ks ? (tcp_socket_t *)ks->transport : NULL;
                    tcp_connection_t *conn = ts ? ts->connection : NULL;
                    extern bool tcp_conn_at_eof(tcp_connection_t *c);
                    if ((kfds[i].events & POLLIN) && ks && ks->listening && ts) {
                        if (tcp_socket_accept_ready(ts)) kfds[i].revents |= POLLIN;
                    } else if ((kfds[i].events & POLLIN) && conn && conn->recv_buffer_used > 0) {
                        kfds[i].revents |= POLLIN;
                    } else if (conn && ks->connected && tcp_conn_at_eof(conn)) {
                        /* peer closed: readable (read returns 0); hung up unless
                         * only the peer's half is closed (we may still write) */
                        kfds[i].revents |= POLLIN;
                        if (conn->state != TCP_STATE_CLOSE_WAIT) kfds[i].revents |= POLLHUP;
                        else kfds[i].revents |= kfds[i].events & 0x2000 /* POLLRDHUP */;
                    }
                    if (kfds[i].events & (POLLOUT | 0x0100 /* POLLWRNORM */)) {
                        if (conn && (conn->state == TCP_STATE_ESTABLISHED ||
                                     conn->state == TCP_STATE_CLOSE_WAIT) && !ks->listening) {
                            kfds[i].revents |= POLLOUT;
                            if (kfds[i].events & 0x0100) kfds[i].revents |= 0x0100;
                            ks->connected = true;
                        } else if (ts && (!conn || conn->state == TCP_STATE_CLOSED)) {
                            /* connect finished with failure (RST/refuse): wake like Linux */
                            kfds[i].revents |= POLLOUT | POLLERR | POLLHUP;
                        }
                    }
                }
            }
            if (fd >= 0 && fd < MAX_OPEN_FILES_SYSCALL && proc->fds[fd].in_use &&
                proc->fds[fd].fd_type == 8 /* FD_TYPE_EVENTFD */) {
                uint64_t v = *(volatile uint64_t *)proc->fds[fd].file_handle;
                if ((kfds[i].events & POLLIN) && v) kfds[i].revents |= POLLIN;
                if (kfds[i].events & POLLOUT) kfds[i].revents |= POLLOUT;
            }
            if (fd >= 0 && fd < MAX_OPEN_FILES_SYSCALL && proc->fds[fd].in_use &&
                proc->fds[fd].fd_type == 16 /* FD_TYPE_INOTIFY */) {
                extern bool inotify_ready(void *h);
                if ((kfds[i].events & (POLLIN | 0x0040)) && inotify_ready(proc->fds[fd].file_handle))
                    kfds[i].revents |= kfds[i].events & (POLLIN | 0x0040);
            }
            if (fd >= 0 && fd < MAX_OPEN_FILES_SYSCALL && proc->fds[fd].in_use &&
                proc->fds[fd].fd_type == 14 /* FD_TYPE_TIMERFD */) {
                extern bool lx_timerfd_ready(void *h);
                if ((kfds[i].events & POLLIN) && lx_timerfd_ready(proc->fds[fd].file_handle))
                    kfds[i].revents |= POLLIN;
            }
            if (fd >= 0 && fd < MAX_OPEN_FILES_SYSCALL && proc->fds[fd].in_use &&
                proc->fds[fd].fd_type == 15 /* FD_TYPE_SIGNALFD */) {
                extern bool lx_signalfd_ready(void *h, process_t *proc);
                if ((kfds[i].events & POLLIN) && lx_signalfd_ready(proc->fds[fd].file_handle, proc))
                    kfds[i].revents |= POLLIN;
            }
            if (fd >= 0 && fd < MAX_OPEN_FILES_SYSCALL && proc->fds[fd].in_use &&
                proc->fds[fd].fd_type == 10 /* FD_TYPE_DEV_INPUT */) {
                extern bool evdev_poll_ready(void *handle, bool *hup);
                bool hup = false;
                if (evdev_poll_ready(proc->fds[fd].file_handle, &hup)) {
                    if (kfds[i].events & POLLIN) kfds[i].revents |= POLLIN;
                    if (hup) kfds[i].revents |= POLLHUP | POLLERR;
                }
            }
            if (fd >= 0 && fd < MAX_OPEN_FILES_SYSCALL && proc->fds[fd].in_use &&
                proc->fds[fd].fd_type == 11 /* FD_TYPE_DEV_UINPUT: writable */)
                kfds[i].revents |= kfds[i].events & POLLOUT;
            if (fd >= 0 && fd < MAX_OPEN_FILES_SYSCALL && proc->fds[fd].in_use &&
                proc->fds[fd].fd_type == 9 /* FD_TYPE_DEV_FB: always ready */)
                kfds[i].revents |= kfds[i].events & (POLLIN | POLLOUT);
            if (fd >= 0 && fd < MAX_OPEN_FILES_SYSCALL && proc->fds[fd].in_use &&
                proc->fds[fd].fd_type == 12 /* FD_TYPE_DEV_WM */) {
                extern bool wm_dev_poll_ready(void *handle);
                if ((kfds[i].events & POLLIN) && wm_dev_poll_ready(proc->fds[fd].file_handle))
                    kfds[i].revents |= POLLIN;
                kfds[i].revents |= kfds[i].events & POLLOUT;
            }
            if (fd >= 0 && fd < MAX_OPEN_FILES_SYSCALL && proc->fds[fd].in_use &&
                proc->fds[fd].fd_type == FD_TYPE_PIPE_WRITE) {
                pipe_t *wp = (pipe_t *)proc->fds[fd].file_handle;
                if (wp && wp->readers == 0)
                    kfds[i].revents |= POLLERR;
                else if (!wp || wp->count < wp->buf_size)
                    kfds[i].revents |= kfds[i].events & POLLOUT;
            }
            if ((kfds[i].events & POLLOUT) && fd >= 1 && fd <= 2)
                kfds[i].revents |= POLLOUT;
            /* POSIX: return value is the number of fds with nonzero revents */
            if (kfds[i].revents)
                cnt++;
        }
        extern int lx_strace;
        if (lx_strace && (cnt > 0 || nfds <= 2)) {
            for (unsigned j = 0; j < nfds; j++)
                serial_printf("lxt:  poll fd %d type %d ev %x rev %x c=%d\n", kfds[j].fd,
                              (kfds[j].fd >= 0 && kfds[j].fd < MAX_OPEN_FILES_SYSCALL &&
                               proc->fds[kfds[j].fd].in_use) ? proc->fds[kfds[j].fd].fd_type : -1,
                              kfds[j].events, kfds[j].revents, cnt);
        }
        if (cnt > 0) {
            if (poll_copy_out(fds, kfds, nfds, kbuf) != 0)
                return -EFAULT;
            return cnt;
        }
        if (timeout_ms == 0)
            break;  /* one-shot: report no ready fds */
        if (timeout_ms > 0 && ktime_expired(deadline))
            break;
        if (proc_kill_pending(get_current_process()))
            return -EINTR;
        if (proc && proc->linux_abi && proc_signal_interrupts(proc))
            return -EINTR;            /* SIGALRM / handled signal: EINTR */
        fork_release_pending(proc);   /* about to sleep: let held kids run */
        {   /* sync /dev/wm client waiting: kmain is blocked, pump for it */
            extern bool exec_current_is_sync_program(void);
            extern bool wm_dev_pid_connected(uint32_t pid);
            extern void desktop_sync_pump(void);
            if (proc && exec_current_is_sync_program() && wm_dev_pid_connected(proc->pid))
                desktop_sync_pump();
        }
        if (global_scheduler.scheduler_running)
            thread_sleep(1);
        else
            __asm__ volatile("hlt");
        if (timeout_ms > 0 && ktime_expired(deadline))
            break;
    }
    if (poll_copy_out(fds, kfds, nfds, kbuf) != 0)
        return -EFAULT;
    return 0;
}

int sys_getppid(void) {
    process_t *p = get_current_process();
    if (p && p->parent)
        return (int)p->parent->pid;
    return 1;
}

mode_t sys_umask(mode_t mask) {
    process_t *p = get_current_process();
    mode_t old = p->file_umask;
    p->file_umask = mask & 0777;
    return old;
}

int sys_chmod(const char *pathname, mode_t mode) {
    char kpath[256];
    if (!pathname || !strncpy_from_user(kpath, pathname, sizeof(kpath)))
        return -EFAULT;
    process_t *proc = get_current_process();
    char abspath[256];
    if (resolve_abspath(proc, kpath, abspath, sizeof(abspath)) != 0)
        return -ENAMETOOLONG;
    if (perm_chmod(abspath, (uint16_t)(mode & 0777)) != 0)
        return -EACCES;
    return 0;
}

int sys_fchmod(int fd, mode_t mode) {
    process_t *proc = get_current_process();
    if (fd < 0 || fd >= MAX_OPEN_FILES_SYSCALL || !proc->fds[fd].in_use)
        return -EBADF;
    if (proc->fds[fd].fd_type != FD_TYPE_VFS || proc->fds[fd].vfs_fd < 0)
        return -ENODEV;
    char path[VFS_MAX_PATH];
    if (vfs_fd_get_path(proc->fds[fd].vfs_fd, path, sizeof(path)) != 0)
        return -EIO;
    if (perm_chmod(path, (uint16_t)(mode & 0777)) != 0)
        return -EACCES;
    return 0;
}

int sys_rename(const char *oldpath, const char *newpath) {
    char ko[256], kn[256];
    if (!oldpath || !newpath)
        return -EFAULT;
    if (!strncpy_from_user(ko, oldpath, sizeof(ko)) || !strncpy_from_user(kn, newpath, sizeof(kn)))
        return -EFAULT;
    process_t *proc = get_current_process();
    char ao[256], an[256];
    if (resolve_abspath(proc, ko, ao, sizeof(ao)) != 0 ||
        resolve_abspath(proc, kn, an, sizeof(an)) != 0)
        return -ENAMETOOLONG;
    return vfs_rename(ao, an);
}

int sys_link(const char *oldpath, const char *newpath) {
    (void)oldpath;
    (void)newpath;
    return -ENOSYS;
}

int sys_symlink(const char *target, const char *linkpath) {
    (void)target;
    (void)linkpath;
    return -ENOSYS;
}

int sys_readlink(const char *pathname, char *buf, size_t bufsiz) {
    (void)pathname;
    (void)buf;
    (void)bufsiz;
    return -ENOSYS;
}

int sys_sigaction(int signum, const sigaction_ind_t *act, sigaction_ind_t *oldact) {
    if (signum < 1 || signum >= NSIG)
        return -EINVAL;
    process_t *proc = get_current_process();
    if (oldact) {
        sigaction_ind_t o;
        memset(&o, 0, sizeof(o));
        o.sa_handler = (uint64_t)(uintptr_t)proc->signal_handlers[signum];
        o.sa_flags = proc->signal_sa_flags[signum];
        if (copy_to_user(oldact, &o, sizeof(o)) != 0)
            return -EFAULT;
    }
    if (act) {
        sigaction_ind_t a;
        if (copy_from_user(&a, act, sizeof(a)) != 0)
            return -EFAULT;
        if (signum == SIGKILL || signum == SIGSTOP)
            return -EINVAL;
        proc->signal_handlers[signum] = (sighandler_t)(uintptr_t)a.sa_handler;
        proc->signal_sa_flags[signum] = a.sa_flags;
    }
    return 0;
}

int sys_sigprocmask(int how, uint32_t *set, uint32_t *oldset) {
    process_t *proc = get_current_process();
    uint32_t old = proc->blocked_signals;
    if (oldset && copy_to_user(oldset, &old, sizeof(old)) != 0)
        return -EFAULT;
    if (set) {
        uint32_t s;
        if (copy_from_user(&s, set, sizeof(s)) != 0)
            return -EFAULT;
        if (how == SIG_BLOCK)
            proc->blocked_signals |= s;
        else if (how == SIG_UNBLOCK)
            proc->blocked_signals &= ~s;
        else if (how == SIG_SETMASK)
            proc->blocked_signals = s;
        else
            return -EINVAL;
    }
    return 0;
}

int sys_clock_gettime(int clockid, timespec_ind_t *tp) {
    if (!tp || !access_ok_user_range((uint64_t)tp, sizeof(*tp)))
        return -EFAULT;
    uint64_t ms = sys_time();
    if (clockid != CLOCK_REALTIME && clockid != CLOCK_MONOTONIC)
        return -EINVAL;
    timespec_ind_t ts;
    ts.tv_sec = (time_t_ind)(ms / 1000u);
    ts.tv_nsec = (long)((ms % 1000u) * 1000000ull);
    return copy_to_user(tp, &ts, sizeof(ts)) ? -EFAULT : 0;
}

int sys_nanosleep(const timespec_ind_t *req, timespec_ind_t *rem) {
    (void)rem;
    if (!req || !access_ok_user_range((uint64_t)req, sizeof(*req)))
        return -EFAULT;
    timespec_ind_t r;
    if (copy_from_user(&r, req, sizeof(r)) != 0)
        return -EFAULT;
    uint64_t ms = (uint64_t)r.tv_sec * 1000ull + (uint64_t)r.tv_nsec / 1000000ull;
    extern uint64_t ktime_deadline_ms(uint64_t);
    extern void     ktime_yield_until(uint64_t);
    /* Same path as SDL_Delay(): HLT until the HPET deadline. Safe from a
     * Ring-3 syscall (does not thread_sleep / reschedule this frame). */
    {
        extern bool ktime_expired(uint64_t);
        process_t *sp_ = get_current_process();
        uint64_t dl = ktime_deadline_ms(ms);
        (void)ktime_yield_until;
        /* Sleeping parent: held fork newborns must get to run meanwhile
         * (they were only released by a blocking wait). */
        fork_release_pending(sp_);
        while (!ktime_expired(dl)) {
            if (proc_signal_interrupts(sp_))
                return -EINTR;
            __asm__ volatile("sti; hlt" ::: "memory");
        }
    }
    return 0;
}

uint64_t sys_time(void) {
    /* hpet_get_ticks() already returns milliseconds from the HPET main
     * counter. Do not multiply by hpet_get_current_interval(); that value is
     * only the optional HPET periodic interrupt interval and may be zero when
     * the LAPIC timer is driving scheduling. */
    return hpet_get_ticks();
}

// Function to set current program mapping (called by ELF loader)
void syscall_set_program_mapping(void *real_base, uint64_t virtual_base, uint64_t size) {
    current_program_base = real_base;
    current_program_vbase = virtual_base;
    current_program_size = size;
}

// Simple user-space memory copy function
static int copy_string_from_user(const char *user_ptr, char *kernel_buffer, size_t max_len) {
    if (!user_ptr || !kernel_buffer || max_len == 0) return -1;
    return strncpy_from_user(kernel_buffer, user_ptr, max_len) ? 0 : -1;
}

int sys_print(const char *message) {
    if (!message) {
        return -EFAULT;
    }

    char kernel_buffer[256];
    if (copy_string_from_user(message, kernel_buffer, sizeof(kernel_buffer)) == 0) {
        printf("%s", kernel_buffer);
        return strlen(kernel_buffer);
    } else {
        return -EFAULT;
    }
}

int sys_getchar(void) {
    int c = -1;
    while (c < 0) {
        c = keyboard_consume_char();
        if (c < 0) {
            if (proc_kill_pending(get_current_process()))
                return -EINTR;
            if (global_scheduler.scheduler_running)
                thread_sleep(1);
            else
                __asm__ volatile("hlt");
        }
    }
    keyboard_clear_execute();
    return c;
}

int sys_mkdir(const char *pathname) {
    char kpath[256];
    if (!pathname || !strncpy_from_user(kpath, pathname, sizeof(kpath))) {
        return -EFAULT;
    }

    char abspath[256];
    if (resolve_abspath(get_current_process(), kpath, abspath, sizeof(abspath)) != 0)
        return -ENAMETOOLONG;

    if (!vfs_has_root_mount()) return -ENODEV;
    return vfs_mkdir(abspath);
}

int sys_rmdir(const char *pathname) {
    if (!pathname || !is_user_address((uint64_t)pathname))
        return -EFAULT;

    char kpath[256];
    if (!strncpy_from_user(kpath, pathname, sizeof(kpath)))
        return -EFAULT;

    process_t *proc = get_current_process();
    char abspath[256];
    if (resolve_abspath(proc, kpath, abspath, sizeof(abspath)) != 0)
        return -ENAMETOOLONG;

    if (!vfs_has_root_mount())
        return -ENODEV;

    int r = vfs_unlink(abspath);
    if (r == -ENOTEMPTY || r == -EROFS)
        return r;
    if (r != 0)
        return -ENOENT;
    return 0;
}

int sys_unlink(const char *pathname) {
    char kpath[256];
    if (!pathname || !strncpy_from_user(kpath, pathname, sizeof(kpath))) {
        return -EFAULT;
    }

    char abspath[256];
    if (resolve_abspath(get_current_process(), kpath, abspath, sizeof(abspath)) != 0)
        return -ENAMETOOLONG;

    if (!vfs_has_root_mount()) return -ENODEV;
    return vfs_unlink(abspath);
}

static uint64_t stat_ino_hash(const char *path) {
    uint64_t h = 14695981039346656037ULL;
    for (; *path; path++) {
        h ^= (unsigned char)*path;
        h *= 1099511628211ULL;
    }
    return h ? h : 1;
}

static void stat_fill_common(stat_t *st, uint32_t mode, uint64_t size,
                             const char *path, uint32_t uid, uint32_t gid) {
    memset(st, 0, sizeof(*st));
    st->st_dev = 9;
    st->st_ino = stat_ino_hash(path);
    st->st_mode = mode;
    st->st_nlink = 1;
    st->st_uid = uid;
    st->st_gid = gid;
    st->st_size = size;
    st->st_blksize = 4096;
    st->st_blocks = (size + 511) / 512;
    uint64_t sec = sys_time() / 1000;
    st->st_atime = st->st_mtime = st->st_ctime = sec;
}

static int stat_vfs_path(const char *abspath, stat_t *st) {
    uint32_t uid = 0, gid = 0;
    uint16_t pm = 0644;
    {
        vfs_stat_t vs;
        int vr = vfs_stat(abspath, &vs);
        if (vr == 0) {                      /* filesystem with real inodes (ext2/3/4) */
            stat_fill_common(st, vs.mode, vs.size, abspath, vs.uid, vs.gid);
            st->st_ino = vs.ino;
            st->st_nlink = vs.nlink ? vs.nlink : 1;
            st->st_blksize = vs.blksize ? vs.blksize : 4096;
            st->st_blocks = vs.blocks;
            st->st_atime = vs.atime;
            st->st_mtime = vs.mtime;
            st->st_ctime = vs.ctime;
            return 0;
        }
        if (vr != -ENOSYS)
            return vr == -5 ? -EIO : -ENOENT;
    }
    perm_get(abspath, &uid, &gid, &pm);

    int vfd = vfs_open(abspath, VFS_O_READ);
    if (vfd >= 0) {
        uint64_t sz = 0;
        int pos = vfs_seek(vfd, 0, VFS_SEEK_END);
        if (pos >= 0)
            sz = (uint64_t)pos;
        vfs_close(vfd);
        stat_fill_common(st, S_IFREG | (uint32_t)(pm & 0777u), sz, abspath, uid, gid);
        return 0;
    }

    vfs_dirent_t dent[2];
    if (vfs_readdir(abspath, dent, 2) >= 0) {
        stat_fill_common(st, S_IFDIR | (uint32_t)(pm & 0777u), 0, abspath, uid, gid);
        return 0;
    }
    return -ENOENT;
}

/* Fill an inDevOS stat_t for an absolute VFS path (Linux ABI layer). */
int syscall_stat_abspath(const char *abspath, stat_t *st) {
    if (!abspath || !st || !vfs_has_root_mount())
        return -ENOENT;
    return stat_vfs_path(abspath, st);
}

static int resolve_abspath(process_t *proc, const char *kpath, char *abspath, size_t absmax) {
    if (kpath[0] == '/') {
        size_t kl = strlen(kpath);
        if (kl >= absmax)
            return -ENAMETOOLONG;
        memmove(abspath, kpath, kl + 1);
        return 0;
    }
    size_t cwdl = strlen(proc->cwd);
    size_t kl = strlen(kpath);
    if (cwdl + 1 + kl >= absmax)
        return -ENAMETOOLONG;
    memmove(abspath, proc->cwd, cwdl);
    if (cwdl > 0 && proc->cwd[cwdl - 1] != '/') {
        abspath[cwdl] = '/';
        cwdl++;
    }
    memmove(abspath + cwdl, kpath, kl);
    abspath[cwdl + kl] = '\0';
    return 0;
}

static int kernel_path_is_directory(process_t *proc, const char *kpath, char *abspath, size_t absmax) {
    int r = resolve_abspath(proc, kpath, abspath, absmax);
    if (r != 0)
        return r;

    if (!vfs_has_root_mount())
        return -ENOENT;

    vfs_dirent_t dent[1];
    if (vfs_readdir(abspath, dent, 1) >= 0)
        return 0;
    int vfd = vfs_open(abspath, VFS_O_READ);
    if (vfd >= 0) {
        vfs_close(vfd);
        return -ENOTDIR;
    }
    return -ENOENT;
}

int sys_stat(const char *pathname, stat_t *statbuf) {
    if (!pathname || !statbuf || !access_ok_user_range((uint64_t)statbuf, sizeof(stat_t)))
        return -EFAULT;

    char kpath[256];
    if (!strncpy_from_user(kpath, pathname, sizeof(kpath)))
        return -EFAULT;

    process_t *proc = get_current_process();
    char abspath[256];
    if (resolve_abspath(proc, kpath, abspath, sizeof(abspath)) != 0)
        return -ENAMETOOLONG;

    stat_t st;
    if (!vfs_has_root_mount())
        return -ENOENT;

    int r = stat_vfs_path(abspath, &st);
    if (r != 0)
        return r;
    if (copy_to_user(statbuf, &st, sizeof(st)) != 0)
        return -EFAULT;
    return 0;
}

int sys_lstat(const char *pathname, stat_t *statbuf) {
    return sys_stat(pathname, statbuf);
}

int sys_fstat(int fd, stat_t *statbuf) {
    process_t *proc = get_current_process();
    if (!statbuf || !access_ok_user_range((uint64_t)statbuf, sizeof(stat_t)))
        return -EFAULT;
    if (fd < 0 || fd >= MAX_OPEN_FILES_SYSCALL || !proc->fds[fd].in_use)
        return -EBADF;

    stat_t st;
    if (proc->fds[fd].fd_type == FD_TYPE_VFS && proc->fds[fd].vfs_fd >= 0) {
        char path[VFS_MAX_PATH];
        if (vfs_fd_get_path(proc->fds[fd].vfs_fd, path, sizeof(path)) != 0)
            return -EIO;
        int r = stat_vfs_path(path, &st);
        if (r != 0)
            return r;
    } else {
        memset(&st, 0, sizeof(st));
        st.st_mode = S_IFCHR | 0666;
        st.st_nlink = 1;
        st.st_blksize = 1024;
    }

    if (copy_to_user(statbuf, &st, sizeof(st)) != 0)
        return -EFAULT;
    return 0;
}

char* sys_getcwd(char *buf, size_t size) {
    process_t *proc = get_current_process();
    
    if (!buf || size == 0 || !access_ok_user_range((uint64_t)buf, size)) {
        return NULL;
    }
    
    size_t cwd_len = strlen(proc->cwd);
    if (cwd_len >= size) {
        return NULL;  // Buffer too small
    }
    
    if (copy_to_user(buf, proc->cwd, cwd_len + 1) != 0) {
        return NULL;
    }
    return buf;
}

int sys_chdir(const char *path) {
    process_t *proc = get_current_process();
    
    if (!path) {
        return -EFAULT;
    }

    char kpath[256];
    if (!strncpy_from_user(kpath, path, sizeof(kpath))) {
        return -EFAULT;
    }

    char abspath[256];
    int chk = kernel_path_is_directory(proc, kpath, abspath, sizeof(abspath));
    if (chk == -ENAMETOOLONG)
        return -ENAMETOOLONG;
    if (chk != 0)
        return chk;

    if (strlen(abspath) >= sizeof(proc->cwd))
        return -ENAMETOOLONG;

    strcpy(proc->cwd, abspath);
    return 0;
}

int sys_mount(const char *device, const char *mountpoint, const char *fstype) {
    process_t *proc = get_current_process();
    if (proc->uid != 0) return -EPERM;  // only root can mount

    if (!device) return -EFAULT;

    char kdev[64], kmount[256], kfs[32];
    if (!strncpy_from_user(kdev, device, sizeof(kdev))) return -EFAULT;
    kmount[0] = '/'; kmount[1] = '\0';
    kfs[0] = '\0';
    if (mountpoint) strncpy_from_user(kmount, mountpoint, sizeof(kmount));
    if (fstype) strncpy_from_user(kfs, fstype, sizeof(kfs));
    if (kfs[0] == '\0') { kfs[0]='f'; kfs[1]='a'; kfs[2]='t'; kfs[3]='3'; kfs[4]='2'; kfs[5]='\0'; }

    int device_id = 0;
    for (const char *p = kdev; *p >= '0' && *p <= '9'; p++) {
        device_id = device_id * 10 + (*p - '0');
    }

    extern void mount_filesystem_ext(uint8_t, uint32_t, const char*, const char*);
    mount_filesystem_ext((uint8_t)device_id, 0, kfs, kmount);
    return 0;
}

int sys_unmount(const char *mountpoint) {
    process_t *proc = get_current_process();
    if (proc->uid != 0) return -EPERM;  // only root can unmount

    char kmount[256];
    kmount[0] = '/'; kmount[1] = '\0';
    if (mountpoint) strncpy_from_user(kmount, mountpoint, sizeof(kmount));

    extern void unmount_filesystem_path(const char *);
    unmount_filesystem_path(kmount);
    return 0;
}

int sys_readdir(int fd, void *dirp, size_t count) {
    process_t *proc = get_current_process();

    if (fd < 0 || fd >= MAX_OPEN_FILES_SYSCALL || !proc->fds[fd].in_use)
        return -EBADF;
    if (proc->fds[fd].fd_type != FD_TYPE_VFS_DIR)
        return -ENOTDIR;
    if (count == 0)
        return 0;
    if (!dirp || !access_ok_user_range((uint64_t)dirp, count * sizeof(indevos_dirent_t)))
        return -EFAULT;

    const char *path = (const char *)proc->fds[fd].file_handle;
    if (!path)
        return -EIO;

    /* The listing is rebuilt on every call and sliced at fd.position.
     * It used to live in a 256-entry array on the 16 KiB kernel stack
     * (about 68 KiB), so a big directory overwrote whatever lay below the
     * stack and capped listings at 256 names. */
    vfs_dirent_t *kents;
    int total;

    if (!vfs_has_root_mount())
        return -EIO;
    uint32_t pos = proc->fds[fd].position;
    size_t want = count > 256 ? 256 : count;
    kents = (vfs_dirent_t *)kalloc(sizeof(vfs_dirent_t) * want);
    if (!kents)
        return -ENOMEM;
    total = vfs_dircache_read(proc->fds[fd].file_handle, path, pos, kents, (uint32_t)want, NULL, 0);
    if (total < 0) {
        kfree(kents);
        return total == -ENOMEM ? -ENOMEM : -EIO;
    }
    if (total == 0) {
        kfree(kents);
        return 0;
    }

    size_t room = (size_t)total;

    for (size_t i = 0; i < room; i++) {
        indevos_dirent_t out;
        memset(&out, 0, sizeof(out));
        vfs_dirent_t *src = &kents[i];
        size_t nl = strlen(src->name);
        if (nl >= INDEVOS_NAME_MAX)
            nl = INDEVOS_NAME_MAX - 1;
        memcpy(out.d_name, src->name, nl);
        out.d_name[nl] = '\0';
        out.d_ino = src->inode;
        out.d_type = src->type;
        out.d_size = src->size;
        if (copy_to_user((uint8_t *)dirp + i * sizeof(out), &out, sizeof(out)) != 0) {
            kfree(kents);
            return -EFAULT;
        }
    }

    kfree(kents);
    proc->fds[fd].position = pos + (uint32_t)room;
    return (int)room;
}

int sys_network(int operation, void *arg1, void *arg2, void *arg3) {
    (void)arg2; (void)arg3;
    
    switch (operation) {
        case NET_GET_MAC: {
            if (!arg1 || !access_ok_user_range((uint64_t)arg1, 6))
                return -EFAULT;

            if (!net_device_get_current() || !net_is_initialized())
                return -ENODEV;

            net_device_t *nd = net_device_get_current();
            if (!nd->ops.send_packet)
                return -ENODEV;

            uint8_t mac[6];
            net_get_mac_address(mac);
            int nonzero = 0;
            for (int i = 0; i < 6; i++) {
                if (mac[i] != 0) {
                    nonzero = 1;
                    break;
                }
            }
            if (!nonzero)
                return -ENODEV;

            if (copy_to_user(arg1, mac, sizeof(mac)) != 0)
                return -EFAULT;
            return 0;
        }
        case NET_GET_STATUS: {
            if (!net_device_get_current() || !net_is_initialized())
                return 0;
            net_device_t *nd = net_device_get_current();
            if (!nd->ops.send_packet)
                return 0;
            return 1;
        }
        default:
            return -EINVAL;
    }
}

/* SDL syscalls live in syscall_sdl.c */

// POSIX-compliant syscall implementations

/**
 * @brief Duplicate a file descriptor
 *
 * Creates a copy of the file descriptor oldfd, using the lowest-numbered
 * available file descriptor greater than or equal to 0.
 */
int sys_dup(int oldfd) {
    process_t *proc = get_current_process();

    // Security: Validate file descriptor bounds
    if (oldfd < 0 || oldfd >= MAX_OPEN_FILES_SYSCALL || !proc->fds[oldfd].in_use) {
        return -EBADF;
    }

    int newfd = allocate_fd(proc);
    if (newfd < 0) {
        return -EMFILE;
    }

    // Copy the file descriptor entry
    memcpy(&proc->fds[newfd], &proc->fds[oldfd], sizeof(file_descriptor_t));
    proc->fds[newfd].cloexec = false;   /* dup/dup2 never copy FD_CLOEXEC */
    if (proc->fds[newfd].ref_count) {
        (*proc->fds[newfd].ref_count)++;
    } else {
        proc->fds[newfd].ref_count = fd_refcount_create();
        if (!proc->fds[newfd].ref_count) {
            free_fd(proc, newfd);
            return -ENOMEM;
        }
    }
    if (proc->fds[newfd].fd_type == FD_TYPE_PIPE_READ)
        pipe_fork_dup_reader((pipe_t *)proc->fds[newfd].file_handle);
    else if (proc->fds[newfd].fd_type == FD_TYPE_PIPE_WRITE)
        pipe_fork_dup_writer((pipe_t *)proc->fds[newfd].file_handle);
    return newfd;
}

/**
 * @brief Duplicate a file descriptor to a specific number
 *
 * Creates a copy of the file descriptor oldfd, using the file descriptor
 * number specified in newfd. If newfd is already open, it is closed first.
 */
int sys_dup2(int oldfd, int newfd) {
    process_t *proc = get_current_process();

    // Security: Validate old file descriptor
    if (oldfd < 0 || oldfd >= MAX_OPEN_FILES_SYSCALL || !proc->fds[oldfd].in_use) {
        return -EBADF;
    }

    // Security: Validate new file descriptor bounds
    if (newfd < 0 || newfd >= MAX_OPEN_FILES_SYSCALL) {
        return -EBADF;
    }

    if (oldfd == newfd) {
        return newfd;
    }

    // If newfd is already open, close it first
    if (proc->fds[newfd].in_use) {
        if (fd_close_entry(proc, newfd) != 0) {
            return -EIO;
        }
    }

    // Copy the file descriptor entry
    memcpy(&proc->fds[newfd], &proc->fds[oldfd], sizeof(file_descriptor_t));
    proc->fds[newfd].cloexec = false;   /* dup/dup2 never copy FD_CLOEXEC */
    proc->fds[newfd].in_use = true;
    if (proc->fds[newfd].ref_count) {
        (*proc->fds[newfd].ref_count)++;
    } else {
        proc->fds[newfd].ref_count = fd_refcount_create();
        if (!proc->fds[newfd].ref_count) {
            free_fd(proc, newfd);
            return -ENOMEM;
        }
    }
    if (proc->fds[newfd].fd_type == FD_TYPE_PIPE_READ)
        pipe_fork_dup_reader((pipe_t *)proc->fds[newfd].file_handle);
    else if (proc->fds[newfd].fd_type == FD_TYPE_PIPE_WRITE)
        pipe_fork_dup_writer((pipe_t *)proc->fds[newfd].file_handle);

    return newfd;
}

/**
 * @brief Create a pipe
 *
 * Creates a unidirectional data channel that can be used for interprocess communication.
 * pipefd[0] is the read end, pipefd[1] is the write end.
 */
int sys_pipe(int pipefd[2]) {
    if (!pipefd || !access_ok_user_range((uint64_t)pipefd, sizeof(int) * 2))
        return -EFAULT;

    process_t *proc = get_current_process();
    pipe_t *p = pipe_create();
    if (!p)
        return -ENOMEM;

    int read_fd = allocate_fd(proc);
    if (read_fd < 0) {
        pipe_destroy(p);
        return -EMFILE;
    }
    int write_fd = allocate_fd(proc);
    if (write_fd < 0) {
        free_fd(proc, read_fd);
        pipe_destroy(p);
        return -EMFILE;
    }

    memset(&proc->fds[read_fd], 0, sizeof(file_descriptor_t));
    memset(&proc->fds[write_fd], 0, sizeof(file_descriptor_t));
    proc->fds[read_fd].in_use = true;
    proc->fds[write_fd].in_use = true;
    proc->fds[read_fd].fd_type = FD_TYPE_PIPE_READ;
    proc->fds[write_fd].fd_type = FD_TYPE_PIPE_WRITE;
    proc->fds[read_fd].file_handle = p;
    proc->fds[write_fd].file_handle = p;
    proc->fds[read_fd].ref_count = fd_refcount_create();
    proc->fds[write_fd].ref_count = fd_refcount_create();
    if (!proc->fds[read_fd].ref_count || !proc->fds[write_fd].ref_count) {
        if (proc->fds[read_fd].ref_count) {
            kfree(proc->fds[read_fd].ref_count);
        }
        if (proc->fds[write_fd].ref_count) {
            kfree(proc->fds[write_fd].ref_count);
        }
        free_fd(proc, read_fd);
        free_fd(proc, write_fd);
        pipe_destroy(p);
        return -ENOMEM;
    }

    int out[2] = { read_fd, write_fd };
    if (copy_to_user(pipefd, out, sizeof(out)) != 0) {
        fd_close_entry(proc, read_fd);
        fd_close_entry(proc, write_fd);
        return -EFAULT;
    }
    return 0;
}

/**
 * @brief File control operations
 *
 * Performs various operations on file descriptors.
 */
int sys_fcntl(int fd, int cmd, int arg) {
    process_t *proc = get_current_process();

    if (fd < 0 || fd >= MAX_OPEN_FILES_SYSCALL || !proc->fds[fd].in_use)
        return -EBADF;

    switch (cmd) {
        case F_DUPFD: {
            int minfd = arg;
            if (minfd < 0)
                return -EINVAL;
            if (minfd < 3 && !proc->linux_abi)
                minfd = 3;
            int newfd = -1;
            for (int i = minfd; i < MAX_OPEN_FILES_SYSCALL; i++) {
                if (!proc->fds[i].in_use) {
                    newfd = i;
                    break;
                }
            }
            if (newfd < 0)
                return -EMFILE;
            memset(&proc->fds[newfd], 0, sizeof(file_descriptor_t));
            memcpy(&proc->fds[newfd], &proc->fds[fd], sizeof(file_descriptor_t));
            proc->fds[newfd].in_use = true;
            proc->fds[newfd].cloexec = false;
            if (proc->fds[newfd].ref_count)
                (*proc->fds[newfd].ref_count)++;
            else {
                proc->fds[newfd].ref_count = fd_refcount_create();
                if (!proc->fds[newfd].ref_count) {
                    free_fd(proc, newfd);
                    return -ENOMEM;
                }
            }
            if (proc->fds[newfd].fd_type == FD_TYPE_PIPE_READ)
                pipe_fork_dup_reader((pipe_t *)proc->fds[newfd].file_handle);
            else if (proc->fds[newfd].fd_type == FD_TYPE_PIPE_WRITE)
                pipe_fork_dup_writer((pipe_t *)proc->fds[newfd].file_handle);
            return newfd;
        }

        case F_GETFD:
            return proc->fds[fd].cloexec ? FD_CLOEXEC : 0;

        case F_SETFD:
            proc->fds[fd].cloexec = (arg & FD_CLOEXEC) != 0;
            return 0;

        case F_GETFL:
            return (int)proc->fds[fd].flags;

        case F_SETFL:
            proc->fds[fd].flags = (uint32_t)arg;
            return 0;

        default:
            return -EINVAL;
    }
}

/**
 * @brief I/O control operations
 *
 * Performs device-specific operations on file descriptors.
 */
int sys_ioctl(int fd, unsigned long request, void *arg) {
    process_t *proc = get_current_process();

    // Security: Validate file descriptor bounds
    if (fd < 0 || fd >= MAX_OPEN_FILES_SYSCALL || !proc->fds[fd].in_use) {
        return -EBADF;
    }

    /* VFS-backed fds (e.g. dev dri nodes): ioctl with user-buffered arg */
    if (proc->fds[fd].fd_type == FD_TYPE_VFS && proc->fds[fd].vfs_fd >= 0) {
        unsigned long sz = _IOC_SIZE(request);
        if (sz > 16384)
            return -(int64_t)EINVAL;

        void *karg = NULL;
        if (sz > 0) {
            karg = kalloc(sz);
            if (!karg)
                return -(int64_t)ENOMEM;
            unsigned dir = _IOC_DIR(request);
            if (dir & _IOC_WRITE) {
                if (!arg || copy_from_user(karg, arg, sz) != 0) {
                    kfree(karg);
                    return -(int64_t)EFAULT;
                }
            } else {
                memset(karg, 0, sz);
            }
        }

        int ret = vfs_ioctl(proc->fds[fd].vfs_fd, request, karg);

        if (ret == 0 && karg && sz > 0 && (_IOC_DIR(request) & _IOC_READ) && arg) {
            if (copy_to_user(arg, karg, sz) != 0) {
                kfree(karg);
                return -(int64_t)EFAULT;
            }
        }
        if (karg)
            kfree(karg);
        return ret;
    }

    // Security: Validate request bounds to prevent ioctl attacks
    if (request > 0xFFFF) {
        return -EINVAL;
    }

    switch (request) {
        case FIONREAD:
            // Get number of bytes available to read
            if (arg) {
                int value = 0;
                if (fd >= 0 && fd < MAX_OPEN_FILES_SYSCALL && proc->fds[fd].in_use &&
                    proc->fds[fd].fd_type == FD_TYPE_PIPE_READ && proc->fds[fd].file_handle)
                    value = (int)pipe_available((pipe_t *)proc->fds[fd].file_handle);
                if (copy_to_user(arg, &value, sizeof(value)) != 0) {
                    return -EFAULT;
                }
                return 0;
            }
            return -EFAULT;

        case FIONBIO:
            // Set/clear non-blocking I/O
            if (arg) {
                int nonblock = 0;
                if (copy_from_user(&nonblock, arg, sizeof(nonblock)) != 0) {
                    return -EFAULT;
                }
                if (nonblock) {
                    proc->fds[fd].flags |= O_NONBLOCK;
                } else {
                    proc->fds[fd].flags &= ~O_NONBLOCK;
                }
                return 0;
            }
            return -EFAULT;

        default:
            return -ENOTTY;  // Unsupported ioctl
    }
}

/* ---- Kernel-side stdio plumbing for the desktop terminal window ----
 * The desktop terminal spawns a shell whose stdin/stdout/stderr are pipes
 * held by the kernel (a pty-like pair without a line discipline). */
int process_install_pipe_fd(process_t *p, int fd, pipe_t *pp, bool write_end) {
    if (!p || !pp || fd < 0 || fd > 2)
        return -EINVAL;
    file_descriptor_t *f = &p->fds[fd];
    if (f->in_use && !(f->fd_type == FD_TYPE_FILE && f->file_handle == NULL))
        fd_close_entry(p, fd);
    memset(f, 0, sizeof(*f));
    f->ref_count = fd_refcount_create();
    if (!f->ref_count)
        return -ENOMEM;
    f->in_use = true;
    f->fd_type = write_end ? FD_TYPE_PIPE_WRITE : FD_TYPE_PIPE_READ;
    f->file_handle = pp;
    f->vfs_fd = -1;
    return 0;
}

static bool process_is_descendant(process_t *p, uint32_t root_pid) {
    int depth = 0;
    for (process_t *q = p->parent; q && depth < MAX_PROCESSES; q = q->parent, depth++)
        if (q->pid == root_pid)
            return true;
    return false;
}

/* Post @sig to every running descendant of @root_pid (and to the root
 * itself when @include_root). SIGKILL uses the safe deferred-kill path. */
int process_signal_tree(uint32_t root_pid, int sig, bool include_root) {
    int n = 0;
    if (root_pid == 0 || sig < 1 || sig >= NSIG)
        return 0;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t *t = &processes[i];
        if (!t->is_running || t->is_zombie || t->pid == 0)
            continue;
        bool hit = (t->pid == root_pid) ? include_root
                                        : process_is_descendant(t, root_pid);
        if (!hit)
            continue;
        if (sig == SIGKILL)
            process_request_kill(t, sig);
        else
            process_post_signal(t, sig);
        n++;
    }
    return n;
}

bool process_pid_alive(uint32_t pid) {
    if (pid == 0) return false;
    for (int i = 0; i < MAX_PROCESSES; i++)
        if (processes[i].pid == pid && processes[i].is_running &&
            !processes[i].is_zombie)
            return true;
    return false;
}

/* ── Helpers for AF_UNIX (net_unix.c) ────────────────────────────────────
 * SIGPIPE on a broken stream socket, same policy as pipe writes. */
void proc_raise_sigpipe(process_t *proc) {
    if (!proc) return;
    if (!(proc->blocked_signals & (1u << SIGPIPE)) &&
        proc->signal_handlers[SIGPIPE] != SIG_IGN) {
        proc->pending_signals |= (1u << SIGPIPE);
        sighandler_t sh = proc->signal_handlers[SIGPIPE];
        if (sh == SIG_DFL || sh == SIG_ERR || !sh)
            handle_pending_signals(proc);
    }
}

/* SCM_RIGHTS: take an extra reference on fd (like dup) into *out. */
int fd_flight_get(process_t *proc, int fd, file_descriptor_t *out) {
    if (!proc || fd < 0 || fd >= MAX_OPEN_FILES_SYSCALL || !proc->fds[fd].in_use)
        return -EBADF;
    if (!proc->fds[fd].ref_count) {
        proc->fds[fd].ref_count = fd_refcount_create();
        if (!proc->fds[fd].ref_count) return -ENOMEM;
    } else {
        (*proc->fds[fd].ref_count)++;
    }
    memcpy(out, &proc->fds[fd], sizeof(*out));
    out->cloexec = false;
    if (out->fd_type == FD_TYPE_PIPE_READ)
        pipe_fork_dup_reader((pipe_t *)out->file_handle);
    else if (out->fd_type == FD_TYPE_PIPE_WRITE)
        pipe_fork_dup_writer((pipe_t *)out->file_handle);
    return 0;
}

/* Install an in-flight descriptor into proc (lowest free slot). */
int fd_flight_install(process_t *proc, const file_descriptor_t *f, bool cloexec) {
    if (!proc) return -EINVAL;
    int nfd = allocate_fd(proc);
    if (nfd < 0) return nfd;
    memcpy(&proc->fds[nfd], f, sizeof(*f));
    proc->fds[nfd].in_use = true;
    proc->fds[nfd].cloexec = cloexec;
    return nfd;
}

/* Drop an in-flight descriptor that was never received: park it in a free
 * slot of the current process and close it there (normal release path). */
void fd_flight_release(file_descriptor_t *f) {
    process_t *proc = get_current_process();
    if (!proc) return;
    int nfd = fd_flight_install(proc, f, false);
    if (nfd >= 0) fd_close_entry(proc, nfd);
}
