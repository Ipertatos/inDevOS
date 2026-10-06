/*
 * profiler.h — kernel trace sampler + events for performance debugging.
 *
 * Uses per-CPU lock-free rings so LAPIC ISR sampling never blocks on the same
 * lock syscall paths might briefly hold.
 */

#pragma once

#include "types.h"
#include "registers.h"
#include "thread.h"

struct SDL_Window;

#define PROF_KIND_SAMPLE    1u
#define PROF_KIND_SC_ENTER  2u
#define PROF_KIND_SC_EXIT   3u
#define PROF_KIND_SCHED     4u
#define PROF_KIND_FRAME     5u

typedef struct prof_rec_t {
    uint64_t ts_us;
    uint8_t  kind;
    uint8_t  cpu;
    uint16_t unused;
    uint32_t pid;
    uint64_t tid;
    uint64_t arg0;
    uint64_t arg1;
    uint64_t arg2;
    uint64_t arg3;
} prof_rec_t;

void profiler_init(void);

void profiler_set_enabled(bool on);
bool profiler_enabled(void);

uint32_t profiler_get_sample_divisor(void);
void profiler_set_sample_divisor(uint32_t div);

/** Monotonic microseconds from HPET (fallback: ms ticks * 1000). */
uint64_t profiler_now_us(void);

void profiler_reset_buffers(void);

void profiler_dump_ascii(void);

/** Aggregate ISR samples into folded stacks and print a text flame / top list. */
void profiler_dump_flame(void);

/** Aggregate syscall exit durations (total / count / max). */
void profiler_dump_syscalls(void);

void profiler_print_stats(void);

void profiler_set_overlay(bool on);
bool profiler_overlay_enabled(void);
void profiler_on_sdl_present(struct SDL_Window *window);

void profiler_cli(int argc, char **argv);

/* Hooks */
void profiler_sample_from_isr(registers_t *r);
void profiler_syscall_enter(uint64_t syscall_num);
void profiler_syscall_leave(uint64_t syscall_num, uint64_t retval);
void profiler_sched_switch(thread_t *from, thread_t *to);
void profiler_on_frame_present(uint64_t window_ptr_u64);
