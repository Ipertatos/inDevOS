#include "idt.h"
#include "apic.h"
#include "utils.h"
#include "keyboard.h"
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
extern void isr33();
extern void isr172();

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
    setIdtGate(33, isr33, 0x8E);//LAPIC timer
    setIdtGate(172, isr172, 0x8E); //PS/2 keyboard

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

void isr_handler(registers_t* r) {
    if(r->int_no < 32) {
        printf(exception_messages[r->int_no]);
        printf("\n");
        printf("Exception. System Halted\n");
        printf("Error code: ");
        printf(int2string(r->err_code));
        printf("\n");
        __asm__ volatile ("cli");
        __asm__ volatile ("hlt");
    }
    if(r->int_no == 33){
        apic_timer();
    }
    if(r->int_no == 172){
        keyboard_handler();
    }
}