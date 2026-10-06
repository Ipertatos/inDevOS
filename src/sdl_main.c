#include "../include/SDL.h"
#include "../include/sdl_kernel_log.h"
#include "utils.h"
#include "mem.h"
#include "term.h"
#include <stdint.h>
#include <stdbool.h>

// SDL Main implementation - Core initialization and management

// External function declarations for subsystems
extern int SDL_VideoInit(void);
extern void SDL_VideoQuit(void);
extern int SDL_EventsInit(void);
extern void SDL_EventsQuit(void);
extern int SDL_TimerInit(void);
extern void SDL_TimerQuit(void);

// SDL state
static uint32_t initialized_subsystems = 0;
static bool sdl_initialized = false;
static char error_message[256] = "No error";

// Set SDL error message
static void SDL_SetError(const char* fmt, ...) {
    // Simple error message setting (no varargs for simplicity)
    const char* src = fmt;
    char* dst = error_message;
    int i = 0;
    
    while (*src && i < 255) {
        *dst++ = *src++;
        i++;
    }
    *dst = '\0';
}

// Get SDL error message
const char* SDL_GetError(void) {
    return error_message;
}

// Initialize SDL
int SDL_Init(uint32_t flags) {
    if (sdl_initialized) {
        sdl_kernel_log("[SDL] Warning: SDL already initialized\n");
        return SDL_SUCCESS;
    }
    
    sdl_kernel_log("[SDL] Initializing SDL v%d.%d.%d\n", 
           SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_PATCHLEVEL);
    
    // Clear error
    SDL_SetError("No error");
    
    // Initialize timer first (needed by other subsystems)
    if (flags & SDL_INIT_TIMER) {
        if (SDL_TimerInit() != SDL_SUCCESS) {
            SDL_SetError("Failed to initialize timer subsystem");
            return SDL_ERROR;
        }
        initialized_subsystems |= SDL_INIT_TIMER;
        sdl_kernel_log("[SDL] Timer subsystem initialized\n");
    }
    
    // Initialize video subsystem
    if (flags & SDL_INIT_VIDEO) {
        if (SDL_VideoInit() != SDL_SUCCESS) {
            SDL_SetError("Failed to initialize video subsystem");
            SDL_Quit(); // Clean up any initialized subsystems
            return SDL_ERROR;
        }
        initialized_subsystems |= SDL_INIT_VIDEO;
        sdl_kernel_log("[SDL] Video subsystem initialized\n");
    }
    
    // Initialize events subsystem
    if (flags & SDL_INIT_EVENTS) {
        if (SDL_EventsInit() != SDL_SUCCESS) {
            SDL_SetError("Failed to initialize events subsystem");
            SDL_Quit(); // Clean up any initialized subsystems
            return SDL_ERROR;
        }
        initialized_subsystems |= SDL_INIT_EVENTS;
        sdl_kernel_log("[SDL] Events subsystem initialized\n");
    }
    
    // Audio not implemented — do not set SDL_INIT_AUDIO in initialized_subsystems
    if (flags & SDL_INIT_AUDIO) {
        sdl_kernel_log("[SDL] Audio subsystem not implemented yet\n");
    }
    
    sdl_initialized = true;
    sdl_kernel_log("[SDL] SDL initialization complete (subsystems: 0x%X)\n", initialized_subsystems);
    
    return SDL_SUCCESS;
}

// Initialize specific SDL subsystem
int SDL_InitSubSystem(uint32_t flags) {
    // For now, just call SDL_Init with the additional flags
    return SDL_Init(initialized_subsystems | flags);
}

// Quit specific SDL subsystem
void SDL_QuitSubSystem(uint32_t flags) {
    if (!sdl_initialized) {
        return;
    }
    
    // Quit requested subsystems
    if ((flags & SDL_INIT_EVENTS) && (initialized_subsystems & SDL_INIT_EVENTS)) {
        SDL_EventsQuit();
        initialized_subsystems &= ~SDL_INIT_EVENTS;
        sdl_kernel_log("[SDL] Events subsystem shut down\n");
    }
    
    if ((flags & SDL_INIT_VIDEO) && (initialized_subsystems & SDL_INIT_VIDEO)) {
        SDL_VideoQuit();
        initialized_subsystems &= ~SDL_INIT_VIDEO;
        sdl_kernel_log("[SDL] Video subsystem shut down\n");
    }
    
    if ((flags & SDL_INIT_TIMER) && (initialized_subsystems & SDL_INIT_TIMER)) {
        SDL_TimerQuit();
        initialized_subsystems &= ~SDL_INIT_TIMER;
        sdl_kernel_log("[SDL] Timer subsystem shut down\n");
    }
    
    if ((flags & SDL_INIT_AUDIO) && (initialized_subsystems & SDL_INIT_AUDIO)) {
        // Audio quit would go here
        initialized_subsystems &= ~SDL_INIT_AUDIO;
        sdl_kernel_log("[SDL] Audio subsystem shut down\n");
    }
}

// Check if subsystem is initialized
uint32_t SDL_WasInit(uint32_t flags) {
    if (!sdl_initialized) {
        return 0;
    }
    
    if (flags == 0) {
        return initialized_subsystems; // Return all initialized subsystems
    }
    
    return initialized_subsystems & flags;
}

// Quit SDL
void SDL_Quit(void) {
    if (!sdl_initialized) {
        return;
    }
    
    sdl_kernel_log("[SDL] Shutting down SDL\n");
    
    // Quit all initialized subsystems in reverse order
    SDL_QuitSubSystem(SDL_INIT_EVERYTHING);
    
    initialized_subsystems = 0;
    sdl_initialized = false;
    
    sdl_kernel_log("[SDL] SDL shutdown complete\n");
}

// Get SDL version
void SDL_GetVersion(int* major, int* minor, int* patch) {
    if (major) *major = SDL_MAJOR_VERSION;
    if (minor) *minor = SDL_MINOR_VERSION;
    if (patch) *patch = SDL_PATCHLEVEL;
}

// Check if SDL is initialized
bool SDL_IsInitialized(void) {
    return sdl_initialized;
}

// Get initialized subsystems
uint32_t SDL_GetInitializedSubsystems(void) {
    return initialized_subsystems;
}

// SDL main loop helper functions
bool SDL_ShouldQuit(void) {
    extern bool SDL_QuitRequested(void);
    return SDL_QuitRequested();
}

// Simple main loop wrapper
int SDL_MainLoop(void (*update_func)(void), void (*render_func)(void), uint32_t target_fps) {
    if (!sdl_initialized) {
        SDL_SetError("SDL not initialized");
        return SDL_ERROR;
    }
    
    extern void SDL_InitFPSLimiter(uint32_t target_fps);
    extern void SDL_LimitFPS(void);
    extern void SDL_StartFrame(void);
    extern void SDL_EndFrame(void);
    
    if (target_fps > 0) {
        SDL_InitFPSLimiter(target_fps);
    }
    
    sdl_kernel_log("[SDL] Starting main loop (target FPS: %u)\n", target_fps);
    
    while (!SDL_ShouldQuit()) {
        SDL_StartFrame();
        
        // Update
        if (update_func) {
            update_func();
        }
        
        // Render
        if (render_func) {
            render_func();
        }
        
        SDL_EndFrame();
        
        // Limit FPS if requested
        if (target_fps > 0) {
            SDL_LimitFPS();
        }
    }
    
    sdl_kernel_log("[SDL] Main loop ended\n");
    return SDL_SUCCESS;
}




