#ifndef keyboard_h
#define keyboard_h

#include "types.h"
#include "registers.h"
#include "pic.h"

int keyboard_init();
void keyboard_handler();

#endif