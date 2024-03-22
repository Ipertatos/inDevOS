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
    idt_set_descriptor(0, (uint32_t)isr0, 0x8E);
    idt_set_descriptor(1, (uint32_t)isr1, 0x8E);
    idt_set_descriptor(2, (uint32_t)isr2, 0x8E);
    idt_set_descriptor(3, (uint32_t)isr3, 0x8E);
    idt_set_descriptor(4, (uint32_t)isr4, 0x8E);
    idt_set_descriptor(5, (uint32_t)isr5, 0x8E);
    idt_set_descriptor(6, (uint32_t)isr6, 0x8E);
    idt_set_descriptor(7, (uint32_t)isr7, 0x8E);
    idt_set_descriptor(8, (uint32_t)isr8, 0x8E);
    idt_set_descriptor(9, (uint32_t)isr9, 0x8E);
    idt_set_descriptor(10, (uint32_t)isr10, 0x8E);
    idt_set_descriptor(11, (uint32_t)isr11, 0x8E);
    idt_set_descriptor(12, (uint32_t)isr12, 0x8E);
    idt_set_descriptor(13, (uint32_t)isr13, 0x8E);
    idt_set_descriptor(14, (uint32_t)isr14, 0x8E);
    idt_set_descriptor(15, (uint32_t)isr15, 0x8E);
    idt_set_descriptor(16, (uint32_t)isr16, 0x8E);
    idt_set_descriptor(17, (uint32_t)isr17, 0x8E);
    idt_set_descriptor(18, (uint32_t)isr18, 0x8E);
    idt_set_descriptor(19, (uint32_t)isr19, 0x8E);
    idt_set_descriptor(20, (uint32_t)isr20, 0x8E);
    idt_set_descriptor(21, (uint32_t)isr21, 0x8E);
    idt_set_descriptor(22, (uint32_t)isr22, 0x8E);
    idt_set_descriptor(23, (uint32_t)isr23, 0x8E);
    idt_set_descriptor(24, (uint32_t)isr24, 0x8E);
    idt_set_descriptor(25, (uint32_t)isr25, 0x8E);
    idt_set_descriptor(26, (uint32_t)isr26, 0x8E);
    idt_set_descriptor(27, (uint32_t)isr27, 0x8E);
    idt_set_descriptor(28, (uint32_t)isr28, 0x8E);
    idt_set_descriptor(29, (uint32_t)isr29, 0x8E);
    idt_set_descriptor(30, (uint32_t)isr30, 0x8E);
    idt_set_descriptor(31, (uint32_t)isr31, 0x8E);
    
    idt_set_descriptor(33, (uint32_t)keyboard_handler, 0x8E);
}

void isr0()
{
    printk("Division by zero\n");
    __asm__ volatile ("cli; hlt");
}

void isr1()
{
    printk("Debug\n");
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

