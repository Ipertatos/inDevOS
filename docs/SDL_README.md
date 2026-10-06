# SDL-like Library for inDevOS

This document describes the SDL-like multimedia library implementation for inDevOS, providing a familiar API for graphics, events, and timing.

## Overview

The SDL library for inDevOS provides:
- **Video subsystem**: Window management, surface operations, and rendering
- **Events subsystem**: Keyboard and mouse input handling
- **Timer subsystem**: High-precision timing and frame rate control
- **Renderer**: High-level drawing operations (points, lines, rectangles)

## Architecture

### Core Components

1. **`include/SDL.h`** - Main header with all SDL structures and function declarations
2. **`src/sdl_main.c`** - Core initialization, cleanup, and main loop utilities
3. **`src/sdl_video.c`** - Video subsystem with surface management and window operations
4. **`src/sdl_renderer.c`** - High-level renderer for drawing operations
5. **`src/sdl_events.c`** - Event system integrating with existing mouse/keyboard drivers
6. **`src/sdl_timer.c`** - Timer subsystem using HPET for precise timing
7. **`src/sdl_integration.c`** - Integration layer with inDevOS input systems

### Integration with inDevOS

The SDL library leverages existing inDevOS infrastructure:
- **Framebuffer**: Uses Limine framebuffer for graphics output
- **Memory Management**: Uses `kalloc()`/`kfree()` for memory allocation
- **Timer**: Integrates with HPET timer for precise timing
- **Input**: Connects with existing PS/2 and USB mouse/keyboard drivers

## Building

The SDL library is automatically included in the kernel build:

```bash
# Build kernel with SDL library
make all

# Build SDL demo application
make sdl_demo.exe

# Build and test everything
make sdl-test
```

## API Reference

### Initialization

```c
// Initialize SDL with specified subsystems
int SDL_Init(uint32_t flags);

// Subsystem flags
SDL_INIT_VIDEO      // Graphics and windowing
SDL_INIT_EVENTS     // Keyboard and mouse input
SDL_INIT_TIMER      // High-precision timing
SDL_INIT_EVERYTHING // All subsystems

// Clean shutdown
void SDL_Quit(void);
```

### Window Management

```c
// Create a window
SDL_Window* SDL_CreateWindow(const char* title, int x, int y, int w, int h, uint32_t flags);

// Get window surface for direct pixel access
SDL_Surface* SDL_GetWindowSurface(SDL_Window* window);

// Update window display
int SDL_UpdateWindowSurface(SDL_Window* window);

// Clean up
void SDL_DestroyWindow(SDL_Window* window);
```

### Surfaces

```c
// Create RGB surface
SDL_Surface* SDL_CreateRGBSurface(uint32_t flags, int width, int height, int depth,
                                  uint32_t Rmask, uint32_t Gmask, uint32_t Bmask, uint32_t Amask);

// Surface operations
int SDL_FillRect(SDL_Surface* dst, const SDL_Rect* rect, uint32_t color);
int SDL_BlitSurface(SDL_Surface* src, const SDL_Rect* srcrect, 
                    SDL_Surface* dst, SDL_Rect* dstrect);

// Pixel operations
uint32_t SDL_MapRGB(const SDL_PixelFormat* format, uint8_t r, uint8_t g, uint8_t b);
void SDL_PutPixel(SDL_Surface* surface, int x, int y, uint32_t pixel);
uint32_t SDL_GetPixel(SDL_Surface* surface, int x, int y);

void SDL_FreeSurface(SDL_Surface* surface);
```

### Renderer (High-level Drawing)

```c
// Create renderer for a window
SDL_Renderer* SDL_CreateRenderer(SDL_Window* window, int index, uint32_t flags);

// Set drawing color
int SDL_SetRenderDrawColor(SDL_Renderer* renderer, uint8_t r, uint8_t g, uint8_t b, uint8_t a);

// Drawing operations
int SDL_RenderClear(SDL_Renderer* renderer);
int SDL_RenderDrawPoint(SDL_Renderer* renderer, int x, int y);
int SDL_RenderDrawLine(SDL_Renderer* renderer, int x1, int y1, int x2, int y2);
int SDL_RenderDrawRect(SDL_Renderer* renderer, const SDL_Rect* rect);
int SDL_RenderFillRect(SDL_Renderer* renderer, const SDL_Rect* rect);

// Present rendered frame
void SDL_RenderPresent(SDL_Renderer* renderer);

void SDL_DestroyRenderer(SDL_Renderer* renderer);
```

### Events

```c
// Event structure
typedef union SDL_Event {
    uint32_t type;
    SDL_QuitEvent quit;
    SDL_KeyboardEvent key;
    SDL_MouseButtonEvent button;
    SDL_MouseMotionEvent motion;
} SDL_Event;

// Event handling
int SDL_PollEvent(SDL_Event* event);  // Non-blocking
int SDL_WaitEvent(SDL_Event* event);  // Blocking
void SDL_PumpEvents(void);            // Update from hardware
```

### Event Types

- **SDL_QUIT**: Application quit requested
- **SDL_KEYDOWN/SDL_KEYUP**: Keyboard events
- **SDL_MOUSEBUTTONDOWN/SDL_MOUSEBUTTONUP**: Mouse button events
- **SDL_MOUSEMOTION**: Mouse movement events

### Timing

```c
// Basic timing
uint32_t SDL_GetTicks(void);          // Milliseconds since SDL_Init()
void SDL_Delay(uint32_t ms);          // Sleep for specified time

// High-precision timing
uint64_t SDL_GetPerformanceCounter(void);
uint64_t SDL_GetPerformanceFrequency(void);

// Frame timing utilities
void SDL_InitFPSLimiter(uint32_t target_fps);
void SDL_LimitFPS(void);
uint32_t SDL_GetFPS(void);

// Frame timing
void SDL_StartFrame(void);
void SDL_EndFrame(void);
uint32_t SDL_GetFrameTime(void);      // Last frame time in ms
double SDL_GetDeltaTime(void);        // Last frame time in seconds
```

## Demo Application

The `sdl_demo.c` application demonstrates SDL functionality:

```bash
# Build and run the demo
make sdl_demo.exe
# Then from the inDevOS shell:
./sdl_demo.exe
```

### Demo Features

- **Mode 0**: Bouncing ball animation
- **Mode 1**: Rotating lines with color animation
- **Mode 2**: Horizontal color gradient with sine wave
- **Mode 3**: Plasma effect using mathematical functions

### Demo Controls

- **SPACE**: Switch between demo modes
- **R**: Reset animation
- **ESC**: Quit demo
- **Mouse Click**: Move ball (in bouncing ball mode)

## Example Usage

```c
#include "include/SDL.h"

int main(void) {
    // Initialize SDL
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER) != 0) {
        printf("SDL_Init Error: %s\n", SDL_GetError());
        return 1;
    }
    
    // Create window
    SDL_Window* window = SDL_CreateWindow("My Game", 0, 0, 800, 600, SDL_WINDOW_SHOWN);
    if (!window) {
        printf("SDL_CreateWindow Error: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    
    // Create renderer
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer) {
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    
    // Main loop
    SDL_Event e;
    bool quit = false;
    while (!quit) {
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                quit = true;
            }
        }
        
        // Clear screen
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        
        // Draw red rectangle
        SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255);
        SDL_Rect rect = {100, 100, 200, 150};
        SDL_RenderFillRect(renderer, &rect);
        
        // Present
        SDL_RenderPresent(renderer);
        
        SDL_Delay(16); // ~60 FPS
    }
    
    // Cleanup
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
```

## Performance Considerations

- The library uses software rendering (no GPU acceleration)
- For best performance:
  - Use surfaces for static content
  - Use the renderer for dynamic drawing
  - Limit updates to changed areas when possible
  - Consider the framebuffer resolution for performance

## Current Limitations

1. **Single Window**: Only one fullscreen window is supported
2. **Software Rendering**: No hardware acceleration
3. **Limited Audio**: Audio subsystem is not yet implemented
4. **Input Integration**: Mouse/keyboard integration is simplified
5. **Pixel Formats**: Only 32-bit RGBA is fully supported

## Future Enhancements

- Audio subsystem using existing WAV playback
- Multiple window support
- Hardware-accelerated rendering (if GPU driver available)
- More complete mouse/keyboard integration
- Texture support for optimized sprite rendering
- Additional pixel format support

## Integration Notes

To integrate SDL applications with inDevOS:

1. Include `SDL.h` in your application
2. Use the provided syscall interface for system interaction
3. Build with `build_userapp.sh` script
4. Applications run in user space and communicate with the kernel-embedded SDL library

The SDL library is embedded in the kernel, providing direct access to hardware resources while maintaining a familiar programming interface.




