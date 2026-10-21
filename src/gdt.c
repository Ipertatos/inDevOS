#include "gdt.h"
#include "utils.h"
#include "mem.h"
#include "smp.h"

#define GDTSEGMENTS 7 // 6 + 1 - TSS takes up 2

static gdt_entry_t gdt[GDTSEGMENTS];
static gdt_entry_t gdt_ap[MAX_CPUS][GDTSEGMENTS];
static tss_t tss_ap[MAX_CPUS];
static uint8_t ap_kstack[MAX_CPUS][16384] __attribute__((aligned(16)));

gdt_pointer_t gdt_pointer;
tss_t tss;

// Kernel stack for Ring 3 -> Ring 0 transitions (set during init)
static uint8_t kernel_stack_area[16384] __attribute__((aligned(16)));

// IST stacks for critical exception handlers (double fault, NMI)
// These use separate stacks so they can fire even when the main stack is corrupted.
static uint8_t ist1_stack[16384] __attribute__((aligned(16)));  // IST1: Double Fault (#DF)
static uint8_t ist2_stack[16384] __attribute__((aligned(16)));  // IST2: NMI
static uint8_t ist3_stack[16384] __attribute__((aligned(16)));  // IST3: Machine Check (#MC)

void gdt_set_entry(int num, uint64_t base, uint64_t limit, uint8_t access, uint8_t granularity){
    gdt[num].base_low = (base & 0xFFFF);
    gdt[num].base_middle = (base >> 16) & 0xFF;
    gdt[num].base_high = (base >> 24) & 0xFF;

    gdt[num].limit_low = (limit & 0xFFFF);
    gdt[num].granularity = ((limit >> 16) & 0x0F);

    gdt[num].granularity |= (granularity & 0xF) << 4;
    gdt[num].access = access;
}

/*
 * GDT Layout (ordered for syscall/sysret compatibility):
 *   0x00 (0): NULL descriptor
 *   0x08 (1): Kernel code segment (Ring 0)
 *   0x10 (2): Kernel data segment (Ring 0)
 *   0x18 (3): User data segment   (Ring 3) -- must come before user code for sysret
 *   0x20 (4): User code segment   (Ring 3)
 *   0x28 (5): TSS (takes up 2 entries)
 *
 * For SYSCALL: STAR[47:32] = 0x08  -> kernel CS=0x08, kernel SS=0x08+8=0x10
 * For SYSRET:  STAR[63:48] = 0x10  -> user SS=0x10+8=0x18|3=0x1B, user CS=0x10+16=0x20|3=0x23
 */
void gdt_init(){
    gdt_pointer.limit_low = (sizeof(gdt_entry_t) * GDTSEGMENTS) - 1;
    gdt_pointer.base = (uint64_t)&gdt;

    memset(&tss, 0, sizeof(tss));

    // Set up TSS kernel stack (RSP0) for Ring 3 -> Ring 0 transitions
    tss.rsp0 = (uint64_t)&kernel_stack_area[sizeof(kernel_stack_area)];

    // Set up IST entries for critical exceptions that need known-good stacks
    tss.ist1 = (uint64_t)&ist1_stack[sizeof(ist1_stack)];  // Double Fault
    tss.ist2 = (uint64_t)&ist2_stack[sizeof(ist2_stack)];  // NMI
    tss.ist3 = (uint64_t)&ist3_stack[sizeof(ist3_stack)];  // Machine Check

    //entry 0 null
    gdt_set_entry(0, 0, 0, 0, 0);

    //entry 1 kernel code seg (0x08)
    gdt_set_entry(1, 0, 0xFFFF, 0x9A, 0xA);

    //entry 2 kernel data seg (0x10)
    gdt_set_entry(2, 0, 0xFFFF, 0x92, 0xA);

    //entry 3 user data seg (0x18) - MUST be before user code for sysret
    gdt_set_entry(3, 0, 0xFFFF, 0xF2, 0xA);

    //entry 4 user code seg (0x20)
    gdt_set_entry(4, 0, 0xFFFF, 0xFA, 0xA);

    //entry 5 TSS (0x28) - first 8 bytes of 16-byte 64-bit TSS descriptor
    gdt_set_entry(5, (uint64_t)&tss, sizeof(tss), 0x89, 0x0);

    /*
     * 64-bit TSS descriptors are 16 bytes.  The second 8 bytes (entry 6)
     * hold bits [63:32] of the base address followed by 4 reserved bytes.
     *
     * gdt_entry_t byte layout for the upper half:
     *   limit_low  (bytes 0-1 of gdt[6]) = base[47:32]
     *   base_low   (bytes 2-3 of gdt[6]) = base[63:48]
     *   everything else                  = 0 (reserved)
     */
    {
        uint64_t tss_base = (uint64_t)&tss;
        gdt[6].limit_low   = (uint16_t)(tss_base >> 32);
        gdt[6].base_low    = (uint16_t)(tss_base >> 48);
        gdt[6].base_middle = 0;
        gdt[6].access      = 0;
        gdt[6].granularity = 0;
        gdt[6].base_high   = 0;
    }

    s_setgdt(gdt_pointer.limit_low, gdt_pointer.base);

    s_settss();
}

/**
 * @brief Set the kernel stack pointer in the TSS for Ring 3 transitions
 * Call this when switching processes to update the kernel stack.
 */
void gdt_init_ap(uint32_t cpu_id) {
    if (cpu_id == 0 || cpu_id >= MAX_CPUS)
        return;

    memcpy(gdt_ap[cpu_id], gdt, sizeof(gdt));
    memset(&tss_ap[cpu_id], 0, sizeof(tss_t));
    tss_ap[cpu_id].rsp0 = (uint64_t)&ap_kstack[cpu_id][sizeof(ap_kstack[cpu_id])];
    tss_ap[cpu_id].ist1 = tss.ist1;
    tss_ap[cpu_id].ist2 = tss.ist2;
    tss_ap[cpu_id].ist3 = tss.ist3;

    uint64_t tss_base = (uint64_t)&tss_ap[cpu_id];
    gdt_entry_t *ag = gdt_ap[cpu_id];
    ag[5].base_low    = (uint16_t)(tss_base & 0xFFFF);
    ag[5].base_middle = (uint8_t)((tss_base >> 16) & 0xFF);
    ag[5].base_high   = (uint8_t)((tss_base >> 24) & 0xFF);
    ag[5].limit_low   = (uint16_t)sizeof(tss_t);
    ag[5].access      = 0x89;
    ag[5].granularity = 0;
    ag[6].limit_low   = (uint16_t)(tss_base >> 32);
    ag[6].base_low    = (uint16_t)(tss_base >> 48);
    ag[6].base_middle = 0;
    ag[6].access      = 0;
    ag[6].granularity = 0;
    ag[6].base_high   = 0;

    gdt_pointer_t ptr;
    ptr.limit_low = (sizeof(gdt_entry_t) * GDTSEGMENTS) - 1;
    ptr.base = (uint64_t)ag;
    s_setgdt(ptr.limit_low, ptr.base);
    s_settss();
}

void gdt_set_kernel_stack(uint64_t stack_top) {
    per_cpu_t *cpu = smp_get_current_cpu();
    uint32_t id = (cpu && cpu->cpu_id < MAX_CPUS) ? cpu->cpu_id : 0;
    if (id == 0)
        tss.rsp0 = stack_top;
    else
        tss_ap[id].rsp0 = stack_top;
}

uint64_t gdt_get_kernel_stack(void) {
    per_cpu_t *cpu = smp_get_current_cpu();
    uint32_t id = (cpu && cpu->cpu_id < MAX_CPUS) ? cpu->cpu_id : 0;
    if (id == 0)
        return tss.rsp0;
    return tss_ap[id].rsp0;
}