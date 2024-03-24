#include "isr.h"
#include "keyboard.h"

void NMI_enable()
{
    outb(0x70, inb(0x70) & 0x7F);
    inb(0x71);
}

void NMI_disable()
{
    outb(0x70, inb(0x70) | 0x80);
    inb(0x71);
}

void isr_install()
{
    //idt_set_descriptor(0, (uint32_t)isr0, 0x8E); to 32
    idt_set_descriptor(0, isr0, 0x8E);
    idt_set_descriptor(1, isr1, 0x8E);
    idt_set_descriptor(2, isr2, 0x8E);
    idt_set_descriptor(3, isr3, 0x8E);
    idt_set_descriptor(4, isr4, 0x8E);
    idt_set_descriptor(5, isr5, 0x8E);
    idt_set_descriptor(6, isr6, 0x8E);
    idt_set_descriptor(7, isr7, 0x8E);
    idt_set_descriptor(8, isr8, 0x8E);
    idt_set_descriptor(9, isr9, 0x8E);
    idt_set_descriptor(10, isr10, 0x8E);
    idt_set_descriptor(11, isr11, 0x8E);
    idt_set_descriptor(12, isr12, 0x8E);
    idt_set_descriptor(13, isr13, 0x8E);
    idt_set_descriptor(14, isr14, 0x8E);
    idt_set_descriptor(15, isr15, 0x8E);
    idt_set_descriptor(16, isr16, 0x8E);
    idt_set_descriptor(17, isr17, 0x8E);
    idt_set_descriptor(18, isr18, 0x8E);
    idt_set_descriptor(19, isr19, 0x8E);
    idt_set_descriptor(20, isr20, 0x8E);
    idt_set_descriptor(21, isr21, 0x8E);
    idt_set_descriptor(22, isr22, 0x8E);
    idt_set_descriptor(23, isr23, 0x8E);
    idt_set_descriptor(24, isr24, 0x8E);
    idt_set_descriptor(25, isr25, 0x8E);
    idt_set_descriptor(26, isr26, 0x8E);
    idt_set_descriptor(27, isr27, 0x8E);
    idt_set_descriptor(28, isr28, 0x8E);
    idt_set_descriptor(29, isr29, 0x8E);
    idt_set_descriptor(30, isr30, 0x8E);
    idt_set_descriptor(31, isr31, 0x8E);
    
    idt_set_descriptor(33, keyboard_handler, 0x8E);
}

void isr0()
{
    printk("Division by zero\n");
    __asm__ volatile ("cli; hlt");
}

void isr1()
{
    printk("Debug\n");
    stack_dump();
    __asm__ volatile ("cli; hlt");
}

void isr2()
{
    printk("Non-maskable interrupt\n");
    __asm__ volatile ("cli; hlt");
}

void isr3()
{
    printk("Breakpoint\n");
    __asm__ volatile ("cli; hlt");
}

//do it for all of them
void isr4()
{
    printk("Overflow\n");
    __asm__ volatile ("cli; hlt");
}

void isr5()
{
    printk("Bounds\n");
    __asm__ volatile ("cli; hlt");
}

void isr6()
{
    printk("Invalid opcode\n");
    __asm__ volatile ("cli; hlt");
}

void isr7()
{
    printk("Device not available\n");
    __asm__ volatile ("cli; hlt");
}

void isr8()
{
    printk("Double fault\n");
    stack_dump();
    __asm__ volatile ("cli; hlt");
}

void isr9()
{
    printk("Coprocessor segment overrun\n");
    __asm__ volatile ("cli; hlt");
}

void isr10()
{
    printk("Invalid TSS\n");
    __asm__ volatile ("cli; hlt");
}

void isr11()
{
    printk("Segment not present\n");
    __asm__ volatile ("cli; hlt");
}

void isr12()
{
    printk("Stack-segment fault\n");
    __asm__ volatile ("cli; hlt");
}

void isr13()
{
    printk("General protection fault\n");
    __asm__ volatile ("cli; hlt");
}

void isr14()
{
    printk("Page fault\n");
    __asm__ volatile ("cli; hlt");
}

void isr15()
{
    printk("Unknown interrupt\n");
    __asm__ volatile ("cli; hlt");
}

void isr16()
{
    printk("Coprocessor fault\n");
    __asm__ volatile ("cli; hlt");
}

void isr17()
{
    printk("Alignment check\n");
    __asm__ volatile ("cli; hlt");
}

void isr18()
{
    printk("Machine check\n");
    __asm__ volatile ("cli; hlt");
}

void isr19()
{
    printk("SIMD floating-point exception\n");
    __asm__ volatile ("cli; hlt");
}

void isr20()
{
    printk("Virtualization exception\n");
    __asm__ volatile ("cli; hlt");
}

void isr21()
{
    printk("Control protection exception\n");
    __asm__ volatile ("cli; hlt");
}

void isr22()
{
    printk("Reserved\n");
    __asm__ volatile ("cli; hlt");
}

void isr23()
{
    printk("Reserved\n");
    __asm__ volatile ("cli; hlt");
}

void isr24()
{
    printk("Reserved\n");
    __asm__ volatile ("cli; hlt");
}

void isr25()
{
    printk("Reserved\n");
    __asm__ volatile ("cli; hlt");
}

void isr26()
{
    printk("Reserved\n");
    __asm__ volatile ("cli; hlt");
}

void isr27()
{
    printk("Reserved\n");
    __asm__ volatile ("cli; hlt");
}

void isr28()
{
    printk("Reserved\n");
    __asm__ volatile ("cli; hlt");
}

void isr29()
{
    printk("Reserved\n");
    __asm__ volatile ("cli; hlt");
}

void isr30()
{
    printk("Reserved\n");
    __asm__ volatile ("cli; hlt");
}

void isr31()
{
    printk("Reserved\n");
    __asm__ volatile ("cli; hlt");
}

