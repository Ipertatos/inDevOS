#pragma once

#include "types.h"
#include "registers.h"
#include "utils.h"

void onIrq0(registers_t *r);

void initTimer();