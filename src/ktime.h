#ifndef KTIME_H
#define KTIME_H

#include "types.h"

/*
 * Unified kernel timing helpers.
 *
 * hpet_get_ticks() already returns milliseconds (HPET main counter /
 * (freq/1000)). Do not multiply by 1000/frequency again.
 *
 * All deadline/sleep APIs work in milliseconds.
 */

uint64_t ktime_now_ms(void);

/* Microseconds since boot (raw HPET counter; use for profiling / deltas). */
uint64_t ktime_now_us(void);

uint64_t ktime_deadline_ms(uint64_t ms_from_now);

bool ktime_expired(uint64_t deadline_ms);

void ktime_busywait_ms(uint64_t ms);

void ktime_yield_until(uint64_t deadline_ms);

/* Sleep until the next 1/hz slot. Uses the PM timer / TSC when HPET is absent
 * (HLT does not pace Hyper-V's ACPI PM-timer fallback). */
void ktime_pace_hz(uint32_t hz);

#endif /* KTIME_H */
