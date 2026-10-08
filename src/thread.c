#include "thread.h"
#include "fpu.h"
#include "hpet.h"
#include "ktime.h"
#include "serial.h"
#include "utils.h"
#include "string.h"
#include "smp.h"

// Global scheduler instance
scheduler_t global_scheduler = {0};

bool sched_debug_switches = false;

static uint64_t sched_last_switch_us;
static uint64_t sched_switch_count;

void scheduler_log_switch(thread_t *from, thread_t *to, const char *kind,
                          uint64_t rip) {
    if (!sched_debug_switches)
        return;

    uint64_t now = ktime_now_us();
    uint64_t delta = sched_last_switch_us ? (now - sched_last_switch_us) : 0;
    sched_last_switch_us = now;
    sched_switch_count++;

    const char *from_name = (from && from->name[0]) ? from->name : "?";
    const char *to_name   = (to   && to->name[0])   ? to->name   : "?";
    uint64_t from_id = from ? from->id : 0;
    uint64_t to_id   = to   ? to->id   : 0;

    serial_printf("[sched #%llu +%lluus dt=%lluus] %s: %s(%llu) -> %s(%llu) rip=0x%llx\n",
                  (unsigned long long)sched_switch_count,
                  (unsigned long long)now,
                  (unsigned long long)delta,
                  kind ? kind : "?",
                  from_name, (unsigned long long)from_id,
                  to_name,   (unsigned long long)to_id,
                  (unsigned long long)rip);
}

// Default stack size for threads
#define DEFAULT_STACK_SIZE (16 * 1024)  // 16KB

// Thread context structure for assembly context switching
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rsp, rbx, rdx, rcx, rax;
    uint64_t rip, rflags, cs, ss, ds, es, fs, gs;
} thread_context_t;

typedef struct thread_ctx_node {
    thread_t *thread;
    thread_context_t ctx;
    struct thread_ctx_node *next;
} thread_ctx_node_t;
static thread_ctx_node_t *thread_ctx_list = NULL;

// Assembly functions for context switching
extern void thread_switch_asm(thread_context_t *from, thread_context_t *to);
extern void thread_start_asm(void);

// Forward declarations for internal functions
static void _scheduler_remove_thread_unlocked(thread_t *thread);
static bool _queue_unlink(thread_t *thread, thread_t **head, thread_t **tail);
static bool _ready_unlink(thread_t *thread);
static void scheduler_kick_home_cpu(thread_t *thread);

/* Clamp a priority to a valid ready-queue index. */
static inline int _prio_index(thread_priority_t p) {
    int i = (int)p;
    if (i < 0) i = 0;
    if (i >= THREAD_NUM_PRIORITIES) i = THREAD_NUM_PRIORITIES - 1;
    return i;
}

// Initialize the scheduler
void scheduler_init(void) {
    memset(&global_scheduler, 0, sizeof(scheduler_t));
    global_scheduler.next_thread_id = 1;
    global_scheduler.scheduler_running = false;
    
    printf("Scheduler initialized\n");
}

// Create a new thread
thread_t* thread_create(const char *name, void (*entry)(void*), void *arg, 
                       uint64_t stack_size, thread_priority_t priority) {
    if (!entry) {
        printf("thread_create: NULL entry point\n");
        return NULL;
    }
    
    if (stack_size == 0) {
        stack_size = DEFAULT_STACK_SIZE;
    }
    
    // Allocate thread structure
    thread_t *thread = kalloc(sizeof(thread_t));
    if (!thread) {
        printf("thread_create: failed to allocate thread structure\n");
        return NULL;
    }
    
    // Allocate stack
    uint64_t stack_base = (uint64_t)kalloc(stack_size);
    if (!stack_base) {
        printf("thread_create: failed to allocate stack\n");
        kfree(thread);
        return NULL;
    }
    
    // Initialize thread structure
    memset(thread, 0, sizeof(thread_t));
    thread->id = global_scheduler.next_thread_id++;
    kstrncpy(thread->name, name, sizeof(thread->name));
    thread->entry_point = entry;
    thread->arg = arg;
    thread->stack_base = stack_base;
    thread->stack_size = stack_size;
    thread->priority = priority;
    thread->state = THREAD_READY;
    thread->home_cpu = 0xFFFFFFFFu;
    
    // Set up initial stack frame for thread_start_asm
    // Stack grows downward, so we start at the top
    uint64_t *stack = (uint64_t*)(stack_base + stack_size);
    
    // Align stack to 16 bytes (required by x86-64 ABI)
    stack = (uint64_t*)((uint64_t)stack & ~0xFULL);
    
    // The thread_start_asm function will pop:
    //   1. Entry function address
    //   2. Thread argument (passed in rdi)
    // Stack should be 16-byte aligned BEFORE the call instruction would push return address
    // So we need one extra slot for alignment
    
    *--stack = (uint64_t)arg;           // Argument for the entry function
    *--stack = (uint64_t)entry;         // Entry function address
    
    // Add alignment padding if needed (stack must be 16-byte aligned before RET)
    if (((uint64_t)stack & 0xF) != 0) {
        *--stack = 0;  // Padding
    }
    
    // Save the initial stack pointer
    thread->rsp = (uint64_t)stack;
    // The RIP points to thread_start_asm which will set up and call the entry function
    thread->rip = (uint64_t)thread_start_asm;
    
    // Initialize preemptive scheduler state
    memset(&thread->saved_regs, 0, sizeof(thread_regs_t));
    thread->saved_regs.rsp = thread->rsp;
    thread->saved_regs.rip = thread->rip;
    thread->saved_regs.cs  = 0x08;      // Kernel code segment
    thread->saved_regs.ss  = 0x10;      // Kernel data segment
    thread->saved_regs.rflags = 0x202;  // IF set (interrupts enabled)
    thread->saved_regs.rdi = (uint64_t)arg;  // First argument for entry point
    /* Synthetic iretq frame so the timer/IPI path can start this
     * kernel thread at thread_start_asm without a cooperative switch. */
    thread->has_saved_context = true;
    thread->on_cpu = 0;
    thread->kwait = 0;
    
    // Initialize FPU/SSE state with a clean copy
    fpu_get_default_state(thread->fxsave_area);
    
    printf("Created thread '%s' (ID: %llu) with stack at 0x%llx, rsp=0x%llx\n", 
           name, (unsigned long long)thread->id, 
           (unsigned long long)stack_base,
           (unsigned long long)thread->rsp);
    
    return thread;
}

/**
 * thread_exit() -- called when a thread's entry function returns.
 * This is the C-side cleanup that replaces the old cli/hlt in thread_start_asm.
 * It removes the thread from the scheduler, frees its resources, and yields
 * to the next runnable thread.  If there is no next thread, it halts.
 */
void thread_exit(void) {
    thread_t *cur = scheduler_get_current_thread();
    if (cur) {
        serial_printf("[thread_exit] Thread '%s' (ID %llu) exiting\n",
                      cur->name, (unsigned long long)cur->id);
        cur->state = THREAD_TERMINATED;

        // Remove from all queues (IRQs must be off while holding the lock)
        uint64_t flags = irq_save();
        spinlock_lock(&global_scheduler.scheduler_lock);
        _scheduler_remove_thread_unlocked(cur);
        global_scheduler.current_thread = NULL;
        per_cpu_t *cpu = smp_get_current_cpu();
        if (cpu && cpu->current_thread == cur)
            cpu->current_thread = cpu->idle_thread;
        spinlock_unlock(&global_scheduler.scheduler_lock);
        irq_restore(flags);

        // NOTE: We cannot free our own stack while we're still running on it.
        // The preemptive scheduler will detect THREAD_TERMINATED and reclaim
        // resources next time it runs.  For now, just yield by halting with
        // interrupts enabled so the timer fires and picks the next thread.
    }

    // Yield to the scheduler -- interrupts must be enabled so the timer can fire.
    __asm__ volatile("sti");
    for (;;) {
        __asm__ volatile("hlt");
    }
}

// Internal version that assumes lock is already held
static void _scheduler_remove_thread_unlocked(thread_t *thread) {
    if (!thread) return;

    if (!_ready_unlink(thread)) {
        if (!_queue_unlink(thread, &global_scheduler.blocked_queue, NULL))
            _queue_unlink(thread, &global_scheduler.sleeping_queue, NULL);
    }

    thread->next = NULL;
    thread->prev = NULL;
}

// Destroy a thread (interrupt-safe)
void thread_destroy(thread_t *thread) {
    if (!thread) return;
    
    uint64_t flags = irq_save();
    
    // Remove from all queues
    spinlock_lock(&global_scheduler.scheduler_lock);
    _scheduler_remove_thread_unlocked(thread);
    spinlock_unlock(&global_scheduler.scheduler_lock);

    // Remove saved switch context for this thread.
    thread_ctx_node_t *prev_ctx = NULL;
    thread_ctx_node_t *ctx = thread_ctx_list;
    while (ctx) {
        if (ctx->thread == thread) {
            if (prev_ctx) prev_ctx->next = ctx->next;
            else thread_ctx_list = ctx->next;
            kfree(ctx);
            break;
        }
        prev_ctx = ctx;
        ctx = ctx->next;
    }
    
    sched_oncpu_forget(thread);

    // Free stack
    if (thread->stack_base) {
        kfree((void*)thread->stack_base);
    }
    
    // Free thread structure
    kfree(thread);
    
    irq_restore(flags);
}

/*
 * Locking model: every public queue operation disables local IRQs (so the
 * timer ISR on this CPU can't re-enter the scheduler mid-update) AND takes
 * scheduler_lock (so other CPUs can't mutate the queues concurrently).
 * IMPORTANT: scheduler_lock must only ever be taken with IRQs disabled,
 * otherwise the timer ISR can spin on a lock its own CPU holds.
 */

static per_cpu_t *rq_cpu(uint32_t id) {
    if (id >= MAX_CPUS || !cpu_data[id].online)
        return &cpu_data[0];
    return &cpu_data[id];
}

static void rq_note_add(per_cpu_t *cpu) {
    cpu->ready_count++;
}

static void rq_note_del(per_cpu_t *cpu) {
    if (cpu->ready_count)
        cpu->ready_count--;
}

static uint32_t pick_least_loaded_cpu(void) {
    uint32_t best = 0;
    uint32_t best_load = ~0u;
    for (uint32_t c = 0; c < MAX_CPUS; c++) {
        if (!cpu_data[c].online) continue;
        uint32_t load = cpu_data[c].ready_count;
        thread_t *cur = cpu_data[c].current_thread;
        if (cur && cur != cpu_data[c].idle_thread)
            load++;
        if (load < best_load) {
            best_load = load;
            best = c;
        }
    }
    return best;
}

static void assign_home_cpu(thread_t *thread) {
    if (thread->home_cpu < MAX_CPUS && cpu_data[thread->home_cpu].online)
        return;
    thread->home_cpu = pick_least_loaded_cpu();
}

static per_cpu_t *thread_rq(thread_t *thread) {
    if (thread->home_cpu >= MAX_CPUS)
        assign_home_cpu(thread);
    return rq_cpu(thread->home_cpu);
}

// Internal: append to the tail of the thread's priority level.
// Caller holds scheduler_lock + IRQs off.
static void _scheduler_add_thread_unlocked(thread_t *thread) {
    int p = _prio_index(thread->priority);
    per_cpu_t *cpu = thread_rq(thread);
    thread->next = NULL;
    thread->prev = cpu->ready_queue_tail[p];
    if (cpu->ready_queue_tail[p]) {
        cpu->ready_queue_tail[p]->next = thread;
    } else {
        cpu->ready_queue[p] = thread;
    }
    cpu->ready_queue_tail[p] = thread;
    rq_note_add(cpu);
}

// Internal: push to the HEAD of the thread's priority level (wakeup boost).
// Caller holds scheduler_lock + IRQs off.
static void _scheduler_add_thread_front_unlocked(thread_t *thread) {
    int p = _prio_index(thread->priority);
    per_cpu_t *cpu = thread_rq(thread);
    thread->prev = NULL;
    thread->next = cpu->ready_queue[p];
    if (cpu->ready_queue[p]) {
        cpu->ready_queue[p]->prev = thread;
    } else {
        cpu->ready_queue_tail[p] = thread;
    }
    cpu->ready_queue[p] = thread;
    rq_note_add(cpu);
}

// Add thread to scheduler (IRQ + SMP safe)
void scheduler_add_thread(thread_t *thread) {
    if (!thread) return;

    uint64_t flags = irq_save();
    spinlock_lock(&global_scheduler.scheduler_lock);
    _scheduler_add_thread_unlocked(thread);
    spinlock_unlock(&global_scheduler.scheduler_lock);
    irq_restore(flags);
    scheduler_kick_home_cpu(thread);
}

/* Internal: unlink a thread from one specific queue, but only if it is
 * actually a member (verified by walking the list — queues are short).
 * Maintains the optional tail pointer. Caller holds the lock + IRQs off.
 * Returns true if the thread was found and removed. */
static bool _queue_unlink(thread_t *thread, thread_t **head, thread_t **tail) {
    if (!thread || !head) return false;

    for (thread_t *t = *head; t; t = t->next) {
        if (t != thread) continue;

        if (t->prev) t->prev->next = t->next;
        else         *head = t->next;
        if (t->next) t->next->prev = t->prev;
        else if (tail) *tail = t->prev;

        t->next = NULL;
        t->prev = NULL;
        return true;
    }
    return false;
}

// Internal helper to remove from a specific (untailed) queue. Lock held.
// Internal: remove from whichever ready level the thread is linked in.
static bool _ready_unlink(thread_t *thread) {
    per_cpu_t *cpu = thread_rq(thread);
    for (int p = 0; p < THREAD_NUM_PRIORITIES; p++) {
        if (_queue_unlink(thread, &cpu->ready_queue[p], &cpu->ready_queue_tail[p])) {
            rq_note_del(cpu);
            return true;
        }
    }
    /* Thread may have been stolen onto another CPU's list. */
    for (uint32_t c = 0; c < MAX_CPUS; c++) {
        if (!cpu_data[c].online) continue;
        for (int p = 0; p < THREAD_NUM_PRIORITIES; p++) {
            if (_queue_unlink(thread, &cpu_data[c].ready_queue[p],
                              &cpu_data[c].ready_queue_tail[p])) {
                rq_note_del(&cpu_data[c]);
                return true;
            }
        }
    }
    return false;
}

// Remove thread from scheduler (IRQ + SMP safe)
void scheduler_remove_thread(thread_t *thread) {
    if (!thread) return;

    uint64_t flags = irq_save();
    spinlock_lock(&global_scheduler.scheduler_lock);
    _scheduler_remove_thread_unlocked(thread);
    spinlock_unlock(&global_scheduler.scheduler_lock);
    irq_restore(flags);
}

/* Remove a thread from all queues and clear current_thread if it points at
 * it — one atomic operation for thread-teardown paths (exec_spawned_exit,
 * signal kill). */
void scheduler_detach_thread(thread_t *thread) {
    if (!thread) return;

    uint64_t flags = irq_save();
    spinlock_lock(&global_scheduler.scheduler_lock);
    _scheduler_remove_thread_unlocked(thread);
    if (global_scheduler.current_thread == thread)
        global_scheduler.current_thread = NULL;
    for (uint32_t c = 0; c < MAX_CPUS; c++) {
        if (cpu_data[c].current_thread == thread)
            cpu_data[c].current_thread = cpu_data[c].idle_thread;
    }
    spinlock_unlock(&global_scheduler.scheduler_lock);
    irq_restore(flags);
}

/* Internal: pop the head of the highest non-empty priority level.
 * The thread is FULLY dequeued — the caller owns it from here on and
 * re-queues it when (if) it gets preempted. A RUNNING thread must never
 * stay linked in a ready queue: the old "rotate to tail" scheme left the
 * dispatched thread linked, and the next preemption re-inserted it,
 * corrupting the list and silently losing threads. Caller holds the lock. */
static thread_t *_rq_pop_any(per_cpu_t *cpu) {
    for (int p = THREAD_NUM_PRIORITIES - 1; p >= 0; p--) {
        for (thread_t *thread = cpu->ready_queue[p]; thread; thread = thread->next) {
            /* Never dequeue a thread whose context is still live on a CPU. */
            if (__atomic_load_n(&thread->on_cpu, __ATOMIC_ACQUIRE))
                continue;
            _queue_unlink(thread, &cpu->ready_queue[p], &cpu->ready_queue_tail[p]);
            rq_note_del(cpu);
            return thread;
        }
    }
    return NULL;
}

static thread_t* _scheduler_get_next_thread_unlocked(void) {
    per_cpu_t *me = smp_get_current_cpu();
    thread_t *thread = _rq_pop_any(me ? me : &cpu_data[0]);
    if (thread)
        return thread;
    for (uint32_t c = 0; c < MAX_CPUS; c++) {
        if (!cpu_data[c].online) continue;
        if (me && c == me->cpu_id) continue;
        thread = _rq_pop_any(&cpu_data[c]);
        if (thread)
            return thread;
    }
    return NULL;
}

// Get next ready thread (IRQ + SMP safe)
thread_t* scheduler_get_next_thread(void) {
    uint64_t flags = irq_save();
    spinlock_lock(&global_scheduler.scheduler_lock);
    thread_t *thread = _scheduler_get_next_thread_unlocked();
    spinlock_unlock(&global_scheduler.scheduler_lock);
    irq_restore(flags);
    return thread;
}

/* Highest-priority READY *kernel* thread (owner_process == NULL).
 *
 * The cooperative switch path must never dispatch a Ring-3 thread: such a
 * thread's real entry point lives in saved_regs (an iretq frame) and is only
 * reachable when the timer ISR restores that frame.  A cooperative switch
 * starts it at thread->rip == thread_start_asm instead, whose entry point is
 * thread_exit — so the thread "ran" for microseconds and vanished without ever
 * entering user mode.  (exec_spawn queues a Ring-3 thread and both the kmain
 * loop and the timer could pick it up; whichever won the race decided whether
 * a spawned app ran at all.) */
static thread_t* _scheduler_get_next_kernel_thread_unlocked(void) {
    for (uint32_t c = 0; c < MAX_CPUS; c++) {
        if (c != 0 && !cpu_data[c].online) continue;
        for (int p = THREAD_NUM_PRIORITIES - 1; p >= 0; p--) {
            for (thread_t *t = cpu_data[c].ready_queue[p]; t; t = t->next) {
                if (t->owner_process)
                    continue;
                if (__atomic_load_n(&t->on_cpu, __ATOMIC_ACQUIRE))
                    continue;
                _queue_unlink(t, &cpu_data[c].ready_queue[p],
                              &cpu_data[c].ready_queue_tail[p]);
                rq_note_del(&cpu_data[c]);
                return t;
            }
        }
    }
    return NULL;
}

static thread_t* scheduler_get_next_kernel_thread(void) {
    uint64_t flags = irq_save();
    spinlock_lock(&global_scheduler.scheduler_lock);
    thread_t *thread = _scheduler_get_next_kernel_thread_unlocked();
    spinlock_unlock(&global_scheduler.scheduler_lock);
    irq_restore(flags);
    return thread;
}

// Lock-free fast-path check used by the timer ISR before doing any work.
bool scheduler_has_ready_threads(void) {
    for (uint32_t c = 0; c < MAX_CPUS; c++) {
        if (c != 0 && !cpu_data[c].online) continue;
        for (int p = 0; p < THREAD_NUM_PRIORITIES; p++) {
            if (cpu_data[c].ready_queue[p])
                return true;
        }
    }
    return false;
}

bool scheduler_has_ready_user_threads(void) {
    for (uint32_t c = 0; c < MAX_CPUS; c++) {
        if (c != 0 && !cpu_data[c].online) continue;
        for (int p = 0; p < THREAD_NUM_PRIORITIES; p++) {
            for (thread_t *t = cpu_data[c].ready_queue[p]; t; t = t->next) {
                if (t->owner_process)
                    return true;
            }
        }
    }
    return false;
}

/* Wake every sleeping thread whose deadline has passed.
 * The sleeping queue is sorted by sleep_until (earliest first) so the
 * sweep stops at the first unexpired entry — one short critical section
 * per timer tick instead of a full-queue walk with per-node locking.
 * Woken threads go to the FRONT of their priority level: a thread that
 * slept until a deadline (frame pacing, input poll, timeout) is exactly
 * the kind of latency-sensitive work that should not sit behind
 * CPU-bound threads at the tail. */
void scheduler_wake_expired(uint64_t now) {
    uint32_t woke_mask = 0;
    uint64_t flags = irq_save();
    spinlock_lock(&global_scheduler.scheduler_lock);
    while (global_scheduler.sleeping_queue) {
        thread_t *thread = global_scheduler.sleeping_queue;
        if (now < thread->sleep_until)
            break;
        global_scheduler.sleeping_queue = thread->next;
        if (thread->next)
            thread->next->prev = NULL;
        thread->next = NULL;
        thread->prev = NULL;
        thread->state = THREAD_READY;
        _scheduler_add_thread_front_unlocked(thread);
        woke_mask |= (thread->home_cpu < 32) ? (1u << thread->home_cpu) : 0;
    }
    spinlock_unlock(&global_scheduler.scheduler_lock);
    irq_restore(flags);

    per_cpu_t *me = smp_get_current_cpu();
    uint32_t my_id = me ? me->cpu_id : 0;
    for (uint32_t c = 0; c < 32 && c < MAX_CPUS; c++) {
        if (c == my_id) continue;
        if ((woke_mask & (1u << c)) && cpu_data[c].online)
            smp_reschedule_cpu(c);
    }
}

static void scheduler_kick_home_cpu(thread_t *thread) {
    if (!thread || thread->home_cpu >= MAX_CPUS) return;
    per_cpu_t *me = smp_get_current_cpu();
    uint32_t my_id = me ? me->cpu_id : 0;
    if (thread->home_cpu == my_id) return;
    if (!cpu_data[thread->home_cpu].online) return;
    smp_reschedule_cpu(thread->home_cpu);
}

/* Dequeue the highest-priority READY user-mode thread (one with an owner
 * process). Kernel threads are skipped IN PLACE. */
static thread_t *_pick_from_cpu(per_cpu_t *cpu, bool want_user, bool kernel_ok,
                                 bool allow_kmain) {
    if (!cpu) return NULL;
    thread_t *idle = cpu->idle_thread;
    thread_t *kmain = smp_get_bsp_boot_thread();
    for (int p = THREAD_NUM_PRIORITIES - 1; p >= 0; p--) {
        for (thread_t *t = cpu->ready_queue[p]; t; t = t->next) {
            if (t == idle) continue;
            if (__atomic_load_n(&t->on_cpu, __ATOMIC_ACQUIRE)) continue;
            if (t == kmain && !allow_kmain) continue;
            if (want_user) {
                if (!t->owner_process) continue;
            } else if (!kernel_ok) {
                continue;
            } else {
                if (t->owner_process) continue;
                if (!t->has_saved_context) continue;
            }
            _queue_unlink(t, &cpu->ready_queue[p], &cpu->ready_queue_tail[p]);
            rq_note_del(cpu);
            return t;
        }
    }
    return NULL;
}

thread_t* scheduler_pick_user_thread(void) {
    uint64_t flags = irq_save();
    spinlock_lock(&global_scheduler.scheduler_lock);

    per_cpu_t *me = smp_get_current_cpu();
    thread_t *picked = _pick_from_cpu(me ? me : &cpu_data[0], true, false, false);
    if (!picked) {
        uint32_t my_id = me ? me->cpu_id : 0;
        for (uint32_t c = 0; c < MAX_CPUS && !picked; c++) {
            if (c == my_id || !cpu_data[c].online) continue;
            picked = _pick_from_cpu(&cpu_data[c], true, false, false);
        }
    }

    spinlock_unlock(&global_scheduler.scheduler_lock);
    irq_restore(flags);
    return picked;
}

thread_t* scheduler_pick_preempt_thread(void) {
    uint64_t flags = irq_save();
    spinlock_lock(&global_scheduler.scheduler_lock);

    per_cpu_t *me = smp_get_current_cpu();
    per_cpu_t *local = me ? me : &cpu_data[0];
    uint32_t my_id = local->cpu_id;

    thread_t *picked = _pick_from_cpu(local, true, false, false);
    if (!picked)
        picked = _pick_from_cpu(local, false, true, my_id == 0);
    if (!picked) {
        for (uint32_t c = 0; c < MAX_CPUS && !picked; c++) {
            if (c == my_id || !cpu_data[c].online) continue;
            picked = _pick_from_cpu(&cpu_data[c], true, false, false);
        }
    }
    if (!picked) {
        for (uint32_t c = 0; c < MAX_CPUS && !picked; c++) {
            if (c == my_id || !cpu_data[c].online) continue;
            picked = _pick_from_cpu(&cpu_data[c], false, true, false);
        }
    }
    if (picked)
        picked->home_cpu = my_id;

    spinlock_unlock(&global_scheduler.scheduler_lock);
    irq_restore(flags);
    return picked;
}

/* ---------------- on_cpu handoff ---------------- */
static thread_t *volatile oncpu_pending[MAX_CPUS];

static inline uint32_t oncpu_cpu_idx(void) {
    per_cpu_t *me = smp_get_current_cpu();
    uint32_t i = me ? me->cpu_id : 0;
    return i < MAX_CPUS ? i : 0;
}

void sched_oncpu_set(thread_t *t) {
    if (t)
        __atomic_store_n(&t->on_cpu, 1, __ATOMIC_SEQ_CST);
}

/* Clear the previously switched-out thread of THIS CPU. Only call while
 * executing on a different thread's stack than the pending one. */
void sched_oncpu_flush(void) {
    uint64_t f = irq_save();
    uint32_t i = oncpu_cpu_idx();
    thread_t *t = oncpu_pending[i];
    if (t) {
        oncpu_pending[i] = NULL;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        __atomic_store_n(&t->on_cpu, 0, __ATOMIC_RELEASE);
    }
    irq_restore(f);
}

void sched_oncpu_defer_clear(thread_t *t) {
    uint64_t f = irq_save();
    uint32_t i = oncpu_cpu_idx();
    thread_t *old = oncpu_pending[i];
    if (old && old != t) {
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        __atomic_store_n(&old->on_cpu, 0, __ATOMIC_RELEASE);
    }
    oncpu_pending[i] = t;
    irq_restore(f);
}

/* Thread memory is about to be freed: drop any deferred clear for it. */
void sched_oncpu_forget(thread_t *t) {
    if (!t) return;
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        thread_t *exp = t;
        __atomic_compare_exchange_n((thread_t **)&oncpu_pending[i], &exp, (thread_t *)NULL,
                                    false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }
}

/* Park a thread blocked in-kernel IN PLACE (no cooperative switch). User
 * threads' coop context is never resumed by the preempt path (it restores
 * saved_regs), so a coop switch resumed them at a stale ring-3 frame
 * (nest_fork replayed fork). The timer may preempt us here with a valid
 * ring-0 frame (kwait != 0). Wake: BLOCKED/SLEEPING -> READY. */
static void thread_kwait_current(thread_t *cur, int kind) {
    uint64_t rf;
    __asm__ volatile("pushfq; pop %0" : "=r"(rf) :: "memory");
    __atomic_store_n(&cur->kwait, kind, __ATOMIC_SEQ_CST);
    for (;;) {
        int s = (int)__atomic_load_n(&cur->state, __ATOMIC_ACQUIRE);
        if (s != (int)THREAD_BLOCKED && s != (int)THREAD_SLEEPING)
            break;
        __asm__ volatile("sti; hlt" ::: "memory");
    }
    __atomic_store_n(&cur->kwait, 0, __ATOMIC_SEQ_CST);
    uint64_t f = irq_save();
    spinlock_lock(&global_scheduler.scheduler_lock);
    if (cur->state == THREAD_READY || cur->state == THREAD_RUNNING) {
        _scheduler_remove_thread_unlocked(cur);
        cur->state = THREAD_RUNNING;
    }
    spinlock_unlock(&global_scheduler.scheduler_lock);
    irq_restore(f);
    if (!(rf & 0x200))
        __asm__ volatile("cli" ::: "memory");
}

// Yield current thread
void thread_yield(void) {
    if (!global_scheduler.scheduler_running) {
        return;
    }
    
    thread_t *current = scheduler_get_current_thread();
    /* User threads never cooperatively switch (their coop context is not
     * what the preempt path resumes). Blocked/sleeping: park in place.
     * Plain RUNNING yield from a user thread in-kernel: just relax. */
    if (current && current->owner_process) {
        if (current->state != THREAD_RUNNING)
            thread_kwait_current(current, 1);
        else
            __asm__ volatile("pause" ::: "memory");
        return;
    }
    /* BSP boot thread running ash, blocked in sys_wait: same. */
    if (current && current->state == THREAD_BLOCKED &&
        current == smp_get_bsp_boot_thread()) {
        thread_kwait_current(current, 2);
        return;
    }
    /* Kernel threads only — see scheduler_get_next_kernel_thread(). */
    thread_t *next = scheduler_get_next_kernel_thread();

    /* If the ready queue is empty but the caller wants to sleep / block,
     * we cannot just keep them RUNNING (the old behaviour) — the caller
     * has already inserted itself into sleeping_queue / blocked_queue and
     * is relying on us actually switching off-CPU so the timer ISR can
     * later wake them up. Falling back to staying RUNNING turned
     * thread_sleep() into a no-op whenever no other thread happened to
     * be ready, which made desktop_thread_main spin and triggered the
     * "[desktop] WARNING: HPET ticks not advancing" path.
     *
     * Use the per-CPU idle thread as the always-available fallback,
     * matching the same pattern preemptive_schedule() already uses.
     */
    if (!next) {
        per_cpu_t *cpu = smp_get_current_cpu();
        if (cpu && cpu->idle_thread && current != cpu->idle_thread) {
            next = cpu->idle_thread;
        } else {
            /* No idle thread either (very early boot) and no other thread:
             * keep current runnable rather than leaving the CPU stranded. */
            if (current && current->state != THREAD_RUNNING) {
                scheduler_remove_thread(current);
                current->state = THREAD_RUNNING;
                global_scheduler.current_thread = current;
                if (cpu)
                    cpu->current_thread = current;
            }
            return;
        }
    }

    if (current && current->state == THREAD_RUNNING) {
        current->state = THREAD_READY;
        scheduler_add_thread(current);
    }
    
    // Switch to next thread
    thread_switch(current, next);
}

// Put current thread to sleep
void thread_sleep(uint64_t milliseconds) {
    if (!global_scheduler.scheduler_running) {
        return;
    }
    
    thread_t *current = scheduler_get_current_thread();
    if (!current) {
        return;
    }

    /* sleep_until is compared against hpet_get_ticks() in
     * preemptive_schedule() and scheduler_tick(), and hpet_get_ticks()
     * already returns milliseconds (it divides the raw HPET counter by
     * hpet_freq / 1000). The previous calculation added
     *     milliseconds * hpet_get_frequency() / 1000
     * which is in raw HPET counter ticks (100,000 per ms on QEMU), so
     * sleep_until ended up ~100,000× larger than now and threads never
     * woke up — thread_sleep(16) effectively meant "sleep ~26 minutes".
     * That broke the desktop compositor's frame pacing. */
    uint64_t wakeup_time = hpet_get_ticks() + milliseconds;

    /* Dequeue + state change + sorted sleeping-queue insert must be one
     * atomic step, or the timer ISR can observe (and corrupt) the
     * half-linked thread. */
    uint64_t flags = irq_save();
    spinlock_lock(&global_scheduler.scheduler_lock);

    _scheduler_remove_thread_unlocked(current);

    current->state = THREAD_SLEEPING;
    current->sleep_until = wakeup_time;
    current->prev = NULL;

    /*
     * Insert into sleeping queue sorted by sleep_until (earliest first).
     * scheduler_tick can then short-circuit once it hits a thread whose
     * wakeup time is still in the future.
     */
    thread_t **pp = &global_scheduler.sleeping_queue;
    thread_t *prev_node = NULL;
    while (*pp && (*pp)->sleep_until <= wakeup_time) {
        prev_node = *pp;
        pp = &(*pp)->next;
    }
    current->next = *pp;
    if (*pp)
        (*pp)->prev = current;
    *pp = current;
    current->prev = prev_node;

    spinlock_unlock(&global_scheduler.scheduler_lock);
    irq_restore(flags);

    // Switch to next thread
    thread_yield();
}

// Wake up a thread (front-of-queue: explicit wakeups are latency-sensitive)
void thread_wake(thread_t *thread) {
    /* No-op-safe: only SLEEPING (pipe/timed) or BLOCKED (wait/hold) -> READY.
     * Never touch READY/RUNNING (avoids double-enqueue / runqueue corruption). */
    if (!thread ||
        (thread->state != THREAD_SLEEPING && thread->state != THREAD_BLOCKED)) {
        return;
    }

    uint64_t flags = irq_save();
    spinlock_lock(&global_scheduler.scheduler_lock);
    if (thread->state != THREAD_SLEEPING && thread->state != THREAD_BLOCKED) {
        spinlock_unlock(&global_scheduler.scheduler_lock);
        irq_restore(flags);
        return;
    }
    _scheduler_remove_thread_unlocked(thread);
    thread->state = THREAD_READY;
    _scheduler_add_thread_front_unlocked(thread);
    spinlock_unlock(&global_scheduler.scheduler_lock);
    irq_restore(flags);
    scheduler_kick_home_cpu(thread);
}

void scheduler_irq_lock(uint64_t *flags) {
    *flags = irq_save();
    spinlock_lock(&global_scheduler.scheduler_lock);
}

void scheduler_irq_unlock(uint64_t flags) {
    spinlock_unlock(&global_scheduler.scheduler_lock);
    irq_restore(flags);
}

void thread_block_current_unlocked(void) {
    thread_t *cur = scheduler_get_current_thread();
    if (!cur)
        return;
    _scheduler_remove_thread_unlocked(cur);
    cur->state = THREAD_BLOCKED;
}

void thread_unblock_current_running_unlocked(void) {
    thread_t *cur = scheduler_get_current_thread();
    if (!cur)
        return;
    _scheduler_remove_thread_unlocked(cur);
    cur->state = THREAD_RUNNING;
}

/* Caller holds scheduler_lock. Wait wake: BLOCKED (or SLEEPING) -> READY. */
void thread_wake_blocked_unlocked(thread_t *thread) {
    if (!thread)
        return;
    if (thread->state == THREAD_SLEEPING)
        thread->sleep_until = 0;
    if (thread->state != THREAD_SLEEPING && thread->state != THREAD_BLOCKED)
        return;
    _scheduler_remove_thread_unlocked(thread);
    thread->state = THREAD_READY;
    _scheduler_add_thread_front_unlocked(thread);
}

void scheduler_kick_thread(thread_t *thread) {
    scheduler_kick_home_cpu(thread);
}

/* Change a thread's priority; if it is READY, move it to the tail of its
 * new priority level so the change takes effect immediately. */
void thread_set_priority(thread_t *thread, thread_priority_t priority) {
    if (!thread) return;

    uint64_t flags = irq_save();
    spinlock_lock(&global_scheduler.scheduler_lock);
    bool was_ready = _ready_unlink(thread);
    thread->priority = priority;
    if (was_ready)
        _scheduler_add_thread_unlocked(thread);
    spinlock_unlock(&global_scheduler.scheduler_lock);
    irq_restore(flags);
}

// Get current thread
thread_t* scheduler_get_current_thread(void) {
    per_cpu_t *cpu = smp_get_current_cpu();
    if (cpu && cpu->current_thread)
        return cpu->current_thread;
    return global_scheduler.current_thread;
}

// Start the scheduler
void scheduler_start(void) {
    global_scheduler.scheduler_running = true;
    printf("Scheduler started\n");
}

// Stop the scheduler
void scheduler_stop(void) {
    global_scheduler.scheduler_running = false;
    printf("Scheduler stopped\n");
}

// Utility functions
uint64_t thread_get_id(void) {
    thread_t *current = scheduler_get_current_thread();
    return current ? current->id : 0;
}

const char* thread_get_name(void) {
    thread_t *current = scheduler_get_current_thread();
    return current ? current->name : "unknown";
}

thread_state_t thread_get_state(thread_t *thread) {
    return thread ? thread->state : THREAD_TERMINATED;
}

// Static context storage for context switching
// Each thread has its own context embedded in thread_t, but we need
// a place to store the "from" context during switch
static thread_context_t kernel_context;  // For saving kernel/main thread context

static thread_context_t *thread_get_context(thread_t *thread, bool initialize) {
    if (!thread) return NULL;

    thread_ctx_node_t *n = thread_ctx_list;
    while (n) {
        if (n->thread == thread) return &n->ctx;
        n = n->next;
    }

    n = (thread_ctx_node_t*)kalloc(sizeof(thread_ctx_node_t));
    if (!n) return NULL;
    memset(n, 0, sizeof(thread_ctx_node_t));
    n->thread = thread;
    if (initialize) {
        n->ctx.rsp = thread->rsp;
        n->ctx.rip = thread->rip;
        n->ctx.rflags = 0x202;
    }
    n->next = thread_ctx_list;
    thread_ctx_list = n;
    return &n->ctx;
}

// Context switching functions (implemented in assembly)
void thread_switch(thread_t *from, thread_t *to) {
    if (!to) return;

    per_cpu_t *cpu = smp_get_current_cpu();
    if (cpu)
        cpu->current_thread = to;
    global_scheduler.current_thread = to;
    to->state = THREAD_RUNNING;

    // Set up context pointers
    thread_context_t *from_ctx = from ? thread_get_context(from, true) : &kernel_context;
    thread_context_t *to_ctx = thread_get_context(to, true);
    if (!to_ctx) return;

    scheduler_log_switch(from, to, "coop",
                         to->has_saved_context ? to->saved_regs.rip : to->rip);

    /* Per-thread FS base (user TLS) */
    thread_switch_fs(from, to);

    /* on_cpu: mark incoming live; outgoing is cleared only after this CPU
     * has left its stack (next flush on this CPU). */
    sched_oncpu_set(to);
    if (from && from != to)
        sched_oncpu_defer_clear(from);

    // Perform the context switch
    // from_ctx can be NULL for the first switch (nothing to save)
    thread_switch_asm(from_ctx, to_ctx);
    /* Resumed on our own stack: the thread that switched to us is off-CPU. */
    sched_oncpu_flush();
    
    // When we return here, we've been switched back to this thread
}

// These are no longer used but kept for compatibility
void thread_save_context(thread_t *thread) {
    (void)thread;
}

void thread_restore_context(thread_t *thread) {
    (void)thread;
} 