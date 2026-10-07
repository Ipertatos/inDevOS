/**
 * @file smp.h
 * @brief Symmetric Multi-Processing (SMP) support
 *
 * Provides per-CPU data structures, secondary CPU initialization,
 * and inter-processor interrupt (IPI) support.
 */

#ifndef SMP_H
#define SMP_H

#include "types.h"
#include "atomic.h"
#include "thread.h"

/** @brief Maximum number of CPUs supported */
#define MAX_CPUS        16

/** @brief Per-CPU stack size (16KB) */
#define AP_STACK_SIZE   16384

/** @brief IPI vector for scheduling reschedule */
#define IPI_VECTOR_RESCHEDULE   0xFD

/** @brief IPI vector for TLB shootdown */
#define IPI_VECTOR_TLB_FLUSH    0xFE

/** @brief IPI vector for panic/halt all CPUs */
#define IPI_VECTOR_PANIC        0xFF

/**
 * @brief Per-CPU data structure
 *
 * Each CPU has its own instance of this structure, containing
 * CPU-local state needed for scheduling and operation.
 */
typedef struct per_cpu {
    uint32_t cpu_id;                /**< CPU index (0 = BSP) */
    uint32_t lapic_id;              /**< Local APIC ID */
    bool online;                    /**< CPU is initialized and running */
    bool is_bsp;                    /**< Is this the bootstrap processor? */

    /* Scheduling */
    thread_t *current_thread;       /**< Currently running thread on this CPU */
    thread_t *idle_thread;          /**< Idle thread for this CPU */
    uint64_t idle_ticks;            /**< Number of ticks spent idle */
    spinlock_t run_lock;            /**< Lock for scheduling operations */
    thread_t *ready_queue[THREAD_NUM_PRIORITIES];
    thread_t *ready_queue_tail[THREAD_NUM_PRIORITIES];
    uint32_t ready_count;           /**< Threads queued on this CPU */

    /* Stack */
    uint8_t *stack_base;            /**< Base of this CPU's kernel stack */
    uint64_t stack_top;             /**< Top of kernel stack */

    /* Statistics */
    uint64_t total_ticks;           /**< Total timer ticks on this CPU */
    uint64_t context_switches;      /**< Number of context switches */
} per_cpu_t;

/* ============================================================================
 * Global SMP state
 * ============================================================================ */

/** @brief Array of per-CPU data structures */
extern per_cpu_t cpu_data[MAX_CPUS];

/** @brief Number of CPUs that have completed initialization */
extern volatile uint64_t cpus_online;

/** @brief Total number of CPUs detected */
extern uint64_t total_cpus;

/* ============================================================================
 * Initialization
 * ============================================================================ */

/**
 * @brief Initialize the SMP subsystem
 *
 * Sets up per-CPU data structures for the BSP and prepares
 * for secondary CPU bringup. Called early in kernel init.
 */
void smp_init(void);

/**
 * @brief Bring up all secondary (AP) processors
 *
 * Sends startup signals to all application processors detected
 * by the Limine bootloader. Each AP will initialize its own
 * GDT, IDT, and LAPIC, then enter the scheduler idle loop.
 *
 * Must be called after smp_init() and after the scheduler is ready.
 */
void smp_start_aps(void);

/* ============================================================================
 * Per-CPU access
 * ============================================================================ */

/**
 * @brief Get the per-CPU data for the current CPU
 *
 * Uses the LAPIC ID to identify the current CPU.
 *
 * @return Pointer to per-CPU data, or BSP data as fallback
 */
per_cpu_t* smp_get_current_cpu(void);

/**
 * @brief Get the per-CPU data for a specific CPU by index
 *
 * @param cpu_id CPU index (0 = BSP)
 * @return Pointer to per-CPU data, or NULL if invalid
 */
per_cpu_t* smp_get_cpu(uint32_t cpu_id);

/**
 * @brief Get the current CPU's LAPIC ID
 * @return LAPIC ID of the executing CPU
 */
uint32_t smp_get_lapic_id(void);

/** O(1) current CPU index (LAPIC id map). Safe before SMP init (returns 0). */
uint32_t smp_cpu_index_fast(void);

/**
 * @brief Get the number of online CPUs
 * @return Number of CPUs that are initialized and running
 */
uint64_t smp_get_online_count(void);

/* ============================================================================
 * Inter-Processor Interrupts (IPI)
 * ============================================================================ */

/**
 * @brief Send an IPI to a specific CPU
 *
 * @param target_lapic_id LAPIC ID of the target CPU
 * @param vector          Interrupt vector to deliver
 */
void smp_send_ipi(uint32_t target_lapic_id, uint8_t vector);

/**
 * @brief Send an IPI to all CPUs except the sender
 *
 * @param vector Interrupt vector to deliver
 */
void smp_send_ipi_all_except_self(uint8_t vector);
/** Broadcast TLB flush and wait until every online CPU has flushed. */
void smp_tlb_shootdown(void);
/** Called from IPI_VECTOR_TLB_FLUSH handler after local flush. */
void smp_tlb_shootdown_ack(void);

/**
 * @brief Request a reschedule on a specific CPU
 *
 * Sends the reschedule IPI to the target CPU, causing it to
 * run the scheduler at the next opportunity.
 *
 * @param cpu_id Target CPU index
 */
void smp_reschedule_cpu(uint32_t cpu_id);

/* ============================================================================
 * SMP-aware utilities
 * ============================================================================ */

/**
 * @brief Halt all CPUs (used during panic)
 *
 * Sends a halt IPI to all other CPUs and halts the current CPU.
 */
void smp_halt_all(void);

/**
 * @brief Halt all CPUs except the current one
 *
 * Sends a halt IPI to all other CPUs but keeps the current CPU running.
 * Used for shutdown/reboot where the BSP needs to remain active.
 */
void smp_halt_all_except_self(void);

/**
 * @brief Create idle threads for all online CPUs
 *
 * Each CPU gets its own idle thread. Call after smp_start_aps()
 * and scheduler_init().
 */
void smp_create_idle_threads(void);

/**
 * @brief BSP kernel-main thread context (saved when preempted to Ring-3).
 * @return The boot/main-loop thread, or NULL before smp_create_idle_threads().
 */
thread_t *smp_get_bsp_boot_thread(void);

/**
 * @brief Assign a thread to a CPU for execution
 *
 * @param thread  Thread to schedule
 * @param cpu_id  Target CPU index, or -1 for automatic selection
 */
void smp_schedule_thread(thread_t *thread, int cpu_id);

/**
 * @brief Get the current thread on the calling CPU
 * @return Current thread, or NULL
 */
thread_t* smp_get_current_thread(void);

/**
 * @brief Update per-CPU timer statistics (called from timer ISR)
 */
void smp_timer_tick(void);

/**
 * @brief Print SMP status information
 */
void smp_print_status(void);

#endif /* SMP_H */
