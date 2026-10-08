/**
 * @file smp.c
 * @brief Symmetric Multi-Processing (SMP) implementation
 *
 * Manages secondary CPU initialization and inter-processor communication.
 * Uses the Limine bootloader's SMP protocol to bring up application processors.
 */

#include "smp.h"
#include "cpu.h"
#include "apic.h"
#include "gdt.h"
#include "idt.h"
#include "fpu.h"
#include "syscall.h"
#include "mem.h"
#include "serial.h"
#include "utils.h"
#include "hpet.h"
#include "limine.h"
#include "string.h"

/* Forward declaration for snprintf (implemented in term.c) */
extern int snprintf(char *str, size_t size, const char *fmt, ...);

/* ============================================================================
 * Global SMP state
 * ============================================================================ */

_Static_assert(MAX_CPUS <= PREEMPT_MAX_CPUS, "preempt_count_cpu too small");

per_cpu_t cpu_data[MAX_CPUS];
volatile uint64_t cpus_online = 0;
uint64_t total_cpus = 0;

/* LAPIC id → cpu_data[] index. Unmapped ids read as 0 (BSP fallback). */
static uint8_t smp_lapic_to_cpu[256];

/** @brief Lock for SMP initialization synchronization */
static spinlock_t smp_init_lock = {0};

/** @brief Flag indicating SMP is fully initialized */
static bool smp_initialized = false;

/* The BSP enters the scheduler while it is already running kmain() on the
 * boot stack. Model that live execution context as a thread so preemption
 * saves/restores kmain separately from the synthetic idle thread. */
static thread_t bsp_boot_thread;
static bool bsp_boot_thread_initialized = false;

/* External Limine SMP request from cpu.c */
extern volatile struct limine_smp_request smp_request;
extern uint32_t bsp_lapic_id;

/* AP redirect mechanism from cpu.c - APs spin-wait on _ap_release,
 * then jump to the function pointer stored in _ap_redirect. */
extern void (* volatile _ap_redirect)(struct limine_smp_info *);
extern volatile int _ap_release;

/* LAPIC base address from apic.c */
extern uint32_t *lapic_addr;
extern uint64_t vmm_higher_half_offset;

/* LAPIC register offsets (from apic.c) */
#define LAPIC_ID_REG        0x20
#define LAPIC_SIVR_REG      0xF0
#define LAPIC_EOI_REG       0xB0
#define LAPIC_ICR_LOW       0x300
#define LAPIC_ICR_HIGH      0x310
#define LAPIC_TIMERDIV_REG  0x3E0
#define LAPIC_LVTTIMER_REG  0x320
#define LAPIC_INITCNT_REG   0x380

/* ============================================================================
 * Per-CPU LAPIC ID reading
 * ============================================================================ */

uint32_t smp_get_lapic_id(void) {
    if (!lapic_addr) return 0;
    uint32_t id = apic_read((void*)lapic_addr, LAPIC_ID_REG);
    return (id >> 24) & 0xFF;
}

uint32_t smp_cpu_index_fast(void) {
    if (!smp_initialized || !lapic_addr)
        return 0;
    uint32_t id = smp_get_lapic_id();
    if (id > 255) return 0;
    uint8_t idx = smp_lapic_to_cpu[id];
    return (idx < MAX_CPUS) ? idx : 0;
}

/* ============================================================================
 * AP (Application Processor) entry point
 * ============================================================================ */

/**
 * @brief Idle thread entry point
 *
 * This runs as a proper thread on each CPU. When the scheduler has no
 * other threads to run on this CPU, it falls back to this idle thread
 * which halts the CPU until an interrupt (timer, IPI) arrives.
 */
static void idle_thread_func(void *arg) {
    (void)arg;
    while (1) {
        __asm__ volatile("sti; hlt");
    }
}

/**
 * @brief Idle loop for each CPU (used during early AP init before scheduler)
 *
 * When a CPU has no work to do, it enters this loop which uses HLT
 * to sleep until the next interrupt (timer, IPI, etc.).
 */
static void cpu_idle_loop(void) {
    while (1) {
        /* Enable interrupts and halt until an interrupt arrives.
         * sti + hlt is atomic on x86 - the CPU won't miss an interrupt
         * between enabling interrupts and halting. */
        __asm__ volatile("sti; hlt");
    }
}

static void smp_attach_bsp_boot_thread(void) {
    if (bsp_boot_thread_initialized || !cpu_data[0].online)
        return;

    memset(&bsp_boot_thread, 0, sizeof(bsp_boot_thread));
    if (global_scheduler.next_thread_id == 0)
        global_scheduler.next_thread_id = 1;

    bsp_boot_thread.id = global_scheduler.next_thread_id++;
    kstrncpy(bsp_boot_thread.name, "kernel-main", sizeof(bsp_boot_thread.name));
    bsp_boot_thread.priority = PRIORITY_CRITICAL;
    bsp_boot_thread.state = THREAD_RUNNING;
    bsp_boot_thread.saved_regs.cs = 0x08;
    bsp_boot_thread.saved_regs.ss = 0x10;
    bsp_boot_thread.saved_regs.rflags = 0x202;

    cpu_data[0].current_thread = &bsp_boot_thread;
    global_scheduler.current_thread = &bsp_boot_thread;
    bsp_boot_thread_initialized = true;

    serial_printf("smp: attached BSP boot context as thread '%s' (tid %llu)\n",
                  bsp_boot_thread.name, (unsigned long long)bsp_boot_thread.id);
}

thread_t *smp_get_bsp_boot_thread(void) {
    return bsp_boot_thread_initialized ? &bsp_boot_thread : NULL;
}

/**
 * @brief Application Processor entry point
 *
 * Called by the Limine bootloader when an AP wakes up.
 * Each AP initializes its own hardware state and enters the idle loop.
 */
static void ap_entry(struct limine_smp_info *smp_info) {
    uint32_t processor_id = smp_info->processor_id;
    uint32_t lapic_id = smp_info->lapic_id;

    /* Find or assign a CPU slot */
    uint32_t cpu_idx = 0;
    spinlock_lock(&smp_init_lock);
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        if (!cpu_data[i].online && !cpu_data[i].is_bsp) {
            cpu_idx = i;
            break;
        }
    }

    per_cpu_t *cpu = &cpu_data[cpu_idx];
    cpu->cpu_id = cpu_idx;
    cpu->lapic_id = lapic_id;
    cpu->is_bsp = false;
    smp_lapic_to_cpu[lapic_id & 0xFF] = (uint8_t)cpu_idx;

    serial_printf("smp: AP %d (LAPIC %d) starting initialization\n",
                  processor_id, lapic_id);

    cpu_enable_protections();
    gdt_init_ap(cpu_idx);
    idt_load_current();
    fpu_sse_init();
    syscall_enable_cpu(cpu_idx);

    /* Enable the local APIC on this CPU */
    if (lapic_addr) {
        uint32_t sivr = apic_read((void*)lapic_addr, LAPIC_SIVR_REG);
        apic_write((void*)lapic_addr, LAPIC_SIVR_REG, sivr | 0x100);
    }
    apic_enable_timer();

    /* Mark this CPU as online */
    cpu->online = true;
    cpus_online++;

    serial_printf("smp: AP %d online (total online: %llu)\n",
                  cpu_idx, (unsigned long long)cpus_online);

    spinlock_unlock(&smp_init_lock);

    for (int i = 0; i < 10000000 && !cpu->idle_thread; i++)
        __asm__ volatile("pause");
    if (cpu->idle_thread)
        cpu->current_thread = cpu->idle_thread;

    /* Enter the idle loop - the CPU will be woken by timer/IPI interrupts */
    cpu_idle_loop();
}

/* ============================================================================
 * Initialization
 * ============================================================================ */

void smp_init(void) {
    serial_puts("smp: initializing SMP subsystem\n");

    memset(cpu_data, 0, sizeof(cpu_data));
    memset(smp_lapic_to_cpu, 0, sizeof(smp_lapic_to_cpu));
    cpus_online = 0;
    total_cpus = 0;

    /* Set up BSP (CPU 0) */
    per_cpu_t *bsp = &cpu_data[0];
    bsp->cpu_id = 0;
    bsp->lapic_id = bsp_lapic_id;
    bsp->is_bsp = true;
    bsp->online = true;
    smp_lapic_to_cpu[bsp_lapic_id & 0xFF] = 0;
    bsp->current_thread = NULL;
    bsp->idle_thread = NULL;
    bsp->run_lock = (spinlock_t){0};

    cpus_online = 1;
    total_cpus = cpu_count;

    if (total_cpus > MAX_CPUS) {
        serial_printf("smp: warning - %llu CPUs detected, capping at %d\n",
                      (unsigned long long)total_cpus, MAX_CPUS);
        total_cpus = MAX_CPUS;
    }

    smp_initialized = true;

    printf("SMP: BSP initialized (LAPIC ID %d, %llu CPUs detected)\n",
           bsp_lapic_id, (unsigned long long)total_cpus);
    serial_printf("smp: BSP initialized, %llu CPUs total\n",
                  (unsigned long long)total_cpus);
}

void smp_start_aps(void) {
    if (!smp_initialized) {
        serial_puts("smp: cannot start APs - SMP not initialized\n");
        return;
    }

    if (smp_request.response == NULL) {
        serial_puts("smp: no Limine SMP response - single CPU mode\n");
        printf("SMP: Single CPU mode (no SMP response from bootloader)\n");
        return;
    }

    if (total_cpus <= 1) {
        serial_puts("smp: only one CPU detected, no APs to start\n");
        printf("SMP: Single CPU system\n");
        return;
    }

    serial_printf("smp: starting %llu application processors\n",
                  (unsigned long long)(total_cpus - 1));
    printf("SMP: Starting %llu application processors...\n",
           (unsigned long long)(total_cpus - 1));

    /* The APs were already woken by cpu_init() -> _cpu_awake() and are
     * spin-waiting on _ap_release.  We just need to:
     *   1. Pre-allocate per-AP stacks (for bookkeeping / future use)
     *   2. Set the redirect function pointer
     *   3. Release them all at once                                      */

    /* Pre-allocate stacks for each AP (bookkeeping) */
    uint32_t slot = 1;  /* slot 0 is BSP */
    for (uint64_t i = 0; i < smp_request.response->cpu_count && i < MAX_CPUS; i++) {
        struct limine_smp_info *info = smp_request.response->cpus[i];
        if (info->lapic_id == bsp_lapic_id) continue;

        uint8_t *stack = (uint8_t *)kalloc(AP_STACK_SIZE);
        if (!stack) {
            serial_printf("smp: failed to allocate stack for AP %llu\n",
                          (unsigned long long)i);
        }
        if (slot < MAX_CPUS) {
            cpu_data[slot].stack_base = stack;
            cpu_data[slot].stack_top = stack
                ? (uint64_t)(stack + AP_STACK_SIZE) : 0;
        }
        slot++;
    }

    /* Set the redirect target and release all spinning APs.
     * _ap_redirect must be visible before _ap_release is set. */
    _ap_redirect = ap_entry;
    __atomic_store_n(&_ap_release, 1, __ATOMIC_SEQ_CST);

    /* Wait for all APs to come online (with timeout) */
    uint32_t timeout = 5000;  /* 5 second timeout */
    while (cpus_online < total_cpus && timeout > 0) {
        hpet_sleep_counter(1);
        timeout--;
    }

    if (cpus_online < total_cpus) {
        serial_printf("smp: warning - only %llu of %llu CPUs came online\n",
                      (unsigned long long)cpus_online,
                      (unsigned long long)total_cpus);
        printf("SMP: Warning - %llu of %llu CPUs online (timeout)\n",
               (unsigned long long)cpus_online,
               (unsigned long long)total_cpus);
    } else {
        printf("SMP: %llu CPUs online (per-CPU run queues)\n",
               (unsigned long long)cpus_online);
    }

    serial_printf("smp: AP startup complete, %llu CPUs online\n",
                  (unsigned long long)cpus_online);
}

/* ============================================================================
 * Per-CPU access
 * ============================================================================ */

per_cpu_t* smp_get_current_cpu(void) {
    if (!smp_initialized) return &cpu_data[0];
    uint32_t idx = smp_cpu_index_fast();
    if (idx < MAX_CPUS)
        return &cpu_data[idx];
    return &cpu_data[0];
}

per_cpu_t* smp_get_cpu(uint32_t cpu_id) {
    if (cpu_id >= MAX_CPUS) return NULL;
    if (!cpu_data[cpu_id].online) return NULL;
    return &cpu_data[cpu_id];
}

uint64_t smp_get_online_count(void) {
    return cpus_online;
}

/* ============================================================================
 * Inter-Processor Interrupts (IPI)
 * ============================================================================ */

#define ICR_DELIVERY_PENDING (1u << 12)

static void lapic_wait_icr_idle(void) {
    int spins = 100000;
    while (spins-- > 0) {
        if (!(apic_read((void *)lapic_addr, LAPIC_ICR_LOW) & ICR_DELIVERY_PENDING))
            return;
        __asm__ volatile("pause");
    }
}

void smp_send_ipi(uint32_t target_lapic_id, uint8_t vector) {
    if (!lapic_addr) return;

    lapic_wait_icr_idle();
    apic_write((void*)lapic_addr, LAPIC_ICR_HIGH,
               ((uint32_t)target_lapic_id) << 24);
    apic_write((void*)lapic_addr, LAPIC_ICR_LOW, (uint32_t)vector);
}

void smp_send_ipi_all_except_self(uint8_t vector) {
    if (!lapic_addr) return;

    lapic_wait_icr_idle();
    apic_write((void*)lapic_addr, LAPIC_ICR_HIGH, 0);
    apic_write((void*)lapic_addr, LAPIC_ICR_LOW,
               (uint32_t)vector | (3 << 18));
}


/* Generational TLB shootdown, safe against concurrent initiators.
 *
 * One request at a time (tlb_sd_lock).  The initiator bumps tlb_sd_gen, IPIs
 * every other CPU and waits until each ONLINE CPU has published
 * tlb_sd_acked[cpu] >= its generation.  A responder reads the generation
 * BEFORE reloading CR3, so its ack always covers PTE changes made before the
 * request was issued.  Acks are per CPU, so a late or duplicate IPI can never
 * be counted for the wrong request (the old shared counter could be reset by
 * a second initiator mid-wait, or reach `need` with the other request's acks).
 *
 * Deadlock avoidance: an initiator spins for tlb_sd_lock with interrupts off,
 * so it cannot take the current owner's IPI.  While spinning it services the
 * owner's request by polling (flush + ack), which is exactly what the IPI
 * handler would have done; the IPI that arrives later just flushes again. */
static spinlock_t tlb_sd_lock;
static volatile uint64_t tlb_sd_gen;
static volatile uint64_t tlb_sd_acked[MAX_CPUS];
static volatile uint32_t tlb_sd_timeouts;

static inline void tlb_flush_local(void) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

/* Flush this CPU's TLB and acknowledge every request issued so far. */
void smp_tlb_shootdown_ack(void) {
    uint32_t me = smp_cpu_index_fast();
    uint64_t g = __atomic_load_n(&tlb_sd_gen, __ATOMIC_SEQ_CST);
    tlb_flush_local();
    if (me < MAX_CPUS) {
        uint64_t old = __atomic_load_n(&tlb_sd_acked[me], __ATOMIC_RELAXED);
        if (g > old)
            __atomic_store_n(&tlb_sd_acked[me], g, __ATOMIC_SEQ_CST);
    }
}

/* Answer a request that is waiting on this CPU (used while spinning with
 * interrupts disabled). */
static inline void tlb_sd_poll(void) {
    uint32_t me = smp_cpu_index_fast();
    if (me < MAX_CPUS &&
        __atomic_load_n(&tlb_sd_acked[me], __ATOMIC_RELAXED) <
        __atomic_load_n(&tlb_sd_gen, __ATOMIC_ACQUIRE))
        smp_tlb_shootdown_ack();
}

void smp_tlb_shootdown(void) {
    uint64_t online = cpus_online;
    if (online <= 1) {
        tlb_flush_local();
        return;
    }
    uint64_t flags = irq_save();
    while (__sync_lock_test_and_set(&tlb_sd_lock.lock, 1)) {
        while (__atomic_load_n(&tlb_sd_lock.lock, __ATOMIC_RELAXED)) {
            tlb_sd_poll();
            __asm__ volatile("pause");
        }
    }
    uint32_t me = smp_cpu_index_fast();
    uint64_t g = __atomic_add_fetch(&tlb_sd_gen, 1, __ATOMIC_SEQ_CST);
    smp_send_ipi_all_except_self(IPI_VECTOR_TLB_FLUSH);
    tlb_flush_local();
    if (me < MAX_CPUS)
        __atomic_store_n(&tlb_sd_acked[me], g, __ATOMIC_SEQ_CST);

    uint32_t missing = 0;
    for (uint64_t spins = 0; ; spins++) {
        missing = 0;
        for (uint32_t c = 0; c < MAX_CPUS; c++) {
            if (c == me || !cpu_data[c].online) continue;
            if (__atomic_load_n(&tlb_sd_acked[c], __ATOMIC_ACQUIRE) < g)
                missing |= (c < 32) ? (1u << c) : 0x80000000u;
        }
        if (!missing || spins >= 20000000ULL)
            break;
        __asm__ volatile("pause");
    }
    __sync_lock_release(&tlb_sd_lock.lock);
    irq_restore(flags);
    if (missing && tlb_sd_timeouts < 16) {
        tlb_sd_timeouts++;
        serial_printf("smp: TLB shootdown incomplete gen=%llu missing_cpus=0x%x\n",
                      (unsigned long long)g, missing);
    }
}

void smp_reschedule_cpu(uint32_t cpu_id) {
    if (cpu_id >= MAX_CPUS || !cpu_data[cpu_id].online) return;
    smp_send_ipi(cpu_data[cpu_id].lapic_id, IPI_VECTOR_RESCHEDULE);
}

/* ============================================================================
 * Utilities
 * ============================================================================ */

void smp_halt_all(void) {
    serial_puts("smp: HALTING ALL CPUs\n");

    /* Send panic IPI to all other CPUs */
    smp_send_ipi_all_except_self(IPI_VECTOR_PANIC);

    /* Halt this CPU */
    __asm__ volatile("cli");
    while (1) {
        __asm__ volatile("hlt");
    }
}

void smp_halt_all_except_self(void) {
    serial_puts("smp: halting all APs\n");

    /* Send panic IPI to all other CPUs */
    smp_send_ipi_all_except_self(IPI_VECTOR_PANIC);

    /* Give APs time to halt */
    for (volatile int i = 0; i < 100000; i++) {
        __asm__ volatile("nop");
    }
}

/**
 * @brief Create idle threads for all online CPUs
 *
 * Each CPU gets its own idle thread that runs when no real work
 * is available. This enables the preemptive scheduler to always
 * have a valid thread context on every CPU.
 *
 * Call after smp_start_aps() and scheduler_init().
 */
void smp_create_idle_threads(void) {
    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        if (!cpu_data[i].online) continue;

        char name[32];
        snprintf(name, sizeof(name), "idle/%d", i);

        thread_t *idle = thread_create(name, idle_thread_func, NULL,
                                       4096, PRIORITY_LOW);
        if (idle) {
            idle->home_cpu = i;
            cpu_data[i].idle_thread = idle;
            if (cpu_data[i].is_bsp)
                cpu_data[i].current_thread = idle;
            idle->state = THREAD_RUNNING;
            serial_printf("smp: created idle thread for CPU %d (tid %llu)\n",
                          i, (unsigned long long)idle->id);
        } else {
            serial_printf("smp: warning - failed to create idle thread for CPU %d\n", i);
        }
    }

    smp_attach_bsp_boot_thread();

    /* APs are sitting in cpu_idle_loop(); poke them so the timer/IPI
     * path attaches idle as current_thread and starts taking work. */
    for (uint32_t i = 1; i < MAX_CPUS; i++) {
        if (cpu_data[i].online)
            smp_reschedule_cpu(i);
    }

    printf("SMP: Idle threads created for %llu CPUs\n",
           (unsigned long long)cpus_online);
}

/**
 * @brief Assign a thread to a specific CPU for execution
 *
 * Adds the thread to the global ready queue and sends a reschedule
 * IPI to the target CPU so it picks up the work.
 *
 * @param thread  Thread to schedule
 * @param cpu_id  Target CPU (or -1 for any available CPU)
 */
void smp_schedule_thread(thread_t *thread, int cpu_id) {
    if (!thread) return;

    if (cpu_id >= 0 && (uint32_t)cpu_id < MAX_CPUS && cpu_data[cpu_id].online)
        thread->home_cpu = (uint32_t)cpu_id;

    scheduler_add_thread(thread);
}

/**
 * @brief Get the current thread on the calling CPU
 *
 * SMP-aware version: uses LAPIC ID to find the per-CPU data
 * and return its current_thread.
 */
thread_t* smp_get_current_thread(void) {
    per_cpu_t *cpu = smp_get_current_cpu();
    return cpu ? cpu->current_thread : NULL;
}

/**
 * @brief Update per-CPU statistics from timer tick
 *
 * Called from the HPET timer handler. Increments tick count
 * and idle tracking for the current CPU.
 */
void smp_timer_tick(void) {
    per_cpu_t *cpu = smp_get_current_cpu();
    if (!cpu) return;

    cpu->total_ticks++;

    /* Track idle time */
    if (cpu->current_thread == cpu->idle_thread) {
        cpu->idle_ticks++;
    }
}

void smp_print_status(void) {
    printf("SMP Status: %llu/%llu CPUs online\n",
           (unsigned long long)cpus_online,
           (unsigned long long)total_cpus);

    for (uint32_t i = 0; i < MAX_CPUS; i++) {
        if (cpu_data[i].online) {
            uint64_t idle_pct = cpu_data[i].total_ticks > 0
                ? (cpu_data[i].idle_ticks * 100) / cpu_data[i].total_ticks
                : 100;
            printf("  CPU %d: LAPIC=%d %s ticks=%llu switches=%llu rq=%u idle=%llu%%",
                   cpu_data[i].cpu_id,
                   cpu_data[i].lapic_id,
                   cpu_data[i].is_bsp ? "(BSP)" : "(AP)",
                   (unsigned long long)cpu_data[i].total_ticks,
                   (unsigned long long)cpu_data[i].context_switches,
                   cpu_data[i].ready_count,
                   (unsigned long long)idle_pct);
            if (cpu_data[i].current_thread) {
                printf(" running='%s'", cpu_data[i].current_thread->name);
            }
            printf("\n");
        }
    }
}
