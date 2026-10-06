#include "../include/SDL.h"
#include "../include/sdl_kernel_log.h"
#include "utils.h"
#include "mouse.h"
#include "keyboard.h"
#include "term.h"

// SDL Integration with existing inDevOS input systems

// Connect SDL with existing mouse driver
void sdl_integrate_mouse(void) {
    // This function would be called from the mouse interrupt handler
    // to inject SDL events when mouse state changes
    
    // Get current mouse state from existing driver
    extern void mouse_get_position(int* x, int* y);
    extern uint8_t mouse_get_button_state(void);
    
    static int last_x = 0, last_y = 0;
    static uint8_t last_buttons = 0;
    
    int current_x, current_y;
    mouse_get_position(&current_x, &current_y);
    uint8_t current_buttons = mouse_get_buttons();
    
    // Check for motion
    if (current_x != last_x || current_y != last_y) {
        // Inject SDL mouse motion event
        extern void SDL_InjectMouseMotion(int x, int y, int xrel, int yrel);
        SDL_InjectMouseMotion(current_x, current_y, 
                             current_x - last_x, current_y - last_y);
        last_x = current_x;
        last_y = current_y;
    }
    
    // Check for button changes
    for (int button = 1; button <= 3; button++) {
        uint8_t mask = 1 << (button - 1);
        bool was_pressed = (last_buttons & mask) != 0;
        bool is_pressed = (current_buttons & mask) != 0;
        
        if (was_pressed != is_pressed) {
            extern void SDL_InjectMouseButton(int button, bool pressed, int x, int y);
            SDL_InjectMouseButton(button, is_pressed, current_x, current_y);
        }
    }
    
    last_buttons = current_buttons;
}

// Connect SDL with existing keyboard driver
void sdl_integrate_keyboard(uint8_t scancode, bool pressed) {
    // This function would be called from the keyboard interrupt handler
    // to inject SDL keyboard events
    
    extern void SDL_InjectKeyEvent(bool is_down, uint8_t scancode);
    SDL_InjectKeyEvent(pressed, scancode);
}

// Forward declarations for existing mouse functions
extern void mouse_get_position(int* x, int* y);
extern uint8_t mouse_get_buttons(void);

// SDL event injection functions (to be called from hardware drivers)
void SDL_InjectMouseMotion(int x, int y, int xrel, int yrel) {
    extern bool SDL_IsInitialized(void);
    if (!SDL_IsInitialized()) return;
    
    // This would push a mouse motion event to the SDL event queue
    // Implementation depends on making the event queue accessible
}

void SDL_InjectMouseButton(int button, bool pressed, int x, int y) {
    extern bool SDL_IsInitialized(void);
    if (!SDL_IsInitialized()) return;
    
    // This would push a mouse button event to the SDL event queue
    // Implementation depends on making the event queue accessible
}

// Integration initialization
void SDL_InitializeIntegration(void) {
    sdl_kernel_log("[SDL_Integration] Connecting SDL with inDevOS input systems\n");
    
    // In a full implementation, we would:
    // 1. Register callbacks with the mouse driver
    // 2. Register callbacks with the keyboard driver
    // 3. Set up periodic polling if needed
}
