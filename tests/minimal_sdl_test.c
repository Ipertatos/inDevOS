#include "include/syscalls.h"

// Entry point that the kernel expects (no main() when using -nostdlib)
void _start(void) {
    // Test basic syscalls first
    print("Starting minimal SDL test...\n");
    
    // Test SDL initialization with simple error checking
    int init_result = SDL_Init(0x00000020); // SDL_INIT_VIDEO
    
    if (init_result != 0) {
        print("SDL_Init failed\n");
        const char* error = SDL_GetError();
        if (error) {
            print("Error: ");
            print(error);
            print("\n");
        }
        exit(2);
    }
    
    print("SDL_Init succeeded\n");
    
    // Test window creation
    void* window = SDL_CreateWindow("Test", 0, 0, 320, 240, 0);
    if (!window) {
        print("SDL_CreateWindow failed\n");
        const char* error = SDL_GetError();
        if (error) {
            print("Error: ");
            print(error);
            print("\n");
        }
        SDL_Quit();
        exit(3);
    }
    
    print("Window created successfully\n");
    
    // Test renderer creation
    void* renderer = SDL_CreateRenderer(window, -1, 0x00000001); // SDL_RENDERER_SOFTWARE
    if (!renderer) {
        print("SDL_CreateRenderer failed\n");
        const char* error = SDL_GetError();
        if (error) {
            print("Error: ");
            print(error);
            print("\n");
        }
        SDL_DestroyWindow(window);
        SDL_Quit();
        exit(4);
    }
    
    print("Renderer created successfully\n");
    
    // Simple render test
    SDL_SetRenderDrawColor(renderer, 255, 0, 0, 255); // Red
    SDL_RenderClear(renderer);
    SDL_RenderPresent(renderer);
    
    print("Rendered red screen\n");
    
    // Small delay using SDL_Delay
    SDL_Delay(1000); // 1 second
    
    // Cleanup
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    
    print("Test completed successfully\n");
    exit(0);
}
