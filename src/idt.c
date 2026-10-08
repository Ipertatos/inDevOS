#include "idt.h"
#include "fpu.h"
#include "hpet.h"
#include "apic.h"
#include "utils.h"
#include "keyboard.h"
#include "mouse.h"
#include "atomic.h"
#include "debug.h"
#include "serial.h"
#include "thread.h"
#include "gdt.h"
#include "syscall.h"
#include "exec.h"
#include "mem.h"
#include "term.h"
#include "kernel_symtab.h"
#include "rtl8139.h"
#include "e1000.h"
#include "luade.h"
#include "smp.h"
#include "profiler.h"
#include "vconsole.h"
#include "uaccess.h"
#include "cpu.h"
#include "debug_config.h"

#define RFLAGS_AC (1ULL << 18)

// Forward declaration for print_stacktrace
void print_stacktrace(uint64_t rbp);

__attribute__((aligned(0x10))) static idt_entry_t idt_entries[256];
idtr_t idtr;

extern void isr0();
extern void isr1();
extern void isr2();
extern void isr3();
extern void isr4();
extern void isr5();
extern void isr6();
extern void isr7();
extern void isr8();
extern void isr9();
extern void isr10();
extern void isr11();
extern void isr12();
extern void isr13();
extern void isr14();
extern void isr15();
extern void isr16();
extern void isr17();
extern void isr18();
extern void isr19();
extern void isr20();
extern void isr21();
extern void isr22();
extern void isr23();
extern void isr24();
extern void isr25();
extern void isr26();
extern void isr27();
extern void isr28();
extern void isr29();
extern void isr30();
extern void isr31();
extern void isr32();
extern void isr33();
extern void isr35();
extern void isr43();
extern void isr44();
extern void isr45();
extern void isr46();
extern void isr47();
extern void isr48();
extern void isr253();  /* IPI_VECTOR_RESCHEDULE */
extern void isr254();  /* IPI_VECTOR_TLB_FLUSH */
extern void isr255();  /* IPI_VECTOR_PANIC */
void initIDT() {
    idtr.limit = (uint16_t)sizeof(idt_entry_t) * 256 - 1;
    idtr.base = (uint64_t)&idt_entries[0];

    setIdtGate(0, isr0, 0x8E);
    setIdtGate(1, isr1, 0x8E);
    setIdtGateIST(2, isr2, 0x8E, 2);  // NMI uses IST2 (dedicated stack)
    setIdtGate(3, isr3, 0x8E);
    setIdtGate(4, isr4, 0x8E);
    setIdtGate(5, isr5, 0x8E);
    setIdtGate(6, isr6, 0x8E);
    setIdtGate(7, isr7, 0x8E);
    setIdtGateIST(8, isr8, 0x8E, 1);  // Double Fault uses IST1 (dedicated stack)
    setIdtGate(9, isr9, 0x8E);
    setIdtGate(10, isr10, 0x8E);
    setIdtGate(11, isr11, 0x8E);
    setIdtGate(12, isr12, 0x8E);
    setIdtGate(13, isr13, 0x8E);
    setIdtGate(14, isr14, 0x8E);
    setIdtGate(15, isr15, 0x8E);
    setIdtGate(16, isr16, 0x8E);
    setIdtGate(17, isr17, 0x8E);
    setIdtGate(18, isr18, 0x8E);
    setIdtGate(19, isr19, 0x8E);
    setIdtGate(20, isr20, 0x8E);
    setIdtGate(21, isr21, 0x8E);
    setIdtGate(22, isr22, 0x8E);
    setIdtGate(23, isr23, 0x8E);
    setIdtGate(24, isr24, 0x8E);
    setIdtGate(25, isr25, 0x8E);
    setIdtGate(26, isr26, 0x8E);
    setIdtGate(27, isr27, 0x8E);
    setIdtGate(28, isr28, 0x8E);
    setIdtGate(29, isr29, 0x8E);
    setIdtGate(30, isr30, 0x8E);
    setIdtGate(31, isr31, 0x8E);
    setIdtGate(32, isr32, 0x8E);//LAPIC timer
    setIdtGate(33, isr33, 0x8E); //PS/2 keyboard
    setIdtGate(35, isr35, 0x8E); //HPET timer
    setIdtGate(43, isr43, 0x8E); //RTL8139 network card
    setIdtGate(44, isr44, 0x8E); //E1000 network card
    setIdtGate(45, isr45, 0x8E); //Reserved
    setIdtGate(46, isr46, 0x8E); //RTL810xE network card
    setIdtGate(47, isr47, 0x8E); //Reserved
    setIdtGate(48, isr48, 0x8E); //AHCI SATA controller

    /* SMP IPI vectors */
    setIdtGate(253, isr253, 0x8E); // IPI: Reschedule
    setIdtGate(254, isr254, 0x8E); // IPI: TLB Flush
    setIdtGate(255, isr255, 0x8E); // IPI: Panic/Halt

    __asm__ __volatile__("lidtq %0" : : "m"(idtr));
}

void idt_load_current(void) {
    __asm__ __volatile__("lidtq %0" : : "m"(idtr));
    __asm__ __volatile__("sti");

}

void setIdtGate(uint8_t num, void* base, uint8_t flags) {
    setIdtGateIST(num, base, flags, 0);
}

void setIdtGateIST(uint8_t num, void* base, uint8_t flags, uint8_t ist) {
    idt_entry_t* desc = &idt_entries[num];
    
    desc->isr_low        = (uint64_t)base & 0xFFFF;
    desc->kernel_cs      = 0x08;
    desc->ist            = ist & 0x7;  // IST index is 3 bits (0 = no IST, 1-7 = IST entry)
    desc->attributes     = flags;
    desc->isr_mid        = ((uint64_t)base >> 16) & 0xFFFF;
    desc->isr_high       = ((uint64_t)base >> 32) & 0xFFFFFFFF;
    desc->reserved       = 0;
}


char* exception_messages[] = {
    "Division By Zero",
    "Debug",
    "Non Maskable Interrupt",
    "Breakpoint",
    "Into Detected Overflow",
    "Out of Bounds",
    "Invalid Opcode",
    "No Coprocessor",
    "Double fault",
    "Coprocessor Segment Overrun",
    "Bad TSS",
    "Segment not present",
    "Stack fault",
    "General protection fault",
    "Page fault",   
    "Unknown Interrupt",
    "Coprocessor Fault",
    "Alignment Fault",
    "Machine Check", 
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved"
};

// Forward declaration for preemptive scheduling
void preemptive_schedule(registers_t *r);
static void preemptive_schedule_force(void);

/* Job control on every CPU's LAPIC tick: dispatch a posted tty signal and
 * kill a ring-3-interrupted process whose pending signal is fatal by default
 * (busy loops never reach a syscall-return delivery point). */
static void tty_timer_hook(registers_t *r) {
    extern void tty_dispatch_pending(void);
    extern int proc_fatal_default_pending(process_t *p);
    extern void process_fault_kill_current(int signum);
    extern process_t *get_kernel_process(void);
    tty_dispatch_pending();
    if ((r->cs & 3) != 3) return;
    thread_t *cth = scheduler_get_current_thread();
    process_t *op = (cth && cth->owner_process) ? (process_t *)cth->owner_process
                                                : exec_user_as_process();
    int fsig = proc_fatal_default_pending(op);
    if (!fsig) {
        extern int process_irq_deliver(process_t *p, registers_t *r);
        process_irq_deliver(op, r);
        return;
    }
    bool fork_child = op->sched_thread && (thread_t *)op->sched_thread == cth &&
                      op->parent && op->parent != get_kernel_process();
    bool sync = !fork_child && exec_current_is_sync_program();
    if (!fork_child && !sync) return;
    serial_printf("tty: ring3 kill pid=%d sig=%d\n", (int)op->pid, fsig);
    op->pending_signals &= ~(1u << fsig);
    set_current_process(op);
    exit_interrupt_context();
    if (fork_child) {
        process_fault_kill_current(fsig);
        __builtin_unreachable();
    }
    exec_force_terminate(128 + fsig);
    __builtin_unreachable();
}

void handle_timer(registers_t *r){
    hpet_isr();
    fb_cursor_blink_tick();

    /* TTY switch is handled in the main loop (vconsole_poll_switch).
     * Doing the full save/restore + printf from this ISR painted over
     * the desktop and stalled the timer. */

    /* Force-terminate a running program if Ctrl+C was pressed and the
     * process is not responding (no syscalls being made).
     * For Ring-3 (user mode): safe to force-kill immediately.
     * For kernel-mode flat binaries: also safe via setjmp/longjmp. */
    if (get_terminate_flag() && get_program_running()) {
        bool is_ring3 = (r->cs & 3) == 3;
        if (is_ring3 || get_force_kill_flag()) {
            set_terminate_flag(false);
            hpet_ack();
            apic_eoi();
            exit_interrupt_context();
            exec_force_terminate(128 + 2 /* SIGINT */);
            __builtin_unreachable();
        }
    }

    hpet_ack();
    apic_eoi();
}

/*
 * Map a demand-paged heap/stack page or break a COW mapping for @proc.
 * Shared by user faults, kernel faults on user addresses and uaccess.
 */
bool user_fault_fixup(struct process *proc, uint64_t faulting_address,
                      bool page_present, bool was_write) {
    if (!proc || !proc->is_usermode || !proc->pml4 ||
        !is_user_address(faulting_address))
        return false;

    /* ================================================================
     * Demand Paging: "not present" fault in the heap or stack window
     * ================================================================ */
    if (!page_present) {
        {
            uint64_t page_addr = faulting_address & ~0xFFFULL;

            /* Check if the faulting address is in a valid demand-paged region:
             *  - heap:  [brk_start .. brk_current)
             *  - stack: [USER_STACK_TOP - 1MB .. USER_STACK_TOP + PAGE_SIZE)
             *
             * The upper bound is inclusive of the page AT USER_STACK_TOP because
             * the Linux ABI places the initial argc/argv/envp frame exactly there.
             */
            bool in_heap = (faulting_address >= (uint64_t)proc->brk_start &&
                            faulting_address <  (uint64_t)proc->brk_current);
            uint64_t stack_guard = USER_STACK_TOP - (1024 * 1024);  /* 1MB stack limit */
            bool in_stack = (faulting_address >= stack_guard &&
                             faulting_address <  USER_STACK_TOP + PAGE_SIZE);

            if (in_heap || in_stack) {
                uint64_t frame = vmm_alloc_frame();
                if (frame) {
                    memset(phys_to_virt(frame), 0, PAGE_SIZE);
                    vmm_map_page_in(proc->pml4, page_addr, frame,
                                    VMM_PRESENT | VMM_USER | VMM_WRITABLE | VMM_NX);
                    SERIAL_DEBUG_PRINT(MEMORY, "[demand-page] Mapped 0x%llx for PID %d (%s)\n",
                                       (unsigned long long)page_addr,
                                       (int)proc->pid,
                                       in_heap ? "heap" : "stack");
                    return true;  /* Resume faulting instruction */
                }
                serial_puts("[demand-page] Out of physical frames!\n");
                /* Fall through to kill / diagnostics */
            }
        }
    }

    /* ================================================================
     * Copy-on-Write / break-RO: write fault on a present user page
     * ================================================================
     * Always walk the *active* CR3 PML4 (the CPU's tables).  proc->pml4 can
     * lag behind under concurrent fork (ash parent still named current while
     * the child CR3 is loaded), and fixing the wrong PTE leaves the child's
     * RO entry intact — retry then kills the child.
     *
     * Also accept present+RO+USER writes in the stack/heap window even when
     * VMM_COW was lost: sole-owner → make writable; shared → copy.
     */
    if (page_present && was_write) {
        uint64_t page_addr = faulting_address & ~0xFFFULL;
        uint64_t cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
        uint64_t *active_pml4 = (uint64_t *)phys_to_virt(cr3 & PTE_ADDR_MASK);
        uint64_t *pml4 = active_pml4 ? active_pml4 : (proc ? proc->pml4 : NULL);
        uint64_t *pte = pml4 ? vmm_get_pte_in(pml4, page_addr) : NULL;

        uint64_t stack_lo = USER_STACK_TOP - (1024ULL * 1024ULL);
        uint64_t stack_hi = USER_STACK_TOP + PAGE_SIZE;
        bool in_stack = (faulting_address >= stack_lo && faulting_address < stack_hi);
        bool in_heap = proc && (faulting_address >= (uint64_t)proc->brk_start &&
                                faulting_address < (uint64_t)proc->brk_current);
        bool cow_bit = pte && (*pte & VMM_PRESENT) && (*pte & VMM_COW);
        bool ro_user = pte && (*pte & VMM_PRESENT) && (*pte & VMM_USER) &&
                       !(*pte & VMM_WRITABLE);
        /* Stack/heap: always break. Also break fork-shared RO user pages
         * (refs>1) even if VMM_COW was cleared — otherwise the child #PFs
         * writing errno/.data (pipe_sigpipe RIP=0x40172f va=0x401030).
         * Sole-owner PROT_READ mmap (refs<=1, not stack/heap) still faults. */
        bool break_ro = cow_bit || (ro_user && (in_stack || in_heap));
        extern uint8_t vmm_page_get_ref(uint64_t);
        uint8_t refs_peek = 0;
        if (!break_ro && ro_user && pte) {
            refs_peek = vmm_page_get_ref(*pte & PTE_ADDR_MASK);
            if (refs_peek > 1)
                break_ro = true;
        }

        if (break_ro && pte) {
            uint64_t old_phys = *pte & PTE_ADDR_MASK;
            uint64_t old_flags = *pte & PTE_FLAGS_MASK;
            uint8_t refs = refs_peek ? refs_peek : vmm_page_get_ref(old_phys);

            /* refs==1: proven sole owner (every fork share is counted, the
             * peer already dropped its claim) — upgrade in place.
             * refs==0: untracked — no proof nobody else maps it, so copy
             * (treating it as sole-owner once mutated a frame a sibling still
             * mapped).  refs>1: classic COW copy.
             * After a copy the OLD frame's claim is dropped with the
             * vmm_leaf_release rules (TLB shootdown first when that frees
             * it); before this the refs==0 case leaked the old frame. */
            if (refs == 1) {
                *pte = old_phys | ((old_flags & ~VMM_COW) | VMM_WRITABLE);
                __asm__ volatile("invlpg (%0)" : : "r"(page_addr) : "memory");
                return true;
            }

            uint64_t new_frame = vmm_alloc_frame();
            if (new_frame) {
                /* Copy via kernel map BEFORE installing the new PTE. */
                memcpy(phys_to_virt(new_frame), phys_to_virt(old_phys), PAGE_SIZE);
                *pte = new_frame | ((old_flags & ~VMM_COW) | VMM_WRITABLE);
                __asm__ volatile("invlpg (%0)" : : "r"(page_addr) : "memory");
                vmm_cow_release_old(old_phys);
                return true;
            }
            serial_puts("[cow] Out of physical frames for COW!\n");
        }
    }
    return false;
}

static volatile int smap_audit_hits;
static volatile bool smap_selftest_quiet;

/* Log each kernel RIP that touches user memory outside stac/clac once. */
static void smap_report(uint64_t rip, uint64_t addr) {
    static uint64_t seen[32];
    static int nseen;
    smap_audit_hits++;
    if (smap_selftest_quiet)
        return;
    for (int i = 0; i < nseen; i++)
        if (seen[i] == rip)
            return;
    if (nseen < (int)(sizeof(seen) / sizeof(seen[0])))
        seen[nseen++] = rip;
    serial_printf("[smap] kernel read/write of user 0x%llx at RIP 0x%llx <%s> -- resumed with AC=1\n",
                  (unsigned long long)addr, (unsigned long long)rip, lookup_symbol(rip));
}

/* Touch a scratch user page without stac: SMAP must fault and the audit
 * path must resume us with AC set.  Needs a working VMM and the IDT. */
void smap_selftest(void) {
    if (!cpu_smap_enabled)
        return;
    const uint64_t va = 0x00007F0000000000ULL;
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    uint64_t *pml4 = (uint64_t *)phys_to_virt(cr3 & PTE_ADDR_MASK);
    uint64_t frame = vmm_alloc_frame();
    if (!frame)
        return;
    if (vmm_map_page_in(pml4, va, frame, VMM_PRESENT | VMM_USER | VMM_WRITABLE | VMM_NX) != 0) {
        vmm_free_frame(frame);
        return;
    }

    int before = smap_audit_hits;
    uint64_t rflags;
    smap_selftest_quiet = true;
    (void)*(volatile uint64_t *)va;
    __asm__ volatile("pushfq; pop %0; clac" : "=r"(rflags) :: "memory");
    smap_selftest_quiet = false;

    vmm_unmap_page_in(pml4, va);
    vmm_free_frame(frame);
    bool ok = smap_audit_hits == before + 1 && (rflags & RFLAGS_AC);
    klog_info("SMAP self-test: %s\n", ok ? "enforced, audit recovery OK" : "FAILED");
}

/* Returns true if the fault was fixed and user/kernel should resume (demand page, COW). */
bool handle_page_fault(registers_t* r) {
    DEBUG_ENTER_ARGS("error_code=0x%llx, rip=0x%llx", r->err_code, r->rip);
    uint64_t faulting_address;
    
    // Read CR2 register to get the faulting address
    __asm__ volatile("mov %%cr2, %0" : "=r" (faulting_address));
    DEBUG_MSG("Page fault at address: 0x%llx", faulting_address);

    bool from_user = (r->err_code & 0x4) != 0;       /* U/S bit */
    bool page_present = (r->err_code & 0x1) != 0;     /* P bit  */
    bool was_write = (r->err_code & 0x2) != 0;        /* W/R bit */
    bool insn_fetch = (r->err_code & 0x10) != 0;      /* I/D bit */

    if (from_user) {
        /* Resolve the process that owns the *active* CR3.  get_current_process()
         * can still name ash while the child CR3 is loaded (and vice versa). */
        process_t *fp = NULL;
        uint64_t cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
        uint64_t *active = (uint64_t *)phys_to_virt(cr3 & 0x000FFFFFFFFFF000ULL);
        extern process_t *process_find_by_pml4(uint64_t *pml4);
        if (active)
            fp = process_find_by_pml4(active);
        if (!fp || !fp->is_usermode || !fp->pml4) {
            fp = get_current_process();
            if (!fp || !fp->is_usermode || !fp->pml4)
                fp = exec_user_as_process();
        }
        if (user_fault_fixup(fp, faulting_address, page_present, was_write))
            return true;
    } else if (is_user_address(faulting_address) && !insn_fetch) {
        /* Kernel dereferenced a user pointer directly.  Under SMAP that
         * faults even on a valid page; tolerate it (audit mode) by
         * resuming with RFLAGS.AC set, which lasts until the next
         * sysret/iret back to user space. */
        if (page_present && cpu_smap_enabled && !(r->rflags & RFLAGS_AC)) {
            smap_report(r->rip, faulting_address);
            r->rflags |= RFLAGS_AC;
            return true;
        }
        if (user_fault_fixup(exec_user_as_process(), faulting_address,
                             page_present, was_write))
            return true;
    }

    if (!from_user && insn_fetch && is_user_address(faulting_address) && cpu_smep_enabled)
        serial_puts("[smep] kernel tried to execute a user page\n");

    /* ================================================================
     * Unrecoverable fault -- fall through to diagnostics
     * ================================================================ */
    serial_puts("\n");
    serial_puts("╔══════════════════════════════════════════════════════════════════════════════════════╗\n");
    serial_puts("║                                   PAGE FAULT                                         ║\n");
    serial_puts("╠══════════════════════════════════════════════════════════════════════════════════════╣\n");
    serial_printf("║ Faulting Address: 0x%016llx                                                   ║\n", faulting_address);
    serial_printf("║ Error Code:       0x%016llx                                                   ║\n", r->err_code);
    serial_printf("║ Instruction:      0x%016llx                                                   ║\n", r->rip);
    serial_puts("╚══════════════════════════════════════════════════════════════════════════════════════╝\n");
    
    // Analyze the faulting address
    serial_puts("\n📍 ADDRESS ANALYSIS:\n");
    
    // Inline the debug_analyze_address functionality to use serial output
    serial_printf("   Analyzing faulting address 0x%016llx\n", faulting_address);
    
    // Check if it's in kernel space
    if(faulting_address < 0x1000) {
        serial_puts("   - NULL pointer dereference (addr < 0x1000)\n");
    } else if(faulting_address < 0x100000) {
        serial_puts("   - Low memory access (possible uninitialized pointer)\n");
    } else if(faulting_address >= 0xFFFF800000000000ULL) {
        serial_puts("   - Kernel space address\n");
    } else {
        serial_puts("   - User space address\n");
    }
    
    // Check if it's aligned
    if(faulting_address % 8 != 0) {
        serial_printf("   - Unaligned access (addr %% 8 = %d)\n", (int)(faulting_address % 8));
    }
    
    // Check if it looks like a corrupted pointer
    if((faulting_address & 0xFF) == 0xCC) {
        serial_puts("   - Looks like poisoned memory (0xCC pattern)\n");
    }
    if((faulting_address & 0xFFFFFFFF) == 0xDEADBEEF) {
        serial_puts("   - Looks like debug pattern (0xDEADBEEF)\n");
    }
    
    // Decode error code with better formatting
    serial_puts("\n🔍 ERROR CODE BREAKDOWN:\n");
    if(r->err_code & 0x1) {
        serial_puts("   ✗ Page protection violation (page was present)\n");
    } else {
        serial_puts("   ✗ Page not present\n");
    }
    
    if(r->err_code & 0x2) {
        serial_puts("   📝 Write access attempted\n");
    } else {
        serial_puts("   📖 Read access attempted\n");
    }
    
    if(r->err_code & 0x4) {
        serial_puts("   👤 User mode access\n");
    } else {
        serial_puts("   🔒 Supervisor mode access\n");
    }
    
    if(r->err_code & 0x8) {
        serial_puts("   ⚠️  Reserved bit violation\n");
    }
    
    if(r->err_code & 0x10) {
        serial_puts("   🔄 Instruction fetch\n");
    }
    
    // Enhanced instruction analysis
    serial_puts("\n🎯 INSTRUCTION ANALYSIS:\n");
    serial_printf("   RIP: 0x%016llx\n", r->rip);
    
    // Check if RIP looks valid
    if(r->rip < 0x1000) {
        serial_puts("   ❌ Invalid RIP: NULL or very low address\n");
    } else if(r->rip >= 0xFFFF800000000000ULL) {
        serial_puts("   ✅ Kernel space instruction\n");
        
        // Try to determine what function this might be in
        if(r->rip >= 0xFFFF800000000000ULL && r->rip < 0xFFFF800000010000ULL) {
            serial_puts("   📍 Early kernel code region\n");
        }
        
        // Show bytes around the instruction (if accessible)
        serial_puts("   📄 Instruction bytes around RIP:\n");
        serial_puts("      Note: Not reading instruction bytes to avoid recursive page fault\n");
        serial_printf("      RIP: 0x%016llx (instruction address)\n", r->rip);
    } else {
        serial_puts("   👤 User space instruction\n");
        /* Help debug external programs: use full RIP with addr2line (PIE ELF) */
        process_t *proc = exec_user_as_process();
        if (proc && proc->is_usermode && proc->virtual_base != 0 && r->rip >= proc->virtual_base) {
            uint64_t load_base = proc->virtual_base;
            uint64_t offset = r->rip - load_base;
            serial_puts("   📍 User executable debug hint:\n");
            serial_printf("      Load base: 0x%llx  Offset from base: 0x%llx\n",
                          (unsigned long long)load_base, (unsigned long long)offset);
            serial_printf("      addr2line -e <your.elf> -f -C 0x%llx  (use full RIP; build with -g, do not strip)\n",
                          (unsigned long long)r->rip);
        }
    }
    
    // Enhanced register state with better formatting
    serial_puts("\n📊 REGISTER STATE:\n");
    serial_printf("   RAX: 0x%016llx   RBX: 0x%016llx\n", r->rax, r->rbx);
    serial_printf("   RCX: 0x%016llx   RDX: 0x%016llx\n", r->rcx, r->rdx);
    serial_printf("   RSI: 0x%016llx   RDI: 0x%016llx\n", r->rsi, r->rdi);
    serial_printf("   RBP: 0x%016llx   RSP: 0x%016llx\n", r->rbp, r->rsp);
    serial_printf("   R8:  0x%016llx   R9:  0x%016llx\n", r->r8, r->r9);
    serial_printf("   R10: 0x%016llx   R11: 0x%016llx\n", r->r10, r->r11);
    serial_printf("   R12: 0x%016llx   R13: 0x%016llx\n", r->r12, r->r13);
    serial_printf("   R14: 0x%016llx   R15: 0x%016llx\n", r->r14, r->r15);
    serial_printf("   CS:  0x%016llx   SS:  0x%016llx\n", r->cs, r->ss);
    serial_printf("   RFLAGS: 0x%016llx\n", r->rflags);
    
    // RFLAGS breakdown
    serial_puts("   RFLAGS breakdown: ");
    if(r->rflags & (1 << 0)) serial_puts("CF ");
    if(r->rflags & (1 << 2)) serial_puts("PF ");
    if(r->rflags & (1 << 4)) serial_puts("AF ");
    if(r->rflags & (1 << 6)) serial_puts("ZF ");
    if(r->rflags & (1 << 7)) serial_puts("SF ");
    if(r->rflags & (1 << 8)) serial_puts("TF ");
    if(r->rflags & (1 << 9)) serial_puts("IF ");
    if(r->rflags & (1 << 10)) serial_puts("DF ");
    if(r->rflags & (1 << 11)) serial_puts("OF ");
    serial_puts("\n");
    
    // Enhanced stack analysis
    serial_puts("\n🗂️  STACK ANALYSIS:\n");
    uint64_t current_rsp = r->rsp;
    serial_printf("   Current RSP: 0x%016llx\n", current_rsp);
    
    // Check stack validity
    if(current_rsp < 0x1000) {
        serial_puts("   ❌ Invalid stack pointer: NULL or very low\n");
    } else if(current_rsp >= 0xFFFF800000000000ULL) {
        serial_puts("   ✅ Kernel stack\n");
    } else {
        serial_puts("   👤 User stack\n");
    }
    
    // Stack dump - only safe for kernel stacks; skip user RSP to avoid double-fault
    if (current_rsp >= 0xFFFF800000000000ULL) {
        serial_puts("   Stack contents (first 128 bytes):\n");
        uint64_t* stack_ptr = (uint64_t*)current_rsp;
        for(int i = 0; i < 16; i++) {
            serial_printf("      [RSP+%d]: 0x%016llx", i*8, stack_ptr[i]);
            if(stack_ptr[i] >= 0xFFFF800000000000ULL && stack_ptr[i] < 0xFFFF800001000000ULL) {
                serial_puts("  \xe2\x86\x90 Possible return address");
            }
            if(stack_ptr[i] == 0) {
                serial_puts("  \xe2\x86\x90 NULL");
            }
            serial_puts("\n");
        }
    } else {
        serial_puts("   (skipping stack dump - user-space RSP, not safe to dereference)\n");
    }
    
    // Memory context around faulting address
    serial_puts("\n🗃️  MEMORY CONTEXT:\n");
    serial_printf("   Faulting address: 0x%016llx\n", faulting_address);
    
    if(faulting_address < 0x1000) {
        serial_puts("   ❌ NULL pointer dereference area\n");
    } else if(faulting_address >= 0xFFFF800000000000ULL) {
        serial_puts("   🔒 Kernel space memory\n");
        
        // Try to show memory around the fault (if safe)
        serial_puts("   Memory around fault:\n");
        for(int i = -32; i <= 32; i += 8) {
            serial_printf("      [0x%016llx]: ", faulting_address + i);
            if(i == 0) serial_puts("→ ");
            serial_puts("FAULT HERE\n");
            break; // Don't actually try to read the faulting memory
        }
    } else {
        serial_puts("   👤 User space memory\n");
    }
    
    // Enhanced stack trace
    serial_puts("\n🔍 STACK TRACE ANALYSIS (with function names):\n");
    print_stacktrace(r->rbp);
    serial_puts("\n");
    // If Lua is running, print last Lua location
#ifdef ENABLE_LUA
    serial_puts("\n🧠 LUA CONTEXT:\n");
    luade_print_last_lua_location();
#endif
    
    serial_puts("\n");
    serial_puts("╔══════════════════════════════════════════════════════════════════════════════════════╗\n");
    serial_puts("║                                 END PAGE FAULT                                       ║\n");
    serial_puts("╚══════════════════════════════════════════════════════════════════════════════════════╝\n");
    serial_puts("\n");
    return false;
}

// Enhanced handler for General Protection Faults and other exceptions
static void handle_general_protection_fault(registers_t* r) {
    DEBUG_ENTER_ARGS("error_code=0x%llx, rip=0x%llx", r->err_code, r->rip);

    serial_puts("\n");
    serial_puts("╔══════════════════════════════════════════════════════════════════════════════════════╗\n");
    serial_puts("║                             GENERAL PROTECTION FAULT                                 ║\n");
    serial_puts("╠══════════════════════════════════════════════════════════════════════════════════════╣\n");
    serial_printf("║ Error Code: 0x%016llx                                                           ║\n", r->err_code);
    serial_printf("║ Instruction: 0x%016llx                                                           ║\n", r->rip);
    serial_puts("╚══════════════════════════════════════════════════════════════════════════════════════╝\n");

    // Error code breakdown for GPF: bits 0..2 indicate which table; selector index provided
    serial_puts("\n🔍 ERROR CODE BREAKDOWN (GPF):\n");
    serial_printf("   TI: %s\n", (r->err_code & (1ULL << 2)) ? "LDT" : "GDT");
    serial_printf("   External event: %s\n", (r->err_code & (1ULL << 0)) ? "Yes" : "No");
    serial_printf("   Selector index: 0x%llx\n", (r->err_code >> 3));

    // Registers
    serial_puts("\n📊 REGISTER STATE:\n");
    serial_printf("   RAX: 0x%016llx   RBX: 0x%016llx\n", r->rax, r->rbx);
    serial_printf("   RCX: 0x%016llx   RDX: 0x%016llx\n", r->rcx, r->rdx);
    serial_printf("   RSI: 0x%016llx   RDI: 0x%016llx\n", r->rsi, r->rdi);
    serial_printf("   RBP: 0x%016llx   RSP: 0x%016llx\n", r->rbp, r->rsp);
    serial_printf("   R8:  0x%016llx   R9:  0x%016llx\n", r->r8, r->r9);
    serial_printf("   R10: 0x%016llx   R11: 0x%016llx\n", r->r10, r->r11);
    serial_printf("   R12: 0x%016llx   R13: 0x%016llx\n", r->r12, r->r13);
    serial_printf("   R14: 0x%016llx   R15: 0x%016llx\n", r->r14, r->r15);
    serial_printf("   CS:  0x%016llx   SS:  0x%016llx\n", r->cs, r->ss);
    serial_printf("   RFLAGS: 0x%016llx\n", r->rflags);

    serial_puts("\n🔍 STACK TRACE ANALYSIS (with function names):\n");
    print_stacktrace(r->rbp);
    serial_puts("\n");
#ifdef ENABLE_LUA
    serial_puts("\n🧠 LUA CONTEXT:\n");
    luade_print_last_lua_location();
#endif
    serial_puts("\n");
    serial_puts("╔══════════════════════════════════════════════════════════════════════════════════════╗\n");
    serial_puts("║                             END GENERAL PROTECTION FAULT                             ║\n");
    serial_puts("╚══════════════════════════════════════════════════════════════════════════════════════╝\n");
    serial_puts("\n");
}

// Dedicated Double Fault handler -- runs on IST1 (known-good stack).
// A double fault means the CPU failed to invoke the primary exception handler,
// typically due to stack corruption.  This handler should NEVER return.
static void handle_double_fault(registers_t* r) {
    serial_puts("\n");
    serial_puts("╔══════════════════════════════════════════════════════════════════════════════════════╗\n");
    serial_puts("║                              DOUBLE FAULT (#DF)                                      ║\n");
    serial_puts("║  A double fault means the CPU could not deliver the original exception.              ║\n");
    serial_puts("║  This is typically caused by kernel stack corruption or an invalid IDT entry.         ║\n");
    serial_puts("╠══════════════════════════════════════════════════════════════════════════════════════╣\n");
    serial_printf("║ Error Code: 0x%016llx (always 0 for #DF)                                          ║\n", r->err_code);
    serial_printf("║ RIP:        0x%016llx                                                              ║\n", r->rip);
    serial_printf("║ RSP:        0x%016llx                                                              ║\n", r->rsp);
    serial_printf("║ RBP:        0x%016llx                                                              ║\n", r->rbp);
    serial_puts("╚══════════════════════════════════════════════════════════════════════════════════════╝\n");

    serial_puts("\n REGISTER STATE:\n");
    serial_printf("   RAX: 0x%016llx   RBX: 0x%016llx\n", r->rax, r->rbx);
    serial_printf("   RCX: 0x%016llx   RDX: 0x%016llx\n", r->rcx, r->rdx);
    serial_printf("   RSI: 0x%016llx   RDI: 0x%016llx\n", r->rsi, r->rdi);
    serial_printf("   R8:  0x%016llx   R9:  0x%016llx\n", r->r8, r->r9);
    serial_printf("   R10: 0x%016llx   R11: 0x%016llx\n", r->r10, r->r11);
    serial_printf("   R12: 0x%016llx   R13: 0x%016llx\n", r->r12, r->r13);
    serial_printf("   R14: 0x%016llx   R15: 0x%016llx\n", r->r14, r->r15);
    serial_printf("   CS:  0x%016llx   SS:  0x%016llx\n", r->cs, r->ss);
    serial_printf("   RFLAGS: 0x%016llx\n", r->rflags);

    serial_puts("\n STACK TRACE:\n");
    print_stacktrace(r->rbp);

    serial_puts("\n DOUBLE FAULT IS UNRECOVERABLE -- HALTING CPU\n\n");

    // Halt forever -- a double fault cannot be recovered
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

void isr_handler(registers_t* r) {
    // CRITICAL FIX: Track interrupt context to prevent flanterm corruption
    enter_interrupt_context();
    
    if(r->int_no < 32) {
        // Double fault (#8) has its own dedicated handler via IST
        if (r->int_no == 8) {
            handle_double_fault(r);
            // Never returns
        }

        /* Demand paging / COW / SMAP audit fixed the page fault: resume */
        if (r->int_no == 14 && handle_page_fault(r)) {
            exit_interrupt_context();
            return;
        }

        printf("%s\n", exception_messages[r->int_no]);

        /* Print detailed diagnostics for known exceptions */
        if (r->int_no == 13) {
            handle_general_protection_fault(r);
        } else if (r->int_no != 14) {
            printf("Exception #%llu. Error code: %lld, RIP: 0x%llx\n",
                   r->int_no, r->err_code, r->rip);
        }
        
        /* Determine if the fault came from user mode (CPL != 0) */
        bool from_usermode = (r->cs & 3) != 0;

        /* A program started by exec_file runs on the shell's thread: unwind
         * back into exec_file with a signal-style status instead. */
        if (from_usermode && exec_current_is_sync_program()) {
            int sig = (r->int_no == 14 || r->int_no == 13) ? 11 /* SIGSEGV */
                    : (r->int_no == 0 || r->int_no == 16 || r->int_no == 19) ? 8 /* SIGFPE */
                    : (r->int_no == 6) ? 4 /* SIGILL */ : 11;
            serial_printf("[exception] User-mode fault #%llu at RIP 0x%016llx <%s> -- terminating program (signal %d)\n",
                          r->int_no, r->rip, exec_lookup_symbol(exec_user_as_process(), r->rip), sig);
            printf("Program terminated: %s (signal %d)\n", exception_messages[r->int_no], sig);
            exit_interrupt_context();
            exec_force_terminate(128 + sig);
        }

        /* For unrecoverable user #13 / #14: kill thread instead of halting kernel */
        if (from_usermode && (r->int_no == 13 || r->int_no == 14)) {
            serial_printf("[exception] User-mode fault #%llu at RIP 0x%016llx <%s> -- killing thread\n",
                          r->int_no, r->rip, exec_lookup_symbol(exec_user_as_process(), r->rip));

            /* Close pipe ends / wake waiters before dropping the thread.
             * process_fault_kill_current() does not return for concurrent
             * fork children (hlt loop after zombie). EOI first so the
             * LAPIC is not left in-service across the hlt. */
            {
                extern void process_fault_kill_current(int signum);
                int sig = 11; /* SIGSEGV */
                thread_t *cth = global_scheduler.current_thread;
                process_t *op = cth && cth->owner_process
                    ? (process_t *)cth->owner_process
                    : exec_user_as_process();
                if (op && op->is_usermode && op->sched_thread &&
                    op->parent && op->parent != NULL) {
                    /* Likely concurrent fork child — never returns. */
                    set_current_process(op);
                    exit_interrupt_context();
                    apic_eoi();
                    process_fault_kill_current(sig);
                    __builtin_unreachable();
                }
                if (op && op->is_usermode) {
                    set_current_process(op);
                    /* Sync program: close fds / mark zombie, then fall
                     * through to thread switch below. */
                    process_fault_kill_current(sig);
                }
            }

            thread_t *cur = global_scheduler.current_thread;
            if (cur) {
                cur->state = THREAD_TERMINATED;
                scheduler_remove_thread(cur);

                thread_t *next = scheduler_get_next_thread();

                /* If the ready queue was empty, fall back to the BSP
                 * idle thread (the kernel main loop). */
                if (!next) {
                    per_cpu_t *cpu = smp_get_current_cpu();
                    if (cpu && cpu->idle_thread)
                        next = cpu->idle_thread;
                }

                if (next) {
                    scheduler_log_switch(cur, next, "fault-kill",
                                         next->saved_regs.rip);

                    if (smp_get_current_cpu())
                        smp_get_current_cpu()->current_thread = next;
                    global_scheduler.current_thread = next;
                    next->state = THREAD_RUNNING;
                    sched_oncpu_set(next);

                    r->rdi = next->saved_regs.rdi;
                    r->rsi = next->saved_regs.rsi;
                    r->rbp = next->saved_regs.rbp;
                    r->rbx = next->saved_regs.rbx;
                    r->rdx = next->saved_regs.rdx;
                    r->rcx = next->saved_regs.rcx;
                    r->rax = next->saved_regs.rax;
                    r->r8  = next->saved_regs.r8;
                    r->r9  = next->saved_regs.r9;
                    r->r10 = next->saved_regs.r10;
                    r->r11 = next->saved_regs.r11;
                    r->r12 = next->saved_regs.r12;
                    r->r13 = next->saved_regs.r13;
                    r->r14 = next->saved_regs.r14;
                    r->r15 = next->saved_regs.r15;
                    r->rip    = next->saved_regs.rip;
                    r->cs     = next->saved_regs.cs;
                    r->rflags = next->saved_regs.rflags;
                    r->rsp    = next->saved_regs.rsp;
                    r->ss     = next->saved_regs.ss;

                    fpu_restore(next->fxsave_area);
                    thread_wrfsbase(next->fs_base);

                    /* Restore kernel PML4 if switching to a kernel thread */
                    if (!next->owner_process)
                        vmm_switch_address_space(vmm_get_kernel_pml4());

                    /* Do not kfree cur here: this handler may still be on a
                     * stack it owns.  Clear on_cpu after we leave and let the
                     * reaper free it. */
                    sched_oncpu_defer_clear(cur);
                    thread_mark_dead(cur, NULL);

                    serial_printf("[exception] Killed thread, resumed '%s'\n",
                                  next->name);

                    exit_interrupt_context();
                    apic_eoi();
                    return;
                }
            }
            serial_puts("[exception] No surviving threads -- halting\n");
        }
        
        // Kernel-mode fault or no thread to switch to: unrecoverable
        __asm__ volatile ("cli");
        __asm__ volatile ("hlt");
    }
    if(r->int_no == 32){
        apic_timer();
        tty_timer_hook(r);
        profiler_sample_from_isr(r);
        preemptive_schedule(r);
    }
    if(r->int_no == 35){
        handle_timer(r);
    }
    if(r->int_no == 33){
        keyboard_handler();
    }
    if(r->int_no == 43){
        rtl8139_handle_interrupt();
    }
    if(r->int_no == 44){
        e1000_handle_interrupt();
    }
    if(r->int_no == 45){
        mouse_handler();  // PS/2 mouse on vector 45, IRQ 12 (memory allocator now interrupt-safe)
    }
    if(r->int_no == 46){
        extern void rtl810x_handle_interrupt();
        rtl810x_handle_interrupt();
    }
    if(r->int_no == 48){
        extern void ahci_handle_interrupt(void);
        ahci_handle_interrupt();
    }

    /* ---- SMP IPI handlers ---- */
    if (r->int_no == 253) {
        /* IPI_VECTOR_RESCHEDULE: skip the quantum so a remote enqueue
         * is picked up on this interrupt, not 10 ms later. */
        preemptive_schedule_force();
        preemptive_schedule(r);
        apic_eoi();
        exit_interrupt_context();
        return;
    }
    if (r->int_no == 254) {
        /* IPI_VECTOR_TLB_FLUSH: flush this CPU's TLB and ACK the current
         * shootdown generation (smp_tlb_shootdown_ack does both, reading the
         * generation before the CR3 reload). */
        extern void smp_tlb_shootdown_ack(void);
        smp_tlb_shootdown_ack();
        apic_eoi();
        exit_interrupt_context();
        return;
    }
    if (r->int_no == 255) {
        /* IPI_VECTOR_PANIC: Halt this CPU immediately */
        apic_eoi();
        __asm__ volatile("cli");
        while (1) { __asm__ volatile("hlt"); }
    }

    /* Safety net: ensure LAPIC EOI is sent for all external hardware interrupts.
     * Individual handlers (RTL8139, AHCI, etc.) may already send EOI themselves,
     * but double-EOI is harmless. Missing EOI blocks all further interrupts.
     * Vectors 32 (LAPIC timer) and 35 (HPET) already send EOI in their handlers. */
    if (r->int_no >= 32 && r->int_no != 32 && r->int_no != 35) {
        apic_eoi();
    }

    // CRITICAL FIX: Exit interrupt context tracking
    exit_interrupt_context();
}

// ============================================================================
// Preemptive scheduler - saves/restores thread context via interrupt frame
// ============================================================================

/* LAPIC timer ticks (~500 us) per quantum. 4 ≈ 2 ms. */
#define PREEMPT_TICKS 4

/* Per-CPU tick counter.  A single global made the quantum shared between CPUs:
 * two CPUs ticking into the same counter halve the effective time slice. */
static uint64_t preempt_counter[MAX_CPUS];
static volatile uint8_t preempt_force[MAX_CPUS];
volatile uint32_t preempt_count_cpu[PREEMPT_MAX_CPUS];

static void preemptive_schedule_force(void) {
    per_cpu_t *me = smp_get_current_cpu();
    uint32_t cpu_idx = (me && me->cpu_id < MAX_CPUS) ? me->cpu_id : 0;
    preempt_force[cpu_idx] = 1;
}

/* Public: ask for an immediate preempt on the next timer tick (e.g. after fork). */
void preempt_nudge(void) {
    preemptive_schedule_force();
}

/**
 * Called from handle_timer() with a pointer to the interrupt frame.
 * If the cooperative thread scheduler is running and the time slice
 * has expired, we save the current thread's FULL state (all GP registers
 * + FPU/SSE) and restore the next thread's state so that iretq resumes
 * the new thread transparently.
 *
 * SMP-aware: uses per-CPU current_thread instead of global scheduler's.
 */
void preemptive_schedule(registers_t *r) {
    if (!global_scheduler.scheduler_running) { return; }
    /* We are on the current thread's stack: the thread this CPU switched
     * out last is fully off-CPU now. */
    sched_oncpu_flush();
    /* Grace-period mark for the dead-thread reaper, then free what is safe. */
    thread_reaper_tick();

    per_cpu_t *me = smp_get_current_cpu();
    uint32_t cpu_idx = (me && me->cpu_id < MAX_CPUS) ? me->cpu_id : 0;
    if (cpu_idx < PREEMPT_MAX_CPUS && preempt_count_cpu[cpu_idx])
        return;
    bool force = preempt_force[cpu_idx] != 0;
    preempt_force[cpu_idx] = 0;
    if (!force) {
        preempt_counter[cpu_idx]++;
        if (preempt_counter[cpu_idx] < PREEMPT_TICKS) return;
    }
    preempt_counter[cpu_idx] = 0;

    /* Update per-CPU statistics */
    smp_timer_tick();

    /* One CPU walks the sleep queue; woken threads IPI their home core. */
    if (cpu_idx == 0)
        scheduler_wake_expired(hpet_get_ticks());

    /* Get this CPU's current thread */
    per_cpu_t *cpu = smp_get_current_cpu();
    thread_t *cur = cpu ? cpu->current_thread : global_scheduler.current_thread;

    /* Skip preemption only for user threads that are mid-syscall (ring-0
     * CS on a thread that owns a process). Do NOT apply this to kernel-main
     * / idle — that is how Ring-3 apps first get scheduled; blocking that
     * made spawned Doom never appear. */
    if (cur && cur->owner_process && (r->cs & 3) == 0 && !cur->kwait)
        return;

    /* Fast path: nothing in any ready queue and we're already the idle
     * thread (i.e. the kernel main loop) — nothing to switch to. */
    if (!scheduler_has_ready_threads()) {
        if (!cpu || !cur || cur == cpu->idle_thread)
            return;
    }

    /* Long-mode iretq always pops RSP/SS, so a saved kernel interrupt
     * frame is a valid resume target. Prefer Ring-3 threads; if none
     * are ready, dispatch a kernel thread that has a saved frame. */
    thread_t *next = scheduler_pick_preempt_thread();

    /* Spawned Ring-3 processes exit via exec_spawned_exit() and halt on a
     * TERMINATED thread. With no other user threads ready the timer ISR
     * would return to that dead context forever, starving the kernel-main
     * loop (no keyboard echo, no shell). Resume the saved BSP boot context. */
    /* Only on the BSP: an AP must never "resume" the BSP boot thread (it may
     * be live on the BSP); APs fall back to their own idle thread. */
    if (!next && cur && cur->state == THREAD_TERMINATED) {
        thread_t *kmain = smp_get_bsp_boot_thread();
        if (cpu && cpu->is_bsp && kmain && kmain != cur && kmain->has_saved_context)
            next = kmain;
        else if (cpu && cpu->idle_thread && cpu->idle_thread != cur)
            next = cpu->idle_thread;
    }

    if (!next) return;
    if (next == cur) {
        /* Shouldn't happen (RUNNING threads are never queued), but if it
         * does, just keep running it — it's already dequeued and current. */
        next->state = THREAD_RUNNING;
        return;
    }

    profiler_sched_switch(cur, next);
    scheduler_log_switch(cur, next, "preempt", next->saved_regs.rip);

    // --- Save current thread's FULL state from the interrupt frame ---
    if (cur) {
        // Save all GP registers from the interrupt frame
        cur->saved_regs.rdi = r->rdi;
        cur->saved_regs.rsi = r->rsi;
        cur->saved_regs.rbp = r->rbp;
        cur->saved_regs.rbx = r->rbx;
        cur->saved_regs.rdx = r->rdx;
        cur->saved_regs.rcx = r->rcx;
        cur->saved_regs.rax = r->rax;
        cur->saved_regs.r8  = r->r8;
        cur->saved_regs.r9  = r->r9;
        cur->saved_regs.r10 = r->r10;
        cur->saved_regs.r11 = r->r11;
        cur->saved_regs.r12 = r->r12;
        cur->saved_regs.r13 = r->r13;
        cur->saved_regs.r14 = r->r14;
        cur->saved_regs.r15 = r->r15;
        // Save iretq frame fields
        cur->saved_regs.rip    = r->rip;
        cur->saved_regs.cs     = r->cs;
        cur->saved_regs.rflags = r->rflags;
        cur->saved_regs.rsp    = r->rsp;
        cur->saved_regs.ss     = r->ss;
        // Keep legacy fields in sync
        cur->rsp = r->rsp;
        cur->rip = r->rip;

        // Save FPU/SSE state
        fpu_save(cur->fxsave_area);
        // Save FS base (user TLS)
        cur->fs_base = thread_rdfsbase();

        cur->has_saved_context = true;
        /* Frame saved; clear on_cpu after we leave this stack. */
        sched_oncpu_defer_clear(cur);

        // Put current thread back on the ready queue (unless it's the idle thread)
        if (cur->state == THREAD_RUNNING) {
            cur->state = THREAD_READY;
            /* Don't re-queue idle threads — they're always available as fallback */
            if (!cpu || cur != cpu->idle_thread) {
                scheduler_add_thread(cur);
            }
        }
    }

    /* AP idle threads are stateless hlt loops: always start them fresh on
     * their own stack, never from a saved frame (see thread_reset_start_context). */
    if (cpu && !cpu->is_bsp && next == cpu->idle_thread)
        thread_reset_start_context(next);

    // --- Restore next thread's FULL state into the interrupt frame ---
    /* Update both per-CPU and global current_thread */
    if (cpu) {
        cpu->current_thread = next;
        cpu->context_switches++;
    }
    global_scheduler.current_thread = next;
    next->state = THREAD_RUNNING;
    sched_oncpu_set(next);

    // Restore all GP registers into the interrupt frame
    r->rdi = next->saved_regs.rdi;
    r->rsi = next->saved_regs.rsi;
    r->rbp = next->saved_regs.rbp;
    r->rbx = next->saved_regs.rbx;
    r->rdx = next->saved_regs.rdx;
    r->rcx = next->saved_regs.rcx;
    r->rax = next->saved_regs.rax;
    r->r8  = next->saved_regs.r8;
    r->r9  = next->saved_regs.r9;
    r->r10 = next->saved_regs.r10;
    r->r11 = next->saved_regs.r11;
    r->r12 = next->saved_regs.r12;
    r->r13 = next->saved_regs.r13;
    r->r14 = next->saved_regs.r14;
    r->r15 = next->saved_regs.r15;
    // Restore iretq frame
    r->rip    = next->saved_regs.rip;
    r->cs     = next->saved_regs.cs;
    r->rflags = next->saved_regs.rflags;
    r->rsp    = next->saved_regs.rsp;
    r->ss     = next->saved_regs.ss;

    // Restore FPU/SSE state
    fpu_restore(next->fxsave_area);
    // Restore FS base (user TLS, set by arch_prctl)
    thread_wrfsbase(next->fs_base);

    // --- Update TSS RSP0 and per-CPU kernel stack for Ring 3 support ---
    if (next->owner_process) {
        process_t *proc = (process_t *)next->owner_process;
        if (proc->is_usermode && proc->kernel_stack_top) {
            gdt_set_kernel_stack(proc->kernel_stack_top);
            syscall_set_kernel_stack(proc->kernel_stack_top);
        }
        if (proc->pml4) {
            vmm_switch_address_space(proc->pml4);
        }
        set_current_process(proc);
        exec_set_ring3_active(proc);
    } else {
        /* Kernel-only thread. If its saved frame is still Ring-3 (exec_file
         * / ash running on the BSP boot thread), keep that process's PML4
         * so concurrent fork children can preempt and resume correctly. */
        /* kwait==2: ash's boot thread parked in sys_wait (ring-0 frame) —
         * resume with ash's address space like the ring-3 case. */
        if ((next->saved_regs.cs & 3) == 3 || next->kwait == 2) {
            process_t *up = exec_user_as_process();
            if (up && up->pml4) {
                vmm_switch_address_space(up->pml4);
                if (up->kernel_stack_top) {
                    gdt_set_kernel_stack(up->kernel_stack_top);
                    syscall_set_kernel_stack(up->kernel_stack_top);
                }
                set_current_process(up);
            } else {
                vmm_switch_address_space(vmm_get_kernel_pml4());
            }
        } else {
            vmm_switch_address_space(vmm_get_kernel_pml4());
            exec_set_ring3_active(NULL);
        }
    }
}

// Simple stub for lookup_symbol during first phase build
__attribute__((weak)) const char* lookup_symbol(uint64_t addr) {
    (void)addr;
    return "unknown";
}

/* Check if an address is safe to dereference for stack walking.
 * Avoids recursive page faults when the stack pointer is near an unmapped
 * region (e.g. the top of the user stack at USER_STACK_TOP). */
static inline bool rbp_is_safe(uint64_t addr) {
    if (addr == 0) return false;
    if (addr & 7) return false;                     /* must be 8-byte aligned */
    if (addr < 0x1000ULL) return false;             /* null-page guard */
    /* Reject addresses in the non-canonical hole. */
    if (addr >= 0x0000800000000000ULL &&
        addr <  0xffff800000000000ULL) return false;
    /* Fault dumps must not chase user RBP — the stack guard at
     * USER_STACK_TOP is unmapped and re-enters handle_page_fault. */
    if (addr < 0xffff800000000000ULL) return false;
    if (addr + 8 < addr) return false;
    return true;
}

void print_stacktrace(uint64_t rbp) {
    process_t *uproc = exec_user_as_process();
    serial_puts("Stack trace:\n");
    for (int i = 0; i < 16; ++i) {
        if (!rbp_is_safe(rbp) || !rbp_is_safe(rbp + 8)) break;
        uint64_t ret_addr = *((uint64_t *)(rbp + 8));
        serial_printf("  [%d] 0x%016llx <%s>\n", i, ret_addr,
                      exec_lookup_symbol(uproc, ret_addr));
        rbp = *((uint64_t *)rbp);
        if (ret_addr == 0 || rbp == 0) break;
    }
}