#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>
#include <stdbool.h>

/* PS/2 Mouse Driver Header */

// Initialize PS/2 mouse
bool mouse_init(void);

// Mouse interrupt handler (called from IDT)
void mouse_handler(void);
void mouse_poll(void);

// Get current mouse position
void mouse_get_position(int *x, int *y);

// Get current button states (bit 0=left, bit 1=right, bit 2=middle)
uint8_t mouse_get_buttons(void);

/* Raw movement accumulated for user apps (see SYS_SDL_GET_MOUSE_REL_ACCUM). */
extern volatile int sdl_mouse_accum_dx;
extern volatile int sdl_mouse_accum_dy;

// Scroll wheel support (IntelliMouse)
int8_t mouse_get_scroll_delta(void);  // Returns and clears accumulated scroll delta
bool mouse_has_scroll_wheel(void);    // True if IntelliMouse protocol active

// Check if mouse is initialized
bool mouse_is_initialized(void);

/** Refresh screen bounds from fb_get_* (call after changing video resolution). */
void mouse_sync_screen_bounds(void);

/**
 * Apply a USB HID boot-protocol relative report into the canonical cursor state.
 * QEMU often exposes a USB mouse; PS/2 IRQ path updates mouse separately — both
 * must feed the same position used by mouse_get_position / compositor.
 */
void mouse_feed_usb_relative(int8_t dx, int8_t dy, uint8_t buttons);

#endif // MOUSE_H












































