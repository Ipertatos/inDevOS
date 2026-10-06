#include "registers.h"

// MMIO register access - uses volatile pointer dereference for proper memory-mapped I/O
void write_reg(uint32_t reg, uint32_t value)
{
    *(volatile uint32_t *)(uintptr_t)reg = value;
}

uint32_t read_reg(uint32_t reg)
{
    return *(volatile uint32_t *)(uintptr_t)reg;
}

// I/O port access - "Nd" allows compiler to use immediate form for constant ports 0-255
uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile("inb %1, %0"
                     : "=a"(ret)
                     : "Nd"(port));
    return ret;
}

void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile("outb %0, %1"
                     :
                     : "a"(value), "Nd"(port));
}

uint16_t inw(uint16_t port)
{
    uint16_t ret;
    __asm__ volatile("inw %1, %0"
                     : "=a"(ret)
                     : "Nd"(port));
    return ret;
}

void outw(uint16_t port, uint16_t value)
{
    __asm__ volatile("outw %0, %1"
                     :
                     : "a"(value), "Nd"(port));
}

uint32_t inl(uint16_t port)
{
    uint32_t ret;
    __asm__ volatile("inl %1, %0"
                     : "=a"(ret)
                     : "Nd"(port));
    return ret;
}

void outl(uint16_t port, uint32_t value)
{
    __asm__ volatile("outl %0, %1"
                     :
                     : "a"(value), "Nd"(port));
}



void io_wait(){
    outb(0x80,0);
}