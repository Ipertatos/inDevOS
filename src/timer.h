#pragma once

#include "types.h"
#include "registers.h"

void onIrq0(registers_t *r);

void initTimer();