#include "ktime.h"
#include "hpet.h"
#include "thread.h"
#include "serial.h"

uint64_t ktime_now_ms(void) {
    /* hpet_get_ticks() already returns milliseconds (main counter divided by
     * hpet_freq/1000). The old code multiplied by the periodic interval too,
     * scaling every timeout/deadline by ~10x. */
    return hpet_get_ticks();
}

uint64_t ktime_now_us(void) {
    return hpet_get_time_us();
}

uint64_t ktime_deadline_ms(uint64_t ms_from_now) {
    return ktime_now_ms() + ms_from_now;
}

bool ktime_expired(uint64_t deadline_ms) {
    return ktime_now_ms() >= deadline_ms;
}

void ktime_busywait_ms(uint64_t ms) {
    uint64_t deadline = ktime_deadline_ms(ms);
    while (!ktime_expired(deadline))
        __asm__ volatile("pause");
}

void ktime_yield_until(uint64_t deadline_ms) {
    /* Wait out a syscall delay without thread_sleep().
     *
     * thread_sleep() from a syscall is still unsafe: a blocking exec_file()
     * kernel thread is never re-dispatched, and a spawned Ring-3 thread is
     * resumed from the scheduler's user iret frame, so a kernel-mode
     * continuation after SDL_Delay() corrupts the process.
     *
     * HLT pacing does not hold on the ACPI PM-timer fallback (Hyper-V wakes
     * the halt without the PM counter having moved the requested interval, so
     * Doom runs flat out). When the clock is not a real HPET, busy-wait on
     * that clock. STI so a timer IRQ can still sample it; the iret frame
     * restores user IF. */
    if (!hpet_is_hardware()) {
        while (!ktime_expired(deadline_ms))
            __asm__ volatile("sti; pause" ::: "memory");
        return;
    }
    while (!ktime_expired(deadline_ms))
        __asm__ volatile("sti; hlt" ::: "memory");
}

void ktime_pace_hz(uint32_t hz) {
    static uint64_t next_ms;
    static int logged;
    if (hz == 0)
        return;
    if (!logged) {
        logged = 1;
        serial_printf("[ktime] present cap %u Hz via %s\n",
                      hz, hpet_is_hardware() ? "HPET" : "PM/TSC clock");
    }
    uint64_t period = 1000ull / hz;
    if (period == 0)
        period = 1;
    uint64_t now = ktime_now_ms();
    if (next_ms && now < next_ms) {
        uint64_t deadline = next_ms;
        if (deadline > now + 100)
            deadline = now + 100;
        while (ktime_now_ms() < deadline)
            __asm__ volatile("sti; pause" ::: "memory");
        now = ktime_now_ms();
    }
    next_ms = now + period;
}
