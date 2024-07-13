#pragma once
#include "types.h"
#include "registers.h"
#include "pic.h"
#include "idt.h"
extern char buffer[256];
extern uint8_t buffer_len;
extern bool tobuffer;

extern bool execute;

void keyboard_init();
char ps2_translate2ascii(uint16_t scanncode);
void keyboard_handler();