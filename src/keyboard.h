#pragma once
#include "types.h"
#include "registers.h"
#include "pic.h"
#include "idt.h"


void keyboard_init();
char ps2_translate2ascii(uint16_t scanncode);
void keyboard_handler();