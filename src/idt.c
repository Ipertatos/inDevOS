#include "idt.h"

idt_entry_t idt_entries[256];
idtr_t idtr;

static bool vectors[256];

extern void idt_flush(uint64_t);

void* isr_table[32] = {
    isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7, isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15,
    isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23, isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31
};

void* irq_table[16] = {
    irq0, irq1, irq2, irq3, irq4, irq5, irq6, irq7, irq8, irq9, irq10, irq11, irq12, irq13, irq14, irq15
};

void initIDT() {
    idtr.limit = (uint16_t)sizeof(idt_entry_t) * 256 - 1;
    idtr.base = (uint64_t)&idt_entries[0];

    memset(&idt_entries, 0, sizeof(idt_entry_t) * 256);

    for(int i = 0; i < 32; i++) {
        setIdtGate(i, isr_table[i], 0x8E);
        vectors[i] = true;
    }

    for(int i = 32; i < 48; i++) {
        setIdtGate(i, irq_table[i - 32], 0x8E);
        vectors[i] = true;
    }

    __asm__ __volatile__("lidtq %0" : : "m"(idtr));
    __asm__ __volatile__("sti");

}

void setIdtGate(uint8_t num, void* base, uint8_t flags) {
    idt_entries[num].isr_low = (uint64_t)base & 0xFFFF;
    idt_entries[num].kernel_cs = GDT_OFFSET_KERNEL_CODE;
    idt_entries[num].ist = 0;
    idt_entries[num].attributes = flags;
    idt_entries[num].isr_mid = ((uint64_t)base >> 16) & 0xFFFF;
    idt_entries[num].isr_high = ((uint64_t)base >> 32) & 0xFFFFFFFF;
    idt_entries[num].reserved = 0;
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
        print(exception_messages[r->int_no]);
        print("\n");
        print("Exception. System Halted\n");
        print("Error code: ");
        print(int2string(r->err_code));
        print("\n");
        for(;;);
    }
}


void *irq_routines[16] = {0};

void irq_install_handler(int irq, void (*handler)(registers_t *r)) {
    irq_routines[irq] = handler;
}

void irq_uninstall_handler(int irq) {
    irq_routines[irq] = 0;
}

void irq_handler(registers_t* r) {
    outb(0x20, 0x20);

    void (*handler)(registers_t *r);
    handler = irq_routines[r->int_no - 32];
    if(handler) {
        handler(r);
    }

    if(r->int_no >= 40) {
        outb(0xA0, 0x20);
    }
    outb(0x20, 0x20);
}