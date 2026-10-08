#pragma once

#include "types.h"
#include "atomic.h"
#include "mem.h"

// Thread states
typedef enum {
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_BLOCKED,
    THREAD_SLEEPING,
    THREAD_TERMINATED
} thread_state_t;

// Thread priority levels
typedef enum {
    PRIORITY_LOW = 0,
    PRIORITY_NORMAL = 1,
    PRIORITY_HIGH = 2,
    PRIORITY_CRITICAL = 3
} thread_priority_t;

#define THREAD_NUM_PRIORITIES 4

// Saved GP register state for preemptive context switching.
// Mirrors the interrupt frame pushed by idtasm.asm + CPU.
typedef struct {
    uint64_t rdi, rsi, rbp, rbx, rdx, rcx, rax;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    // iretq frame fields
    uint64_t rip, cs, rflags, rsp, ss;
} thread_regs_t;

// Forward-declare process_t so thread_t can hold a pointer to it
struct process;

// Thread control block
typedef struct thread {
    uint64_t id;                    // Unique thread ID
    char name[32];                  // Thread name for debugging
    void (*entry_point)(void*);     // Thread entry function
    void *arg;                      // Argument passed to entry function
    uint64_t stack_base;            // Stack base address
    uint64_t stack_size;            // Stack size
    uint64_t rsp;                   // Current stack pointer (legacy, kept for cooperative path)
    uint64_t rip;                   // Current instruction pointer (legacy)
    thread_regs_t saved_regs;       // Full GP register state for preemptive scheduling
    uint8_t fxsave_area[512] __attribute__((aligned(16)));  // FPU/SSE state (FXSAVE format)
    bool has_saved_context;         // True after thread has been preempted at least once
    thread_state_t state;           // Current thread state
    thread_priority_t priority;     // Thread priority
    uint64_t sleep_until;           // Timestamp for sleep wakeup
    struct thread *next;            // Next thread in list
    struct thread *prev;            // Previous thread in list
    spinlock_t lock;                // Thread lock for synchronization
    struct process *owner_process;  // Owning process (NULL for kernel threads)
    uint32_t home_cpu;              // Run-queue owner
    uint64_t fs_base;               // IA32_FS_BASE (user TLS, Linux ABI arch_prctl)
    /* Per-thread live SYSCALL/INT80 user frame. Captured on EVERY entry
     * before dispatch. Fork/clone snapshots FROM HERE by value — never from
     * a process-global stash a sibling could clear mid-spawn. */
    uint64_t sc_user_rip;
    uint64_t sc_user_rsp;
    uint64_t sc_user_rflags;
    uint64_t sc_r15, sc_r14, sc_r13, sc_r12;
    uint64_t sc_rbp, sc_rbx;
    uint64_t sc_r9, sc_r8, sc_r10, sc_rdx, sc_rsi, sc_rdi;
    int      sc_frame_valid;
    /* SMP: 1 while this thread's context is live on some CPU. Set at
     * switch-in; cleared (deferred) only after the outgoing CPU has left
     * this thread's stack. Pickers never dequeue an on_cpu thread. */
    volatile int on_cpu;
    /* 1 = user thread parked in-kernel (wait/sleep) — preemptible with a
     * ring-0 frame; 2 = BSP boot thread (ash) parked in sys_wait. */
    volatile int kwait;
} thread_t;

/* FS base is per thread: Linux/musl programs point it at their TLS block with
 * arch_prctl(ARCH_SET_FS).  The kernel itself never uses FS, so the context
 * switch only has to save the outgoing value and load the incoming one. */
#define THREAD_MSR_FS_BASE 0xC0000100u
static inline uint64_t thread_rdfsbase(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(THREAD_MSR_FS_BASE));
    return ((uint64_t)hi << 32) | lo;
}
static inline void thread_wrfsbase(uint64_t v) {
    __asm__ volatile("wrmsr" :: "a"((uint32_t)v), "d"((uint32_t)(v >> 32)),
                     "c"(THREAD_MSR_FS_BASE) : "memory");
}
static inline void thread_switch_fs(thread_t *from, thread_t *to) {
    if (from)
        from->fs_base = thread_rdfsbase();
    if (to && (!from || from->fs_base != to->fs_base))
        thread_wrfsbase(to->fs_base);
}

// Scheduler structure
typedef struct {
    thread_t *current_thread;       // Currently running thread
    // Per-priority FIFO ready queues (index = thread_priority_t).
    // Dispatch always takes the head of the highest non-empty level.
    // Invariant: a RUNNING thread is never linked in any ready queue.
    thread_t *ready_queue[THREAD_NUM_PRIORITIES];
    thread_t *ready_queue_tail[THREAD_NUM_PRIORITIES];
    thread_t *blocked_queue;        // Blocked threads queue
    thread_t *sleeping_queue;       // Sleeping threads queue (sorted by sleep_until)
    uint64_t next_thread_id;        // Next available thread ID
    spinlock_t scheduler_lock;      // Scheduler lock
    bool scheduler_running;         // Scheduler state
} scheduler_t;

// Thread management functions
thread_t* thread_create(const char *name, void (*entry)(void*), void *arg, 
                       uint64_t stack_size, thread_priority_t priority);
void thread_destroy(thread_t *thread);
void thread_yield(void);
void thread_sleep(uint64_t milliseconds);
void thread_wake(thread_t *thread);
/* on_cpu handoff (see thread_t.on_cpu). */
void sched_oncpu_set(thread_t *t);
void sched_oncpu_defer_clear(thread_t *t);
void sched_oncpu_flush(void);
void sched_oncpu_forget(thread_t *t);

/* Deferred reaping of exited threads.  A thread that has terminated (and
 * optionally the per-process kernel stack it was last running on) is queued
 * with thread_mark_dead(); thread_reaper_tick() (timer path, every CPU) and
 * thread_create() free it once no CPU can still be running on, switching to
 * or pointing at it.  Never kfree a stack a CPU may still be using. */
void thread_mark_dead(thread_t *t, void *kstack);
void thread_reap_dead(void);
void thread_reaper_tick(void);
void thread_reset_start_context(thread_t *thread);
/* Lost-wakeup wait helpers (hold scheduler_lock via scheduler_irq_*). */
void scheduler_irq_lock(uint64_t *flags);
void scheduler_irq_unlock(uint64_t flags);
void thread_block_current_unlocked(void);
void thread_unblock_current_running_unlocked(void);
void thread_wake_blocked_unlocked(thread_t *thread);
void scheduler_kick_thread(thread_t *thread);
void thread_set_priority(thread_t *thread, thread_priority_t priority);

// Scheduler functions
void scheduler_init(void);
void scheduler_start(void);
void scheduler_stop(void);
thread_t* scheduler_get_current_thread(void);
thread_t* scheduler_get_next_thread(void);
void scheduler_add_thread(thread_t *thread);
void scheduler_remove_thread(thread_t *thread);
/** Remove from all queues and clear current_thread if it matches (atomic). */
void scheduler_detach_thread(thread_t *thread);

/** True if any ready queue is non-empty (lock-free fast-path check). */
bool scheduler_has_ready_threads(void);
/** True if a Ring-3 thread is READY (owner_process set). */
bool scheduler_has_ready_user_threads(void);
/** Wake all sleeping threads whose deadline <= now (one lock acquisition).
 *  Woken threads are queued at the FRONT of their priority level so timed
 *  wakeups (frame pacing, input polls) run with minimal latency. */
void scheduler_wake_expired(uint64_t now);
/** Dequeue the highest-priority READY user-mode thread (owner_process set),
 *  leaving kernel threads and their ordering untouched. NULL if none. */
thread_t* scheduler_pick_user_thread(void);
/** User thread if one is ready, else a kernel thread with a saved iretq
 *  frame (not the idle thread). Used by the timer / reschedule-IPI path. */
thread_t* scheduler_pick_preempt_thread(void);

// Thread context switching
void thread_switch(thread_t *from, thread_t *to);
void thread_save_context(thread_t *thread);
void thread_restore_context(thread_t *thread);

// Called when a thread's entry function returns (cleanup + yield)
void thread_exit(void);

// Utility functions
uint64_t thread_get_id(void);
const char* thread_get_name(void);
thread_state_t thread_get_state(thread_t *thread);

// Global scheduler instance
extern scheduler_t global_scheduler;

/* When true, every context switch is logged to the serial port with
 * microsecond timestamps (see scheduler_log_switch). Toggle at runtime
 * from the zeus shell with `scheddebug on|off`. */
extern bool sched_debug_switches;

void scheduler_log_switch(thread_t *from, thread_t *to, const char *kind,
                          uint64_t rip); 