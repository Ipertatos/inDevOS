#pragma once 
#include "types.h"

extern uint32_t bsp_lapic_id;
extern uint64_t cpu_count;

/** @brief Initialize CPU detection (BSP only, APs are halted initially) */
extern void cpu_init(void);

/** @brief Set when the running CPUs have CR4.SMAP on (stac/clac are valid) */
extern bool cpu_smap_enabled;
extern bool cpu_smep_enabled;

/**
 * @brief Enable NX (EFER.NXE), SMEP and SMAP when CPUID reports them.
 *        Must run on every CPU before it executes user code.
 */
void cpu_enable_protections(void);

/** @brief Get the CPU brand string (allocated via kalloc, caller must free) */
char* cpu_name(void);

/** @brief Number of CPUs that reported as awake during initial bringup */
extern volatile uint64_t _cpus_awake;