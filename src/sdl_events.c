#include "../include/SDL.h"
#include "../include/sdl_kernel_log.h"
#include "utils.h"
#include "mem.h"
#include "term.h"
#include "keyboard.h"
#include "mouse.h"
#include "atomic.h"
#include "desktop.h"
#include <stdint.h>
#include <stdbool.h>
#include "string.h"

// SDL Events implementation

#define MAX_EVENTS 64
/* Parallel to the event queue: current key state by SDL_Keycode index (for user syscall).
 * volatile: updated from keyboard IRQ on any CPU; user syscall reads on another CPU. */
#define SDL_KB_STATE_SIZE 1024
volatile uint8_t sdl_keyboard_state[SDL_KB_STATE_SIZE];

static void sdl_kb_state_clear(void) {
    for (int i = 0; i < SDL_KB_STATE_SIZE; i++)
        sdl_keyboard_state[i] = 0;
}

// Event queue
static SDL_Event event_queue[MAX_EVENTS];
static uint32_t event_queue_head = 0;
static uint32_t event_queue_tail = 0;
static uint32_t event_count = 0;
static spinlock_t event_queue_lock = {0};

// Event system state
static bool events_initialized = false;
static bool quit_requested = false;

static bool sdl_desktop_has_input_focus(void) {
    if (!desktop_is_running())
        return true;
    return desktop_focus_eats_keys();
}

// Previous input states for generating events
static uint8_t prev_mouse_buttons = 0;
static int prev_mouse_x = 0, prev_mouse_y = 0;
static bool prev_key_states[256] = {false};

// Initialize SDL events
int SDL_EventsInit(void) {
    if (events_initialized) {
        return SDL_SUCCESS;
    }
    
    sdl_kernel_log("[SDL_Events] Initializing event system\n");

    sdl_kb_state_clear();

    // Clear event queue
    event_queue_head = 0;
    event_queue_tail = 0;
    event_count = 0;
    quit_requested = false;
    
    // Initialize previous states
    for (int i = 0; i < 256; i++) {
        prev_key_states[i] = false;
    }
    
    events_initialized = true;
    return SDL_SUCCESS;
}

// Quit SDL events
void SDL_EventsQuit(void) {
    if (!events_initialized) {
        return;
    }
    
    /* irq_save: spinlocks are not IRQ-safe; keyboard IRQ may call push_event. */
    uint64_t irqf = irq_save();
    spinlock_lock(&event_queue_lock);
    event_queue_head = 0;
    event_queue_tail = 0;
    event_count = 0;
    spinlock_unlock(&event_queue_lock);
    irq_restore(irqf);

    sdl_kb_state_clear();

    events_initialized = false;
    sdl_kernel_log("[SDL_Events] Event system shut down\n");
}

void SDL_ResetInputState(void) {
    uint64_t irqf = irq_save();
    spinlock_lock(&event_queue_lock);
    event_queue_head = 0;
    event_queue_tail = 0;
    event_count = 0;
    spinlock_unlock(&event_queue_lock);
    irq_restore(irqf);

    sdl_kb_state_clear();
    for (int i = 0; i < 256; i++)
        prev_key_states[i] = false;
    prev_mouse_buttons = 0;
}

// Add event to queue (internal function)
static bool push_event(const SDL_Event* event) {
    uint64_t irqf = irq_save();
    spinlock_lock(&event_queue_lock);

    if (event_count >= MAX_EVENTS) {
        spinlock_unlock(&event_queue_lock);
        irq_restore(irqf);
        return false; // Queue full
    }

    event_queue[event_queue_tail] = *event;
    event_queue_tail = (event_queue_tail + 1) % MAX_EVENTS;
    event_count++;

    spinlock_unlock(&event_queue_lock);
    irq_restore(irqf);
    return true;
}

// Get event from queue (internal function)
static bool pop_event(SDL_Event* event) {
    uint64_t irqf = irq_save();
    spinlock_lock(&event_queue_lock);

    if (event_count == 0) {
        spinlock_unlock(&event_queue_lock);
        irq_restore(irqf);
        return false; // Queue empty
    }

    *event = event_queue[event_queue_head];
    event_queue_head = (event_queue_head + 1) % MAX_EVENTS;
    event_count--;

    spinlock_unlock(&event_queue_lock);
    irq_restore(irqf);
    return true;
}

// Convert PS/2 scancode to SDL keycode (shared with PS/2 keyboard → desktop routing)
SDL_Keycode SDL_Ps2ScancodeToKeycode(uint8_t scancode) {
    switch (scancode) {
        case 0x1E: return SDLK_a;
        case 0x30: return SDLK_b;
        case 0x2E: return SDLK_c;
        case 0x20: return SDLK_d;
        case 0x12: return SDLK_e;
        case 0x21: return SDLK_f;
        case 0x22: return SDLK_g;
        case 0x23: return SDLK_h;
        case 0x17: return SDLK_i;
        case 0x24: return SDLK_j;
        case 0x25: return SDLK_k;
        case 0x26: return SDLK_l;
        case 0x32: return SDLK_m;
        case 0x31: return SDLK_n;
        case 0x18: return SDLK_o;
        case 0x19: return SDLK_p;
        case 0x10: return SDLK_q;
        case 0x13: return SDLK_r;
        case 0x1F: return SDLK_s;
        case 0x14: return SDLK_t;
        case 0x16: return SDLK_u;
        case 0x2F: return SDLK_v;
        case 0x11: return SDLK_w;
        case 0x2D: return SDLK_x;
        case 0x15: return SDLK_y;
        case 0x2C: return SDLK_z;
        case 0x0B: return SDLK_0;
        case 0x02: return SDLK_1;
        case 0x03: return SDLK_2;
        case 0x04: return SDLK_3;
        case 0x05: return SDLK_4;
        case 0x06: return SDLK_5;
        case 0x07: return SDLK_6;
        case 0x08: return SDLK_7;
        case 0x09: return SDLK_8;
        case 0x0A: return SDLK_9;
        case 0x39: return SDLK_SPACE;
        case 0x1C: return SDLK_RETURN;
        case 0x01: return SDLK_ESCAPE;
        case 0x0E: return SDLK_BACKSPACE;
        case 0x0F: return SDLK_TAB;
        case 0x48: return SDLK_UP;
        case 0x50: return SDLK_DOWN;
        case 0x4B: return SDLK_LEFT;
        case 0x4D: return SDLK_RIGHT;
        case 0x49: return SDLK_PAGEUP;
        case 0x51: return SDLK_PAGEDOWN;
        case 0x47: return SDLK_HOME;
        case 0x4F: return SDLK_END;
        case 0x53: return 127;          /* Delete key = SDLK_DELETE */
        case 0x0C: return SDLK_MINUS;
        case 0x0D: return SDLK_EQUALS;
        case 0x33: return SDLK_COMMA;
        case 0x34: return SDLK_PERIOD;
        case 0x35: return SDLK_SLASH;
        case 0x2A: return SDLK_LSHIFT;
        case 0x36: return SDLK_RSHIFT;
        case 0x1D: return SDLK_LCTRL;
        case 0x38: return SDLK_LALT;
        case 0x3E: return SDLK_F4;
        case 0x5B: return SDLK_LGUI;
        default: return SDLK_UNKNOWN;
    }
}

// Pump events from hardware
void SDL_PumpEvents(void) {
    if (!events_initialized) {
        return;
    }
    
    /* Keyboard: PS/2 handler calls SDL_InjectKeyEvent when SDL is initialized. */
    int current_mouse_x = 0;
    int current_mouse_y = 0;
    mouse_get_position(&current_mouse_x, &current_mouse_y);
    uint8_t current_mouse_buttons = mouse_get_buttons();

    /* When the desktop compositor is active, translate screen-space mouse
     * coordinates into window-local coordinates for the focused window.
     * Only deliver events to the process that owns the focused window. */
    int event_mx = current_mouse_x;
    int event_my = current_mouse_y;
    uint32_t event_wid = 1;

    if (desktop_is_running()) {
        desktop_window_t *fw = desktop_get_focused();
        if (fw) {
            int cx = fw->x + (fw->decorated ? DESKTOP_BORDER_W : 0);
            int cy = fw->y + (fw->decorated ? DESKTOP_BORDER_W + DESKTOP_TITLEBAR_H : 0);
            event_mx = current_mouse_x - cx;
            event_my = current_mouse_y - cy;
            event_wid = fw->id;
        }
    }

    // Generate mouse motion events
    if (current_mouse_x != prev_mouse_x || current_mouse_y != prev_mouse_y) {
        SDL_Event motion_event;
        motion_event.type = SDL_MOUSEMOTION;
        motion_event.motion.timestamp = SDL_GetTicks();
        motion_event.motion.windowID = event_wid;
        motion_event.motion.x = event_mx;
        motion_event.motion.y = event_my;
        motion_event.motion.xrel = current_mouse_x - prev_mouse_x;
        motion_event.motion.yrel = current_mouse_y - prev_mouse_y;
        
        push_event(&motion_event);
        
        prev_mouse_x = current_mouse_x;
        prev_mouse_y = current_mouse_y;
    }
    
    // Generate mouse button events
    for (int button = 1; button <= 3; button++) {
        uint8_t mask = 1 << (button - 1);
        bool was_pressed = (prev_mouse_buttons & mask) != 0;
        bool is_pressed = (current_mouse_buttons & mask) != 0;
        
        if (was_pressed != is_pressed) {
            SDL_Event button_event;
            button_event.type = is_pressed ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
            button_event.button.timestamp = SDL_GetTicks();
            button_event.button.windowID = event_wid;
            button_event.button.button = button;
            button_event.button.state = is_pressed ? 1 : 0;
            button_event.button.clicks = 1;
            button_event.button.x = event_mx;
            button_event.button.y = event_my;
            
            push_event(&button_event);
        }
    }
    
    prev_mouse_buttons = current_mouse_buttons;
}

// Poll for events
int SDL_PollEvent(SDL_Event* event) {
    if (!events_initialized || !event) {
        return 0;
    }
    
    // Pump hardware events first
    SDL_PumpEvents();
    
    // Try to get an event from the queue
    return pop_event(event) ? 1 : 0;
}

// Wait for an event
int SDL_WaitEvent(SDL_Event* event) {
    if (!events_initialized || !event) {
        return 0;
    }

    /* Poll then yield. SDL_Delay now sleeps via ktime_yield_until / the
     * thread scheduler instead of busy-spinning on a broken timebase. */
    while (true) {
        if (SDL_PollEvent(event)) {
            return 1;
        }
        SDL_Delay(1);
    }
}

// Request quit
void SDL_RequestQuit(void) {
    if (!events_initialized) {
        return;
    }
    
    SDL_Event quit_event;
    quit_event.type = SDL_QUIT;
    quit_event.quit.timestamp = SDL_GetTicks();
    
    push_event(&quit_event);
    quit_requested = true;
}

// Check if quit was requested
bool SDL_QuitRequested(void) {
    return quit_requested;
}

/*
 * IRQ-safe keyboard state update.
 *
 * The full SDL_InjectKeyEvent() takes the event_queue spinlock and notifies
 * the desktop, neither of which is safe to do from interrupt context (the
 * spinlock is not IRQ-disabling, and the desktop GUI queue isn't either).
 *
 * User apps that drive themselves with SDL_GetKeyboardState() — including
 * the in-tree DOOM port — only need the sdl_keyboard_state[] byte array
 * to be current. That's a single volatile byte store: safe from any
 * context, including the PS/2 keyboard ISR while a Ring-3 thread is
 * actively running (i.e. the kernel main-loop drainer is starved).
 *
 * Without this fast path, sdl_keyboard_state[] only updates after the
 * kernel main loop drains sdl_key_queue — which never happens while a
 * synchronous exec_run_elf is in user mode, so DOOM saw zero key input.
 */
void SDL_InjectKeyState_Fast(bool is_down, uint8_t scancode) {
    SDL_Keycode key = SDL_Ps2ScancodeToKeycode(scancode);
    if (key == SDLK_UNKNOWN) return;
    if (key >= 0 && key < SDL_KB_STATE_SIZE) {
        /* Always record hardware state.  Filtering belongs at read time
         * (GET_KEYBOARD_STATE).  Zeroing here made Doom miss every key
         * when the focused window's pid was stale/0 on SMP. */
        sdl_keyboard_state[(size_t)key] = is_down ? 1u : 0u;
    }
}

/** Modifier bitmask derived from sdl_keyboard_state (PS/2/USB inject path). */
static uint16_t sdl_keyb_modifiers_from_state(void) {
    uint16_t m = 0;
    if (SDLK_LSHIFT < SDL_KB_STATE_SIZE && sdl_keyboard_state[SDLK_LSHIFT]) m |= KMOD_LSHIFT;
    if (SDLK_RSHIFT < SDL_KB_STATE_SIZE && sdl_keyboard_state[SDLK_RSHIFT]) m |= KMOD_RSHIFT;
    if (SDLK_LCTRL < SDL_KB_STATE_SIZE && sdl_keyboard_state[SDLK_LCTRL]) m |= KMOD_LCTRL;
    if (SDLK_RCTRL < SDL_KB_STATE_SIZE && sdl_keyboard_state[SDLK_RCTRL]) m |= KMOD_RCTRL;
    if (SDLK_LALT < SDL_KB_STATE_SIZE && sdl_keyboard_state[SDLK_LALT]) m |= KMOD_LALT;
    if (SDLK_RALT < SDL_KB_STATE_SIZE && sdl_keyboard_state[SDLK_RALT]) m |= KMOD_RALT;
    return m;
}

// Inject a keyboard event (for integration with existing keyboard system)
void SDL_InjectKeyEvent(bool is_down, uint8_t scancode) {
    /*
     * Always update sdl_keyboard_state[] first — user apps read it via
     * SYS_SDL_GET_KEYBOARD_STATE without using the SDL event queue. This must
     * NOT be gated on events_initialized (otherwise keys stay zero if events
     * init races or fails while video/timer are up).
     */
    SDL_Keycode key = SDL_Ps2ScancodeToKeycode(scancode);
    if (desktop_is_running() && desktop_handle_key(scancode, is_down))
        return;
    if (key == SDLK_UNKNOWN) {
        return;
    }
    if (!sdl_desktop_has_input_focus()) {
        /* Keep sdl_keyboard_state[] as the ISR fast path left it. */
        return;
    }
    if (key >= 0 && key < SDL_KB_STATE_SIZE) {
        /* volatile store — visible to other CPUs after IRQ completes */
        sdl_keyboard_state[(size_t)key] = is_down ? 1u : 0u;
    }

    if (desktop_is_running() && key != SDLK_UNKNOWN)
        desktop_gui_enqueue_key_sdl((int32_t)key, is_down);

    if (!events_initialized) {
        return;
    }

    SDL_Event key_event;
    key_event.type = is_down ? SDL_KEYDOWN : SDL_KEYUP;
    key_event.key.timestamp = SDL_GetTicks();
    key_event.key.sym = key;
    key_event.key.mod = sdl_keyb_modifiers_from_state();
    key_event.key.repeat = 0;
    
    push_event(&key_event);
    /* Do not map Escape to SDL_QUIT here — user apps (e.g. games) often use Esc for UI. */
}
