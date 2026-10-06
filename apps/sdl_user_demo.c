#include "include/syscalls.h"

// Simple SDL demo using syscalls
typedef struct {
    int x, y;
    int w, h;
} SDL_Rect;

typedef struct {
    uint32_t type;
    uint32_t timestamp;
} SDL_QuitEvent;

typedef struct {
    uint32_t type;
    uint32_t timestamp;
    uint32_t windowID;
    uint8_t button;
    uint8_t state;
    uint8_t clicks;
    int32_t x, y;
} SDL_MouseButtonEvent;

typedef union {
    uint32_t type;
    SDL_QuitEvent quit;
    SDL_MouseButtonEvent button;
} SDL_Event;

// Event types
#define SDL_QUIT 0x100
#define SDL_MOUSEBUTTONDOWN 0x401
#define SDL_MOUSEBUTTONUP 0x402

// SDL constants
#define SDL_INIT_VIDEO 0x00000020
#define SDL_RENDERER_SOFTWARE 0x00000001

void _start(void) {
    printf("SDL User Demo Starting...\n");
    
    // Initialize SDL
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        printf("SDL_Init Error: %s\n", SDL_GetError());
        exit(1);
    }
    printf("SDL initialized successfully\n");
    
    // Create window
    void* window = SDL_CreateWindow("SDL User Demo", 0, 0, 640, 480, 0);
    if (!window) {
        printf("SDL_CreateWindow Error: %s\n", SDL_GetError());
        SDL_Quit();
        exit(1);
    }
    printf("Window created successfully\n");
    
    // Create renderer
    void* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer) {
        printf("SDL_CreateRenderer Error: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        exit(1);
    }
    printf("Renderer created successfully\n");
    
    printf("Starting main loop - press any key to continue...\n");
    getchar();
    
    // Main loop
    SDL_Event e;
    int running = 1;
    int frame_count = 0;
    uint32_t last_time = SDL_GetTicks();
    
    while (running && frame_count < 100) { // Limit to 100 frames for demo
        // Handle events
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                running = 0;
                printf("Quit event received\n");
            } else if (e.type == SDL_MOUSEBUTTONDOWN) {
                printf("Mouse button pressed at (%d, %d)\n", e.button.x, e.button.y);
            }
        }
        
        // Clear screen to black
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        
        // Draw some colored rectangles
        uint8_t red = (uint8_t)(frame_count * 2);
        uint8_t green = (uint8_t)(128 + frame_count);
        uint8_t blue = (uint8_t)(255 - frame_count * 2);
        
        SDL_SetRenderDrawColor(renderer, red, green, blue, 255);
        
        // Draw moving rectangle
        SDL_Rect rect;
        rect.x = 50 + (frame_count * 2) % 500;
        rect.y = 50 + (frame_count) % 300;
        rect.w = 100;
        rect.h = 80;
        
        SDL_RenderFillRect(renderer, &rect);
        
        // Draw border
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        SDL_Rect border = {rect.x - 2, rect.y - 2, rect.w + 4, rect.h + 4};
        SDL_RenderDrawRect(renderer, &border);
        
        // Present the frame
        SDL_RenderPresent(renderer);
        
        frame_count++;
        
        // Print progress every 20 frames
        if (frame_count % 20 == 0) {
            uint32_t current_time = SDL_GetTicks();
            uint32_t elapsed = current_time - last_time;
            uint32_t fps = elapsed > 0 ? (20 * 1000) / elapsed : 0;
            printf("Frame %d, FPS: %u\n", frame_count, fps);
            last_time = current_time;
        }
        
        // Limit to ~30 FPS
        SDL_Delay(33);
    }
    
    printf("Demo completed after %d frames\n", frame_count);
    
    // Cleanup
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    
    printf("SDL User Demo finished successfully\n");
    exit(0);
}

