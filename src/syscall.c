#include "drm.h"
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
    process_t *p = exec_user_as_process();
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
    if (t && t->owner_process)
        return (process_t *)t->owner_process;
    return current_process;
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
    for (int i = 3; i < MAX_OPEN_FILES_SYSCALL; i++) {  // Start from 3 (skip stdio)
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

static int fd_close_entry(process_t *proc, int fd) {
    if (!proc || fd < 0 || fd >= MAX_OPEN_FILES_SYSCALL || !proc->fds[fd].in_use) {
        return -EBADF;
    }

    file_descriptor_t *fde = &proc->fds[fd];
    bool should_free_object = true;
    if (fde->ref_count) {
        if (*fde->ref_count > 1) {
            (*fde->ref_count)--;
            should_free_object = false;
        } else {
            kfree(fde->ref_count);
        }
    }

    if (should_free_object) {
        if (fde->fd_type == FD_TYPE_SOCKET) {
            kernel_socket_t *sock = (kernel_socket_t *)fde->file_handle;
            if (sock) {
                kernel_socket_destroy(sock);
            }
        } else if (fde->fd_type == FD_TYPE_PIPE_READ) {
            pipe_close_read((pipe_t *)fde->file_handle);
        } else if (fde->fd_type == FD_TYPE_PIPE_WRITE) {
            pipe_close_write((pipe_t *)fde->file_handle);
        } else if (fde->fd_type == FD_TYPE_VFS) {
            if (fde->vfs_fd >= 0) {
                vfs_close(fde->vfs_fd);
            }
        } else if (fde->fd_type == FD_TYPE_VFS_DIR) {
            if (fde->file_handle) {
                kfree(fde->file_handle);
            }
        }
    }

    free_fd(proc, fd);
    return 0;
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

void process_cleanup(process_t *proc) {
    if (!proc) return;

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
            if (processes[i].is_zombie)
                memset(&processes[i], 0, sizeof(process_t));
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
            serial_printf("syscall: SIGINT queued for PID %d (graceful)\n",
                          (int)proc->pid);
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
    return ret;
}

// Syscall implementations
int sys_exit(int status) {
    SERIAL_DEBUG_PRINT(SYSCALL, "sys_exit called with status %d\n", status);

    process_t *proc = current_process;
    if (proc) {
        proc->exit_status = status;
        proc->is_running  = false;
    }

    /* If this is a vfork child, return to the parent's fork() call
     * instead of longjmping to exec_run_elf. */
    if (proc && proc->fork_parent) {
        proc->is_zombie = true;
        process_t *parent = proc->fork_parent;
        int child_pid = (int)proc->pid;
        proc->fork_parent = NULL;
        current_process = parent;
        exec_set_ring3_active(parent);

        serial_printf("fork-exit: child PID %d exited (%d), resuming parent %d\n",
                      child_pid, status, (int)parent->pid);

        exec_longjmp((exec_jmpbuf_t *)parent->fork_jmpbuf, child_pid);
        __builtin_unreachable();
    }

    /* Spawned (preemptively-scheduled) process: thread-based exit. */
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
            char ch = (char)sys_getchar();
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
                /* Our fork() is vfork-shaped: the producer ran to completion
                 * before this reader, but the suspended parent may still hold
                 * the write end so writers>0 forever. No concurrent user
                 * writer can run on this thread, so empty == EOF.
                 * (Avoid spinning on always-READY idle threads.) */
                kfree(kbuf);
                return 0;
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
                if (proc->fork_parent) {
                    kfree(kbuf);
                    return -EAGAIN;
                }
                kfree(kbuf);
                return -EAGAIN;
            }
            kfree(kbuf);
            if (n < 0) return -EPIPE;
            return n;
        }
    } else if (proc->fds[fd].fd_type == FD_TYPE_VFS) {
        void *kbuf = kalloc(count);
        if (!kbuf) return -ENOMEM;
        if (copy_from_user(kbuf, buf, count) != 0) {
            kfree(kbuf);
            return -EFAULT;
        }
        int bytes_written = vfs_write(proc->fds[fd].vfs_fd, kbuf, count);
        if (bytes_written >= 0) {
            proc->fds[fd].position += bytes_written;
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
        return -ENOENT;
    }

    /* All file access goes through the VFS; mounting a filesystem registers
     * it with the VFS (see mount_filesystem_device / vfs_fat32_register). */
    return -ENOENT;
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

void* sys_brk(void *addr) {
    process_t *proc = exec_user_as_process();

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
    if (length == 0 || length > (256ull << 20))
        return MMAP_ERR(EINVAL);
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

    for (size_t p = 0; p < pages; p++) {
        uint64_t va = v0 + p * PAGE_SIZE;
        uint64_t frame = vmm_alloc_frame();
        if (!frame)
            return MMAP_ERR(ENOMEM);
        memset(phys_to_virt(frame), 0, PAGE_SIZE);
        if (vmm_map_page_in(proc->pml4, va, frame, pte_flags) != 0) {
            vmm_free_frame(frame);
            return MMAP_ERR(ENOMEM);
        }
    }

    if (mmap_region_prepend(proc, v0, pages * PAGE_SIZE) != 0) {
        for (size_t p = 0; p < pages; p++)
            vmm_unmap_page_release_in(proc->pml4, v0 + p * PAGE_SIZE);
        return MMAP_ERR(ENOMEM);
    }

    return (void *)v0;
#undef MMAP_ERR
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
        /* Preserve COW bit if set so fork semantics stay intact. */
        if (*pte & VMM_COW)
            flags = (flags & ~VMM_WRITABLE) | VMM_COW;
        *pte = phys | flags;
        __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
    }
    return 0;
}


int sys_munmap(void *addr, size_t length) {
    process_t *proc = exec_user_as_process();
    if (!addr || length == 0)
        return -EINVAL;

    if (!proc->pml4 || !proc->is_usermode) {
        kfree(addr);
        return 0;
    }

    uint64_t v = (uint64_t)addr & ~(uint64_t)(PAGE_SIZE - 1);
    size_t pages = (length + ((uint64_t)addr - v) + PAGE_SIZE - 1) / PAGE_SIZE;

    proc_mmap_region_t **pp = &proc->mmap_regions;
    while (*pp) {
        proc_mmap_region_t *r = *pp;
        if (r->virt == v && r->len >= pages * PAGE_SIZE) {
            /* Release the physical frames, not just the PTEs — otherwise
             * every munmap permanently leaks RAM. */
            for (size_t p = 0; p < pages; p++)
                vmm_unmap_page_release_in(proc->pml4, v + p * PAGE_SIZE);
            *pp = r->next;
            kfree(r);
            return 0;
        }
        pp = &r->next;
    }

    for (size_t p = 0; p < pages; p++)
        vmm_unmap_page_release_in(proc->pml4, v + p * PAGE_SIZE);
    return 0;
}

int sys_getpid(void) {
    return get_current_process()->pid;
}

// Find a free process slot
static process_t* allocate_process(void) {
    for (int i = 1; i < MAX_PROCESSES; i++) { // Skip slot 0 (kernel process)
        if (processes[i].pid == 0) {
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
        vmm_destroy_address_space(proc->pml4);
        proc->pml4 = NULL;
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

int sys_fork(void) {
    process_t *parent = current_process;
    process_t *child = allocate_process();

    if (!child)
        return -ENOMEM;

    /* Field-by-field init instead of wholesale memcpy to avoid aliasing
     * synchronization-sensitive pointers and stale state. */
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
    child->linux_ti_valid = false;
    child->linux_clear_child_tid = 0;
    kstrncpy(child->linux_exe_path, parent->linux_exe_path,
             sizeof(child->linux_exe_path));
    child->mmap_regions = NULL;
    child->pending_signals = 0;
    child->blocked_signals = parent->blocked_signals;
    for (int i = 0; i < NSIG; i++) {
        child->signal_handlers[i] = parent->signal_handlers[i];
        child->signal_sa_flags[i] = parent->signal_sa_flags[i];
    }
    child->user_symtab = NULL;
    child->user_symtab_count = 0;
    child->user_sym_string_pool = NULL;

    if (parent->pml4 && parent->is_usermode) {
        uint64_t *newpml4 = vmm_clone_address_space(parent->pml4);
        if (!newpml4) {
            child->pid = 0;
            parent->child = child->sibling;
            return -ENOMEM;
        }
        uint8_t *ks = (uint8_t *)kalloc(INDEVOS_KSTACK_BYTES);
        if (!ks) {
            vmm_destroy_address_space(newpml4);
            child->pid = 0;
            parent->child = child->sibling;
            return -ENOMEM;
        }
        child->pml4 = newpml4;
        child->kernel_stack = ks;
        child->kernel_stack_top = (uint64_t)ks + INDEVOS_KSTACK_BYTES;
    } else if (parent->kernel_stack) {
        uint8_t *ks = (uint8_t *)kalloc(INDEVOS_KSTACK_BYTES);
        if (!ks) {
            child->pid = 0;
            parent->child = child->sibling;
            return -ENOMEM;
        }
        child->kernel_stack = ks;
        child->kernel_stack_top = (uint64_t)ks + INDEVOS_KSTACK_BYTES;
    }

    for (proc_mmap_region_t *r = parent->mmap_regions; r; r = r->next) {
        proc_mmap_region_t *nr = (proc_mmap_region_t *)kalloc(sizeof(proc_mmap_region_t));
        if (!nr)
            continue;
        nr->virt = r->virt;
        nr->len = r->len;
        nr->next = child->mmap_regions;
        child->mmap_regions = nr;
    }

    for (int i = 0; i < MAX_OPEN_FILES_SYSCALL; i++) {
        if (!parent->fds[i].in_use)
            continue;
        child->fds[i] = parent->fds[i];
        child->fds[i].in_use = true;
        if (child->fds[i].ref_count)
            (*child->fds[i].ref_count)++;
        if (child->fds[i].fd_type == FD_TYPE_PIPE_READ)
            pipe_fork_dup_reader((pipe_t *)child->fds[i].file_handle);
        else if (child->fds[i].fd_type == FD_TYPE_PIPE_WRITE)
            pipe_fork_dup_writer((pipe_t *)child->fds[i].file_handle);
    }

    child->fork_parent = NULL;
    child->fork_user_frame = NULL;
    child->fork_saved_user_rip = 0;

    serial_printf("fork: child PID %d from parent %d\n",
                  (int)child->pid, (int)parent->pid);

    /*
     * vfork semantics: the parent suspends while the child runs on the
     * same kernel thread.  When the child calls exec or exit, it longjmps
     * back here and the parent gets child_pid as the return value.
     */
    int resumed = exec_setjmp((exec_jmpbuf_t *)parent->fork_jmpbuf);
    if (resumed != 0) {
        /* Returned from the child's exec/exit longjmp.
         * 'resumed' is the child PID that was passed to exec_longjmp. */
        if (parent->kernel_stack_top)
            syscall_set_kernel_stack(parent->kernel_stack_top);
        if (parent->pml4)
            vmm_switch_address_space(parent->pml4);
        /* Restore Ring-3 identity so #PF/COW use the parent's PML4 again. */
        exec_set_ring3_active(parent);
        /* Re-write user RIP/RSP/RFLAGS into the original SYSCALL frame.
         * Layout must match linux_syscall_frame_t / syscall_entry. */
        if (parent->fork_user_frame && parent->fork_saved_user_rip) {
            uint64_t *f = (uint64_t *)parent->fork_user_frame;
            /* r15..rbx (6), a6..a1 (6), nr (1), rflags, rip, rsp */
            f[13] = parent->fork_saved_user_rflags;
            f[14] = parent->fork_saved_user_rip;
            f[15] = parent->fork_saved_user_rsp;
            serial_printf("fork: restored user_rip=0x%llx rsp=0x%llx\n",
                          (unsigned long long)parent->fork_saved_user_rip,
                          (unsigned long long)parent->fork_saved_user_rsp);
        } else {
            serial_printf("fork: WARNING no saved user frame (frame=%p rip=0x%llx)\n",
                          parent->fork_user_frame,
                          (unsigned long long)parent->fork_saved_user_rip);
        }
        parent->fork_user_frame = NULL;
        serial_printf("fork: parent PID %d resumed, child was %d\n",
                      (int)parent->pid, resumed);
        return resumed;
    }

    /* First call: hand execution to the child. */
    child->fork_parent = parent;
    current_process = child;
    /* Child's address space is active — #PF must COW against child PTE. */
    exec_set_ring3_active(child);

    /* Route future syscalls through the child's kernel stack so the
     * parent's stack (where the setjmp context lives) is preserved. */
    if (!child->kernel_stack_top) {
        serial_printf("fork: FATAL child PID %d has no kernel stack\n",
                      (int)child->pid);
        current_process = parent;
        exec_set_ring3_active(parent);
        child->fork_parent = NULL;
        return -ENOMEM;
    }
    if (child->kernel_stack_top == parent->kernel_stack_top) {
        serial_printf("fork: FATAL shared kernel stack parent=%d child=%d\n",
                      (int)parent->pid, (int)child->pid);
        current_process = parent;
        exec_set_ring3_active(parent);
        child->fork_parent = NULL;
        return -ENOMEM;
    }
    serial_printf("fork: kstack parent=0x%llx child=0x%llx\n",
                  (unsigned long long)parent->kernel_stack_top,
                  (unsigned long long)child->kernel_stack_top);
    syscall_set_kernel_stack(child->kernel_stack_top);
    if (child->pml4)
        vmm_switch_address_space(child->pml4);

    return 0;   /* child sees fork() == 0 */
}

int sys_wait(int *status) {
    process_t *parent = current_process;
    
    serial_puts("sys_wait called by PID ");
    serial_printf("%d", (int)parent->pid);
    serial_puts("\n");

    for (;;) {
    if (!parent->child)
        return -ECHILD;

    // Look for zombie children
    process_t *child = parent->child;
    while (child) {
        if (child->is_zombie) {
            serial_puts("Found zombie child PID ");
            serial_printf("%d", (int)child->pid);
            serial_puts(" with exit status ");
            serial_printf("%d", child->exit_status);
            serial_puts("\n");
            
            // Copy exit status if requested
            if (status) {
                if (copy_to_user(status, &child->exit_status, sizeof(child->exit_status)) != 0) {
                    return -EFAULT;
                }
            }
            
            pid_t child_pid = child->pid;
            
            // Remove child from parent's child list
            if (parent->child == child) {
                parent->child = child->sibling;
            } else {
                process_t *prev = parent->child;
                while (prev && prev->sibling != child) {
                    prev = prev->sibling;
                }
                if (prev) {
                    prev->sibling = child->sibling;
                }
            }
            
            process_release(child);
            memset(child, 0, sizeof(process_t));
            
            return child_pid;
        }
        child = child->sibling;
    }
    
        if (!parent->child) {
            serial_puts("No children left\n");
            return -ECHILD;
        }
        if (global_scheduler.scheduler_running)
            thread_sleep(1);
        else
            return -ECHILD;
    }
}

/* Terminate a process from the signal path.
 * - If it is the current process, just mark it; it will exit through its own
 *   exit path (sys_exit / exec longjmp).
 * - Otherwise stop its scheduler thread (if any), release resources and, when
 *   the parent is the kernel (which never calls sys_wait), reap immediately so
 *   the slot doesn't leak as a permanent zombie. */
static void process_signal_terminate(process_t *proc, int signum) {
    proc->exit_status = 128 + signum;
    proc->is_running  = false;

    if (proc == current_process) {
        proc->is_zombie = true;
        return;
    }

    if (proc->sched_thread) {
        thread_t *thr = (thread_t *)proc->sched_thread;
        thr->state = THREAD_TERMINATED;
        thr->owner_process = NULL;
        scheduler_detach_thread(thr);
        proc->sched_thread = NULL;
        desktop_destroy_windows_for_pid(proc->pid);
    }

    if (!proc->parent || proc->parent == &processes[0]) {
        process_release(proc);
        process_cleanup(proc);
    } else {
        proc->is_zombie = true;
    }
}

// Handle pending signals for a process
static void handle_pending_signals(process_t *proc) {
    for (int signum = 1; signum < NSIG; signum++) {
        if (proc->pending_signals & (1 << signum)) {
            // Clear the pending signal
            proc->pending_signals &= ~(1 << signum);
            
            sighandler_t handler = proc->signal_handlers[signum];
            
            serial_puts("Handling signal ");
            serial_printf("%d", signum);
            serial_puts(" for PID ");
            serial_printf("%d", (int)proc->pid);
            serial_puts("\n");
            
            if (handler == SIG_IGN) {
                // Ignore the signal
                serial_puts("Signal ignored\n");
            } else if (handler == SIG_DFL) {
                // Default action
                switch (signum) {
                    case SIGTERM:
                    case SIGINT:
                    case SIGQUIT:
                    case SIGKILL:
                        // Terminate process
                        serial_puts("Signal causes termination\n");
                        process_signal_terminate(proc, signum);
                        return; /* proc may be reaped — stop iterating */
                    case SIGCHLD:
                        // Ignore by default
                        break;
                    default:
                        // Most signals terminate by default
                        process_signal_terminate(proc, signum);
                        return;
                }
            } else {
                /* Should not happen: sys_signal rejects custom handlers. */
                process_signal_terminate(proc, signum);
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
    
    process_t *proc = current_process;
    sighandler_t old_handler = proc->signal_handlers[signum];
    
    // Some signals cannot be caught or ignored
    if (signum == SIGKILL || signum == SIGSTOP) {
        return SIG_ERR;
    }

    /* Custom handlers require a Ring-3 sigreturn trampoline (not implemented). */
    if (handler != SIG_DFL && handler != SIG_IGN) {
        return SIG_ERR;
    }
    
    proc->signal_handlers[signum] = handler;
    
    serial_puts("Signal ");
    serial_printf("%d", signum);
    serial_puts(" handler set for PID ");
    serial_printf("%d", (int)proc->pid);
    serial_puts("\n");
    
    return old_handler;
}

int sys_kill(pid_t pid, int signum) {
    if (signum < 1 || signum >= NSIG) {
        return -EINVAL;
    }
    
    process_t *sender = get_current_process();
    
    process_t *target = NULL;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (processes[i].pid == pid && processes[i].is_running) {
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
    
    target->pending_signals |= (1 << signum);
    handle_pending_signals(target);
    
    return 0;
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
static int exec_resolve_path(process_t *proc, const char *name, char *out, size_t outsz) {
    if (!name || !out || outsz < 4) return -EINVAL;
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

    for (int i = 3; i < MAX_OPEN_FILES_SYSCALL; i++) {
        if (p->fds[i].in_use && p->fds[i].cloexec)
            fd_close_entry(p, i);
    }
    char kargs[16][128];
    char *kargv[17];
    int argc = 0;
    if (argv && is_user_address((uint64_t)argv)) {
        for (; argc < 16; argc++) {
            char *uptr = NULL;
            if (copy_from_user(&uptr, &argv[argc], sizeof(uptr)) != 0)
                break;
            if (!uptr)
                break;
            if (!is_user_address((uint64_t)uptr))
                break;
            if (!strncpy_from_user(kargs[argc], uptr, sizeof(kargs[argc])))
                break;
            kargv[argc] = kargs[argc];
        }
    }
    kargv[argc] = NULL;
    char resolved[256];
    exec_resolve_path(p, kpath, resolved, sizeof(resolved));
    /* Nested sync exec (e.g. vfork child running wc) must not clobber the
     * outer ash/busybox exec_return_ctx or the next exec_file panics. */
    bool nested_exec = get_program_running();
    if (nested_exec)
        exec_push_return_ctx();
    int ret = exec_file(resolved, argc ? kargv : NULL);
    if (nested_exec)
        exec_pop_return_ctx();

    /*
     * If this is a vfork child, the exec'd program has completed.
     * Jump back to the parent's fork() instead of returning to the
     * child's userspace (which would hit stale GS / PML4 state).
     */
    if (p->fork_parent) {
        /* No GS fix-up needed: syscall_entry swaps GS back before it calls
         * into C, so no exit path leaves GS swapped any more. */
        exec_did_swapgs = false;

        p->exit_status = ret;
        p->is_running  = false;
        p->is_zombie   = true;

        process_t *parent = p->fork_parent;
        int child_pid = (int)p->pid;
        p->fork_parent = NULL;
        current_process = parent;
        exec_set_ring3_active(parent);

        serial_printf("fork-exec: child PID %d done (%d), resuming parent %d\n",
                      child_pid, ret, (int)parent->pid);

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
        if (global_scheduler.scheduler_running)
            thread_sleep(1);
        else
            __asm__ volatile("hlt");
    }
    return 0;
}

int sys_poll(pollfd_t *fds, unsigned int nfds, int timeout_ms) {
    syscall_uaccess_ensure_user_cr3();
    if (!fds || nfds == 0 || nfds > 64 ||
        !access_ok_user_range((uint64_t)fds, nfds * sizeof(pollfd_t)))
        return -EFAULT;

    extern uint64_t ktime_deadline_ms(uint64_t);
    extern bool     ktime_expired(uint64_t);
    uint64_t deadline;
    if (timeout_ms >= 0)
        deadline = ktime_deadline_ms((uint64_t)timeout_ms);
    else
        deadline = ktime_deadline_ms(60000);

    pollfd_t kfds[64];
    if (copy_from_user(kfds, fds, nfds * sizeof(pollfd_t)) != 0)
        return -EFAULT;

    process_t *proc = get_current_process();
    while (!ktime_expired(deadline)) {
        int cnt = 0;
        for (unsigned i = 0; i < nfds; i++) {
            kfds[i].revents = 0;
            int fd = kfds[i].fd;
            if (fd < 0)
                continue;
            if ((kfds[i].events & POLLIN) && fd == STDIN_FILENO &&
                keyboard_get_buffer_length() > 0) {
                kfds[i].revents |= POLLIN;
                cnt++;
            } else if ((kfds[i].events & POLLIN) && fd >= 0 && fd < MAX_OPEN_FILES_SYSCALL &&
                       proc->fds[fd].in_use && proc->fds[fd].fd_type == FD_TYPE_PIPE_READ) {
                if (pipe_available((pipe_t *)proc->fds[fd].file_handle) > 0) {
                    kfds[i].revents |= POLLIN;
                    cnt++;
                }
            } else if (fd >= 0 && fd < MAX_OPEN_FILES_SYSCALL &&
                       proc->fds[fd].in_use && proc->fds[fd].fd_type == FD_TYPE_SOCKET) {
                kernel_socket_t *ks = (kernel_socket_t *)proc->fds[fd].file_handle;
                if (ks && ks->type == SOCK_DGRAM) {
                    udp_pcb_t *pcb = (udp_pcb_t *)ks->transport;
                    if ((kfds[i].events & POLLIN) && pcb && udp_pcb_rx_ready(pcb) > 0) {
                        kfds[i].revents |= POLLIN;
                        cnt++;
                    }
                    if (kfds[i].events & POLLOUT) {
                        kfds[i].revents |= POLLOUT;
                        cnt++;
                    }
                } else {
                    tcp_socket_t *ts = ks ? (tcp_socket_t *)ks->transport : NULL;
                    tcp_connection_t *conn = ts ? ts->connection : NULL;
                    if ((kfds[i].events & POLLIN) && conn && conn->recv_buffer_used > 0) {
                        kfds[i].revents |= POLLIN;
                        cnt++;
                    }
                    if ((kfds[i].events & POLLOUT) && conn &&
                        conn->state == TCP_STATE_ESTABLISHED) {
                        kfds[i].revents |= POLLOUT;
                        cnt++;
                    }
                }
            }
            if ((kfds[i].events & POLLOUT) && fd >= 1 && fd <= 2) {
                kfds[i].revents |= POLLOUT;
                cnt++;
            }
        }
        if (cnt > 0) {
            if (copy_to_user(fds, kfds, nfds * sizeof(pollfd_t)) != 0)
                return -EFAULT;
            return cnt;
        }
        poll_network_once();
        if (global_scheduler.scheduler_running)
            thread_sleep(1);
        else
            __asm__ volatile("hlt");
    }
    if (copy_to_user(fds, kfds, nfds * sizeof(pollfd_t)) != 0)
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
    ktime_yield_until(ktime_deadline_ms(ms));
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
    
    serial_puts("Program mapping set: vbase=0x");
    serial_printf("%llx", virtual_base);
    serial_puts(" -> real=0x");
    serial_printf("%llx", (uint64_t)real_base);
    serial_puts(" size=");
    serial_printf("%llu", size);
    serial_puts("\n");
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
    if (r == -ENOTEMPTY)
        return -ENOTEMPTY;
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

    enum { BATCH = 256 };
    vfs_dirent_t kents[BATCH];
    int total;

    if (!vfs_has_root_mount())
        return -EIO;
    total = vfs_readdir(path, kents, BATCH);
    if (total < 0)
        return -EIO;

    uint32_t pos = proc->fds[fd].position;
    if (pos >= (uint32_t)total)
        return 0;

    size_t room = count;
    if (room > (size_t)(total - (int)pos))
        room = (size_t)(total - (int)pos);

    for (size_t i = 0; i < room; i++) {
        indevos_dirent_t out;
        memset(&out, 0, sizeof(out));
        vfs_dirent_t *src = &kents[pos + i];
        size_t nl = strlen(src->name);
        if (nl >= INDEVOS_NAME_MAX)
            nl = INDEVOS_NAME_MAX - 1;
        memcpy(out.d_name, src->name, nl);
        out.d_name[nl] = '\0';
        out.d_ino = src->inode;
        out.d_type = src->type;
        out.d_size = src->size;
        if (copy_to_user((uint8_t *)dirp + i * sizeof(out), &out, sizeof(out)) != 0)
            return -EFAULT;
    }

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
            if (minfd < 3)
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
