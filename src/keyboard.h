#pragma once
#include "types.h"
#include "registers.h"
#include "pic.h"
#include "idt.h"
void keyboard_init();
void keyboard_handler(registers_t *regs);
