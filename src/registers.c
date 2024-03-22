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