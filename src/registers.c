#include "registers.h"

void write_reg(uint32_t reg, uint32_t value)
{
    __asm__ volatile("movl %0, %1"
                     :
                     : "r"(value), "m"(reg));
}

uint32_t read_reg(uint32_t reg)
{
    uint32_t ret;
    __asm__ volatile("movl %1, %0"
                     : "=r"(ret)
                     : "m"(reg));
    return ret;
}

uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile("inb %1, %0"
                     : "=a"(ret)
                     : "d"(port));
    return ret;
}

void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile("outb %0, %1"
                     :
                     : "a"(value), "d"(port));
}

uint16_t inw(uint16_t port)
{
    uint16_t ret;
    __asm__ volatile("inw %1, %0"
                     : "=a"(ret)
                     : "d"(port));
    return ret;
}

void outw(uint16_t port, uint16_t value)
{
    __asm__ volatile("outw %0, %1"
                     :
                     : "a"(value), "d"(port));
}

uint32_t inl(uint16_t port)
{
    uint32_t ret;
    __asm__ volatile("inl %1, %0"
                     : "=a"(ret)
                     : "d"(port));
    return ret;
}

void outl(uint16_t port, uint32_t value)
{
    __asm__ volatile("outl %0, %1"
                     :
                     : "a"(value), "d"(port));
}



void io_wait(){
    outb(0x80,0);
}