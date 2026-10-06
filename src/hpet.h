#pragma once

#include "types.h"

extern volatile uint64_t *hpet_main_cnt;
extern uint64_t hpet_freq;

// _ticks is now internal and atomic - use hpet_get_ticks() to access

void hpet_init();
/* 1 when the free-running clock is a real HPET, else ACPI PM timer or TSC. */
int hpet_is_hardware(void);
/* Called from the LAPIC timer ISR. Samples the fallback clock so a 24-bit
 * ACPI PM timer cannot wrap unnoticed. No-op when HPET is in use. */
void hpet_keepalive(void);
void hpet_enable();
void hpet_disable();
void hpet_reset();
void hpet_ack();
void hpet_isr();
void hpet_sleep(uint64_t ms);
void hpet_sleep_counter(uint64_t ms);
void hpet_one_shot(uint64_t ms);
void hpet_set_periodic(uint64_t ms);
uint64_t hpet_get_ticks();
/* Microseconds since boot from the raw HPET main counter (finest resolution). */
uint64_t hpet_get_time_us(void);
uint64_t hpet_get_current_interval(void);
uint64_t hpet_get_frequency(void);
void hpet_print_status(void);