#pragma once 
#include "types.h"

void write_reg(uint32_t reg, uint32_t value);
uint32_t read_reg(uint32_t reg);


uint8_t inb(uint16_t port);
void outb(uint16_t port, uint8_t value);