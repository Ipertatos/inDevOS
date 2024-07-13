#pragma once

#include "types.h"

volatile uint64_t _ticks;

void hpet_init();
void hpet_enable();
void hpet_disable();
void hpet_reset();
void hpet_ack();
void hpet_isr();
void hpet_sleep(uint64_t ms);
void hpet_sleep_counter(uint64_t ms);
uint64_t hpet_get_ticks();