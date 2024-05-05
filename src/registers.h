#pragma once 
#include "types.h"

uint8_t stack[4096];

void write_reg(uint32_t reg, uint32_t value);
uint32_t read_reg(uint32_t reg);


uint8_t inb(uint16_t port);
void outb(uint16_t port, uint8_t value);

uint16_t inw(uint16_t port);
void outw(uint16_t port, uint16_t value);

uint32_t inl(uint16_t port);
void outl(uint16_t port, uint32_t value);

typedef struct {
    uint64_t rdi, rsi, rbp, rbx, rdx, rcx, rax;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t int_no, err_code;
    uint64_t rip, cs, rflags, ss;
} registers_t;
