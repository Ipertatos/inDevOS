#include "../include/SDL.h"
#include "../include/sdl_kernel_log.h"
#include "hpet.h"
#include "ktime.h"
#include "utils.h"
#include "mem.h"
#include "term.h"
#include <stdint.h>
#include <stdbool.h>

// SDL Timer implementation
//
// hpet_get_ticks() already returns milliseconds. The old code treated those
// values as a 14.318 MHz raw counter, making SDL time ~14,000x too slow and
// turning short SDL_Delay / SDL_WaitEvent loops into permanent busy-spins.

static bool timer_initialized = false;
static uint64_t timer_start_ms = 0;

int SDL_TimerInit(void) {
    if (timer_initialized) {
        return SDL_SUCCESS;
    }

    sdl_kernel_log("[SDL_Timer] Initializing timer subsystem\n");

    timer_start_ms = hpet_get_ticks();
    timer_initialized = true;

    sdl_kernel_log("[SDL_Timer] Timer initialized (HPET %llu Hz, ms timebase)\n",
                   (unsigned long long)hpet_get_frequency());

    return SDL_SUCCESS;
}

void SDL_TimerQuit(void) {
    if (!timer_initialized) {
        return;
    }

    timer_initialized = false;
    sdl_kernel_log("[SDL_Timer] Timer subsystem shut down\n");
}

uint32_t SDL_GetTicks(void) {
    if (!timer_initialized) {
        return 0;
    }

    uint64_t now_ms = hpet_get_ticks();
    if (now_ms < timer_start_ms) {
        return 0;
    }
    return (uint32_t)(now_ms - timer_start_ms);
}

void SDL_Delay(uint32_t ms) {
    if (ms == 0) {
        return;
    }

    /* Yield to the scheduler (or pause until HPET advances if it is not
     * running yet). Never busy-spin on SDL_GetTicks for short delays —
     * that path previously hung forever when the timebase was wrong. */
    if (!timer_initialized) {
        hpet_sleep(ms);
        return;
    }

    ktime_yield_until(ktime_deadline_ms(ms));
}

uint64_t SDL_GetPerformanceCounter(void) {
    return hpet_get_time_us();
}

uint64_t SDL_GetPerformanceFrequency(void) {
    /* Performance counter is in microseconds. */
    return 1000000ULL;
}

double SDL_GetElapsedTime(uint64_t start_counter, uint64_t end_counter) {
    if (end_counter <= start_counter) {
        return 0.0;
    }

    uint64_t elapsed = end_counter - start_counter;
    return (double)elapsed / (double)SDL_GetPerformanceFrequency();
}

typedef struct {
    uint32_t target_fps;
    uint32_t frame_time_ms;
    uint32_t last_frame_time;
    bool initialized;
} SDL_FPSLimiter;

static SDL_FPSLimiter fps_limiter = {0};

void SDL_InitFPSLimiter(uint32_t target_fps) {
    if (target_fps == 0) {
        fps_limiter.initialized = false;
        return;
    }

    fps_limiter.target_fps = target_fps;
    fps_limiter.frame_time_ms = 1000 / target_fps;
    fps_limiter.last_frame_time = SDL_GetTicks();
    fps_limiter.initialized = true;

    sdl_kernel_log("[SDL_Timer] FPS limiter set to %u FPS (%u ms per frame)\n",
                   target_fps, fps_limiter.frame_time_ms);
}

void SDL_LimitFPS(void) {
    if (!fps_limiter.initialized) {
        return;
    }

    uint32_t current_time = SDL_GetTicks();
    uint32_t elapsed = current_time - fps_limiter.last_frame_time;

    if (elapsed < fps_limiter.frame_time_ms) {
        uint32_t delay = fps_limiter.frame_time_ms - elapsed;
        SDL_Delay(delay);
    }

    fps_limiter.last_frame_time = SDL_GetTicks();
}

static uint32_t frame_count = 0;
static uint32_t fps_last_time = 0;
static uint32_t current_fps = 0;

uint32_t SDL_GetFPS(void) {
    frame_count++;
    uint32_t current_time = SDL_GetTicks();

    if (current_time - fps_last_time >= 1000) {
        current_fps = (frame_count * 1000) / (current_time - fps_last_time);
        frame_count = 0;
        fps_last_time = current_time;
    }

    return current_fps;
}

typedef struct {
    uint32_t frame_start_time;
    uint32_t frame_end_time;
    uint32_t frame_time_ms;
    double delta_time;
    bool timing_active;
} SDL_FrameTimer;

static SDL_FrameTimer frame_timer = {0};

void SDL_StartFrame(void) {
    frame_timer.frame_start_time = SDL_GetTicks();
    frame_timer.timing_active = true;
}

void SDL_EndFrame(void) {
    if (!frame_timer.timing_active) {
        return;
    }

    frame_timer.frame_end_time = SDL_GetTicks();
    frame_timer.frame_time_ms = frame_timer.frame_end_time - frame_timer.frame_start_time;
    frame_timer.delta_time = (double)frame_timer.frame_time_ms / 1000.0;
    frame_timer.timing_active = false;
}

uint32_t SDL_GetFrameTime(void) {
    return frame_timer.frame_time_ms;
}

double SDL_GetDeltaTime(void) {
    return frame_timer.delta_time;
}
