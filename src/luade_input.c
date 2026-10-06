#include "luade_input.h"
#include "keyboard.h"
#include "mem.h"
#include "atomic.h"
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

/* LuaDE Input/Event System */

#define MAX_EVENTS 256

typedef struct {
    enum {
        EVENT_NONE = 0,
        EVENT_KEY_PRESS,
        EVENT_KEY_RELEASE,
        EVENT_MOUSE_MOVE,
        EVENT_MOUSE_CLICK,
        EVENT_MOUSE_RELEASE
    } type;
    union {
        struct {
            char key;
            int scancode;
        } keyboard;
        struct {
            int x, y;
            int button; // 1=left, 2=right, 3=middle
        } mouse;
    } data;
} luade_event_t;

static luade_event_t event_queue[MAX_EVENTS];
static volatile int event_head = 0;
static volatile int event_tail = 0;
static volatile bool luade_input_initialized = false;
/** Last-seen key-down state by USB/PS2 scancode (0–255). */
static volatile uint8_t luade_key_held[256];

// Add event to queue (called from interrupt handlers) - INTERRUPT SAFE
static void add_event(luade_event_t *event) {
    // Don't add events if LuaDE input system isn't initialized
    if (!luade_input_initialized) {
        return;
    }
    
    // CRITICAL: Disable interrupts instead of spinlock (prevents deadlock)
    uint64_t flags = irq_save();
    
    int next_head = (event_head + 1) % MAX_EVENTS;
    if (next_head != event_tail) {
        event_queue[event_head] = *event;
        event_head = next_head;
    }
    
    irq_restore(flags);
}

// Lua function: input.poll_event() -> event_table or nil - INTERRUPT SAFE  
static int l_poll_event(lua_State *L) {
    // CRITICAL: Disable interrupts instead of spinlock (prevents deadlock)
    uint64_t flags = irq_save();
    
    if (event_tail == event_head) {
        irq_restore(flags);
        lua_pushnil(L);
        return 1;
    }
    
    luade_event_t event = event_queue[event_tail];  // Copy event while interrupts disabled
    event_tail = (event_tail + 1) % MAX_EVENTS;
    
    irq_restore(flags);
    
    lua_newtable(L);
    
    switch (event.type) {
        case EVENT_KEY_PRESS:
            lua_pushstring(L, "key_press");
            lua_setfield(L, -2, "type");
            lua_pushinteger(L, event.data.keyboard.key);
            lua_setfield(L, -2, "key");
            lua_pushinteger(L, event.data.keyboard.scancode);
            lua_setfield(L, -2, "scancode");
            break;
            
        case EVENT_KEY_RELEASE:
            lua_pushstring(L, "key_release");
            lua_setfield(L, -2, "type");
            lua_pushinteger(L, event.data.keyboard.key);
            lua_setfield(L, -2, "key");
            lua_pushinteger(L, event.data.keyboard.scancode);
            lua_setfield(L, -2, "scancode");
            break;
            
        case EVENT_MOUSE_MOVE:
            lua_pushstring(L, "mouse_move");
            lua_setfield(L, -2, "type");
            lua_pushinteger(L, event.data.mouse.x);
            lua_setfield(L, -2, "x");
            lua_pushinteger(L, event.data.mouse.y);
            lua_setfield(L, -2, "y");
            break;
            
        case EVENT_MOUSE_CLICK:
            lua_pushstring(L, "mouse_click");
            lua_setfield(L, -2, "type");
            lua_pushinteger(L, event.data.mouse.x);
            lua_setfield(L, -2, "x");
            lua_pushinteger(L, event.data.mouse.y);
            lua_setfield(L, -2, "y");
            lua_pushinteger(L, event.data.mouse.button);
            lua_setfield(L, -2, "button");
            break;
            
        case EVENT_MOUSE_RELEASE:
            lua_pushstring(L, "mouse_release");
            lua_setfield(L, -2, "type");
            lua_pushinteger(L, event.data.mouse.x);
            lua_setfield(L, -2, "x");
            lua_pushinteger(L, event.data.mouse.y);
            lua_setfield(L, -2, "y");
            lua_pushinteger(L, event.data.mouse.button);
            lua_setfield(L, -2, "button");
            break;
            
        default:
            lua_pushnil(L);
            return 1;
    }
    
    return 1;
}

// Lua function: input.get_key_state(key) -> boolean
static int l_get_key_state(lua_State *L) {
    int key = luaL_checkinteger(L, 1);
    int sc = key & 255;
    lua_pushboolean(L, luade_key_held[sc] != 0);
    return 1;
}

// Hook into keyboard system to generate events
void luade_keyboard_event(char key, int scancode, bool pressed) {
    // SAFETY: Validate parameters to prevent corruption
    if (!luade_input_initialized) {
        return;
    }
    
    luade_event_t event;
    event.type = pressed ? EVENT_KEY_PRESS : EVENT_KEY_RELEASE;
    event.data.keyboard.key = key;
    event.data.keyboard.scancode = scancode;
    if (scancode >= 0 && scancode < 256)
        luade_key_held[scancode] = pressed ? 1 : 0;
    add_event(&event);
}

// Mouse event generation with safety checks
void luade_mouse_event(int x, int y, int button, bool pressed) {
    // SAFETY: Validate parameters to prevent corruption
    if (!luade_input_initialized) {
        return;
    }
    
    // SAFETY: Sanity check coordinates to prevent invalid values
    if (x < 0 || x > 10000 || y < 0 || y > 10000) {
        printf("[LuaDE] WARNING: Invalid mouse coordinates: %d,%d\n", x, y);
        return;
    }
    
    luade_event_t event;
    if (button == 0) {
        event.type = EVENT_MOUSE_MOVE;
        event.data.mouse.x = x;
        event.data.mouse.y = y;
        event.data.mouse.button = 0;
    } else {
        event.type = pressed ? EVENT_MOUSE_CLICK : EVENT_MOUSE_RELEASE;
        event.data.mouse.x = x;
        event.data.mouse.y = y;
        event.data.mouse.button = button;
    }
    add_event(&event);
}

// Lua function: input.inject_mouse_move(x, y) - for testing
static int l_inject_mouse_move(lua_State *L) {
    int x = luaL_checkinteger(L, 1);
    int y = luaL_checkinteger(L, 2);
    luade_mouse_event(x, y, 0, false);
    return 0;
}

// Initialize LuaDE input system - CALL THIS BEFORE USING!
void luade_input_init(void) {
    uint64_t flags = irq_save();
    
    // Clear event queue
    event_head = 0;
    event_tail = 0;
    
    // Mark as initialized
    luade_input_initialized = true;
    
    irq_restore(flags);
    printf("[LuaDE] Input system initialized\n");
}

// Cleanup LuaDE input system
void luade_input_cleanup(void) {
    uint64_t flags = irq_save();
    
    // Mark as not initialized (stops interrupt handlers from adding events)
    luade_input_initialized = false;
    
    // Clear event queue
    event_head = 0;
    event_tail = 0;
    
    irq_restore(flags);
    printf("[LuaDE] Input system cleaned up\n");
}

// Register input functions with Lua
void luade_register_input(lua_State *L) {
    // Initialize the input system when registering
    luade_input_init();
    
    lua_newtable(L);
    
    lua_pushcfunction(L, l_poll_event);
    lua_setfield(L, -2, "poll_event");
    
    lua_pushcfunction(L, l_get_key_state);
    lua_setfield(L, -2, "get_key_state");
    
    lua_pushcfunction(L, l_inject_mouse_move);
    lua_setfield(L, -2, "inject_mouse_move");
    
    // Key constants
    lua_pushinteger(L, 27); lua_setfield(L, -2, "ESC");
    lua_pushinteger(L, 13); lua_setfield(L, -2, "ENTER");
    lua_pushinteger(L, 32); lua_setfield(L, -2, "SPACE");
    lua_pushinteger(L, 8);  lua_setfield(L, -2, "BACKSPACE");
    lua_pushinteger(L, 9);  lua_setfield(L, -2, "TAB");
    
    lua_setglobal(L, "input");
}

