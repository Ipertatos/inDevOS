#pragma once
#include "types.h"
#include "registers.h"
#include "pic.h"
#include "idt.h"

// Keyboard buffer and state are now internal and protected by spinlocks
// Use keyboard_get_buffer() and keyboard_get_execute() to access safely
bool keyboard_get_execute(void);
char* keyboard_get_buffer(void);
uint32_t keyboard_get_buffer_length(void);
void keyboard_clear_execute(void);
void keyboard_clear_buffer(void);

// Safe echo processing functions (call from main thread)
void keyboard_process_echo(void);
bool keyboard_has_echo_pending(void);

void keyboard_init();
char ps2_translate2ascii(uint16_t scanncode);
void keyboard_handler();