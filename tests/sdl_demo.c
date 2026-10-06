#include "include/SDL.h"
#include "include/syscalls.h"
#include <stdint.h>
#include <stdbool.h>

// Simple math functions (since we don't have libm)
static double fabs(double x) {
    return x < 0.0 ? -x : x;
}

static double sin_approx(double x) {
    // Simple sine approximation using Taylor series (first few terms)
    // sin(x) ≈ x - x³/6 + x⁵/120 - x⁷/5040
    double x2 = x * x;
    double x3 = x2 * x;
    double x5 = x3 * x2;
    double x7 = x5 * x2;
    
    return x - (x3 / 6.0) + (x5 / 120.0) - (x7 / 5040.0);
}

static double cos_approx(double x) {
    // cos(x) ≈ 1 - x²/2 + x⁴/24 - x⁶/720
    double x2 = x * x;
    double x4 = x2 * x2;
    double x6 = x4 * x2;
    
    return 1.0 - (x2 / 2.0) + (x4 / 24.0) - (x6 / 720.0);
}

// Demo state
typedef struct {
    SDL_Window* window;
    SDL_Renderer* renderer;
    SDL_Surface* surface;
    
    // Animation variables
    uint32_t frame_count;
    double time;
    int ball_x, ball_y;
    int ball_dx, ball_dy;
    
    // Colors
    uint32_t red, green, blue, white, black;
    
    // Demo mode
    int demo_mode;
    
    bool running;
} DemoState;

static DemoState demo;

// Initialize demo
bool demo_init(void) {
    printf("SDL Demo: Initializing...\n");
    
    // Initialize SDL
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER) != SDL_SUCCESS) {
        printf("SDL Demo: Failed to initialize SDL: %s\n", SDL_GetError());
        return false;
    }
    
    // Create window
    demo.window = SDL_CreateWindow("SDL Demo", 0, 0, 800, 600, SDL_WINDOW_SHOWN);
    if (!demo.window) {
        printf("SDL Demo: Failed to create window: %s\n", SDL_GetError());
        SDL_Quit();
        return false;
    }
    
    // Create renderer
    demo.renderer = SDL_CreateRenderer(demo.window, -1, SDL_RENDERER_SOFTWARE);
    if (!demo.renderer) {
        printf("SDL Demo: Failed to create renderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(demo.window);
        SDL_Quit();
        return false;
    }
    
    // Get surface for direct pixel access
    demo.surface = SDL_GetWindowSurface(demo.window);
    if (!demo.surface) {
        printf("SDL Demo: Failed to get window surface: %s\n", SDL_GetError());
        SDL_DestroyRenderer(demo.renderer);
        SDL_DestroyWindow(demo.window);
        SDL_Quit();
        return false;
    }
    
    // Set up colors
    demo.red = SDL_MapRGB(demo.surface->format, 255, 0, 0);
    demo.green = SDL_MapRGB(demo.surface->format, 0, 255, 0);
    demo.blue = SDL_MapRGB(demo.surface->format, 0, 0, 255);
    demo.white = SDL_MapRGB(demo.surface->format, 255, 255, 255);
    demo.black = SDL_MapRGB(demo.surface->format, 0, 0, 0);
    
    // Initialize animation state
    demo.frame_count = 0;
    demo.time = 0.0;
    demo.ball_x = 100;
    demo.ball_y = 100;
    demo.ball_dx = 2;
    demo.ball_dy = 3;
    demo.demo_mode = 0;
    demo.running = true;
    
    printf("SDL Demo: Initialization complete\n");
    return true;
}

// Clean up demo
void demo_cleanup(void) {
    printf("SDL Demo: Cleaning up...\n");
    
    if (demo.renderer) {
        SDL_DestroyRenderer(demo.renderer);
    }
    if (demo.window) {
        SDL_DestroyWindow(demo.window);
    }
    
    SDL_Quit();
    printf("SDL Demo: Cleanup complete\n");
}

// Handle events
void demo_handle_events(void) {
    SDL_Event event;
    
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_QUIT:
                demo.running = false;
                break;
                
            case SDL_KEYDOWN:
                switch (event.key.sym) {
                    case SDLK_ESCAPE:
                        demo.running = false;
                        break;
                    case SDLK_SPACE:
                        demo.demo_mode = (demo.demo_mode + 1) % 4;
                        printf("SDL Demo: Switched to mode %d\n", demo.demo_mode);
                        break;
                    case SDLK_r:
                        // Reset animation
                        demo.frame_count = 0;
                        demo.time = 0.0;
                        demo.ball_x = 100;
                        demo.ball_y = 100;
                        break;
                }
                break;
                
            case SDL_MOUSEBUTTONDOWN:
                printf("SDL Demo: Mouse button %d clicked at (%d, %d)\n", 
                       event.button.button, event.button.x, event.button.y);
                // Reset ball position to mouse click
                demo.ball_x = event.button.x;
                demo.ball_y = event.button.y;
                break;
                
            case SDL_MOUSEMOTION:
                // Optional: could track mouse for interactive demos
                break;
        }
    }
}

// Draw bouncing ball
void demo_bouncing_ball(void) {
    // Update ball position
    demo.ball_x += demo.ball_dx;
    demo.ball_y += demo.ball_dy;
    
    // Bounce off edges
    if (demo.ball_x <= 10 || demo.ball_x >= demo.surface->w - 10) {
        demo.ball_dx = -demo.ball_dx;
    }
    if (demo.ball_y <= 10 || demo.ball_y >= demo.surface->h - 10) {
        demo.ball_dy = -demo.ball_dy;
    }
    
    // Draw ball
    SDL_Rect ball_rect = {demo.ball_x - 10, demo.ball_y - 10, 20, 20};
    SDL_FillRect(demo.surface, &ball_rect, demo.red);
}

// Draw rotating lines
void demo_rotating_lines(void) {
    int center_x = demo.surface->w / 2;
    int center_y = demo.surface->h / 2;
    int radius = 200;
    
    // Draw multiple rotating lines
    for (int i = 0; i < 8; i++) {
        double angle = demo.time + (i * 3.14159 / 4.0);
        int end_x = center_x + (int)(radius * cos_approx(angle));
        int end_y = center_y + (int)(radius * sin_approx(angle));
        
        // Draw line using renderer
        uint8_t color_val = (uint8_t)(128 + 127 * sin_approx(demo.time + i));
        SDL_SetRenderDrawColor(demo.renderer, color_val, 255 - color_val, 
                              (color_val + 128) % 256, 255);
        SDL_RenderDrawLine(demo.renderer, center_x, center_y, end_x, end_y);
    }
}

// Draw color gradient
void demo_color_gradient(void) {
    // Draw horizontal color gradient
    for (int x = 0; x < demo.surface->w; x++) {
        uint8_t r = (uint8_t)((x * 255) / demo.surface->w);
        uint8_t g = (uint8_t)(128 + 127 * sin_approx(demo.time + x * 0.01));
        uint8_t b = 255 - r;
        
        uint32_t color = SDL_MapRGB(demo.surface->format, r, g, b);
        
        SDL_Rect line = {x, 100, 1, demo.surface->h - 200};
        SDL_FillRect(demo.surface, &line, color);
    }
}

// Draw plasma effect
void demo_plasma_effect(void) {
    int w = demo.surface->w;
    int h = demo.surface->h;
    
    // Simple plasma effect using direct pixel access
    for (int y = 0; y < h; y += 4) { // Skip pixels for performance
        for (int x = 0; x < w; x += 4) {
            double dx = (double)x / 50.0;
            double dy = (double)y / 50.0;
            
            double value = sin_approx(dx + demo.time) +
                          sin_approx(dy + demo.time * 1.3) +
                          sin_approx((dx + dy) * 0.5 + demo.time * 0.7);
            
            uint8_t r = (uint8_t)(128 + 127 * sin_approx(value));
            uint8_t g = (uint8_t)(128 + 127 * sin_approx(value + 2.0));
            uint8_t b = (uint8_t)(128 + 127 * sin_approx(value + 4.0));
            
            uint32_t color = SDL_MapRGB(demo.surface->format, r, g, b);
            
            // Draw 4x4 block for performance
            SDL_Rect block = {x, y, 4, 4};
            SDL_FillRect(demo.surface, &block, color);
        }
    }
}

// Update demo
void demo_update(void) {
    demo.frame_count++;
    demo.time = (double)demo.frame_count * 0.016; // Assume ~60 FPS
    
    // Clear screen
    SDL_SetRenderDrawColor(demo.renderer, 0, 0, 0, 255);
    SDL_RenderClear(demo.renderer);
    SDL_FillRect(demo.surface, NULL, demo.black);
    
    // Draw based on current mode
    switch (demo.demo_mode) {
        case 0:
            demo_bouncing_ball();
            break;
        case 1:
            demo_rotating_lines();
            break;
        case 2:
            demo_color_gradient();
            break;
        case 3:
            demo_plasma_effect();
            break;
    }
    
    // Present
    SDL_RenderPresent(demo.renderer);
    SDL_UpdateWindowSurface(demo.window);
}

// Print demo instructions
void demo_print_instructions(void) {
    printf("\n=== SDL Demo Instructions ===\n");
    printf("SPACE: Switch demo mode\n");
    printf("R: Reset animation\n");
    printf("ESC: Quit\n");
    printf("Mouse Click: Move ball (in bouncing ball mode)\n");
    printf("\nDemo Modes:\n");
    printf("0: Bouncing Ball\n");
    printf("1: Rotating Lines\n");
    printf("2: Color Gradient\n");
    printf("3: Plasma Effect\n");
    printf("============================\n\n");
}

// Main demo function
int main(void) {
    printf("SDL Demo Starting...\n");
    
    demo_print_instructions();
    
    if (!demo_init()) {
        printf("SDL Demo: Failed to initialize\n");
        return 1;
    }
    
    printf("SDL Demo: Running (current mode: %d)\n", demo.demo_mode);
    printf("SDL Demo: Press SPACE to change modes, ESC to quit\n");
    
    // Main loop
    uint32_t last_fps_time = SDL_GetTicks();
    uint32_t frame_count = 0;
    
    while (demo.running) {
        uint32_t frame_start = SDL_GetTicks();
        
        demo_handle_events();
        demo_update();
        
        frame_count++;
        
        // Print FPS every second
        uint32_t current_time = SDL_GetTicks();
        if (current_time - last_fps_time >= 1000) {
            uint32_t fps = (frame_count * 1000) / (current_time - last_fps_time);
            printf("SDL Demo: FPS: %u, Mode: %d\n", fps, demo.demo_mode);
            frame_count = 0;
            last_fps_time = current_time;
        }
        
        // Limit to roughly 60 FPS
        uint32_t frame_time = SDL_GetTicks() - frame_start;
        if (frame_time < 16) {
            SDL_Delay(16 - frame_time);
        }
    }
    
    demo_cleanup();
    printf("SDL Demo: Finished\n");
    return 0;
}




