#include "idt.h"
#include "hpet.h"
#include "apic.h"
#include "utils.h"
#include "keyboard.h"
#include "atomic.h"
#include "debug.h"
#include "serial.h"
#include "term.h"
#include "kernel_symtab.h"
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
void initIDT() {
    idtr.limit = (uint16_t)sizeof(idt_entry_t) * 256 - 1;
    idtr.base = (uint64_t)&idt_entries[0];

    setIdtGate(0, isr0, 0x8E);
    setIdtGate(1, isr1, 0x8E);
    setIdtGate(2, isr2, 0x8E);
    setIdtGate(3, isr3, 0x8E);
    setIdtGate(4, isr4, 0x8E);
    setIdtGate(5, isr5, 0x8E);
    setIdtGate(6, isr6, 0x8E);
    setIdtGate(7, isr7, 0x8E);
    setIdtGate(8, isr8, 0x8E);
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

    __asm__ __volatile__("lidtq %0" : : "m"(idtr));
    __asm__ __volatile__("sti");

}

void setIdtGate(uint8_t num, void* base, uint8_t flags) {
    idt_entry_t* desc = &idt_entries[num];
    
    desc->isr_low        = (uint64_t)base & 0xFFFF;
    desc->kernel_cs      = 0x08;
    desc->ist            = 0;
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

void handle_timer(){
    hpet_isr();
    fb_cursor_blink_tick();
    hpet_ack();
    apic_eoi();
}

// Enhanced page fault handler with improved diagnostics
void handle_page_fault(registers_t* r) {
    DEBUG_ENTER_ARGS("error_code=0x%llx, rip=0x%llx", r->err_code, r->rip);
    uint64_t faulting_address;
    
    // Read CR2 register to get the faulting address
    __asm__ volatile("mov %%cr2, %0" : "=r" (faulting_address));
    DEBUG_MSG("Page fault at address: 0x%llx", faulting_address);
    
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
    }
    
    // Enhanced register state with better formatting
    serial_puts("\n📊 REGISTER STATE:\n");
    serial_printf("   RAX: 0x%016llx   RBX: 0x%016llx\n", r->rax, r->rbx);
    serial_printf("   RCX: 0x%016llx   RDX: 0x%016llx\n", r->rcx, r->rdx);
    serial_printf("   RSI: 0x%016llx   RDI: 0x%016llx\n", r->rsi, r->rdi);
    serial_printf("   RBP: 0x%016llx   RSP: 0x%016llx\n", r->rbp, (uint64_t)r + sizeof(registers_t));
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
    uint64_t current_rsp = (uint64_t)r + sizeof(registers_t);
    serial_printf("   Current RSP: 0x%016llx\n", current_rsp);
    
    // Check stack validity
    if(current_rsp < 0x1000) {
        serial_puts("   ❌ Invalid stack pointer: NULL or very low\n");
    } else if(current_rsp >= 0xFFFF800000000000ULL) {
        serial_puts("   ✅ Kernel stack\n");
    } else {
        serial_puts("   👤 User stack\n");
    }
    
    // Stack dump with better formatting
    serial_puts("   Stack contents (first 128 bytes):\n");
    uint64_t* stack_ptr = (uint64_t*)current_rsp;
    for(int i = 0; i < 16; i++) {
        serial_printf("      [RSP+%d]: 0x%016llx", i*8, stack_ptr[i]);
        
        // Try to identify if this looks like a return address
        if(stack_ptr[i] >= 0xFFFF800000000000ULL && stack_ptr[i] < 0xFFFF800001000000ULL) {
            serial_puts("  ← Possible return address");
        }
        
        // Check for NULL pointers
        if(stack_ptr[i] == 0) {
            serial_puts("  ← NULL");
        }
        
        serial_puts("\n");
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
    
    serial_puts("\n");
    serial_puts("╔══════════════════════════════════════════════════════════════════════════════════════╗\n");
    serial_puts("║                                 END PAGE FAULT                                       ║\n");
    serial_puts("╚══════════════════════════════════════════════════════════════════════════════════════╝\n");
    serial_puts("\n");
}

void isr_handler(registers_t* r) {
    // CRITICAL FIX: Track interrupt context to prevent flanterm corruption
    enter_interrupt_context();
    
    if(r->int_no < 32) {
        printf("%s\n", exception_messages[r->int_no]);
        
        // Special handling for page fault
        if(r->int_no == 14) {
            handle_page_fault(r);
        } else {
            printf("Exception. System Halted\n");
            printf("Error code: %lld\n", r->err_code);
            printf("RIP: 0x%llx\n", r->rip);
        }
        
        __asm__ volatile ("cli");
        __asm__ volatile ("hlt");
    }
    if(r->int_no == 32){
        apic_timer();  // LAPIC timer on vector 32
    }
    if(r->int_no == 35){
        handle_timer(); // HPET timer on vector 35
    }
    if(r->int_no == 33){
        keyboard_handler();
    }
    
    // CRITICAL FIX: Exit interrupt context tracking
    exit_interrupt_context();
}

// Simple stub for lookup_symbol during first phase build
__attribute__((weak)) const char* lookup_symbol(uint64_t addr) {
    (void)addr;
    return "unknown";
}

void print_stacktrace(uint64_t rbp) {
    serial_puts("Stack trace:\n");
    for (int i = 0; i < 16 && rbp; ++i) {
        uint64_t ret_addr = *((uint64_t *)(rbp + 8));
        serial_printf("  [%d] 0x%016llx <%s>\n", i, ret_addr, lookup_symbol(ret_addr));
        rbp = *((uint64_t *)rbp);
        if (ret_addr == 0 || rbp == 0) break;
    }
}