#ifndef LUADE_INPUT_H
#define LUADE_INPUT_H

#include <lua.h>
#include <stdbool.h>

/* LuaDE Input/Event System Header */

// Initialize/cleanup input system  
void luade_input_init(void);
void luade_input_cleanup(void);

// Register input functions with Lua state
void luade_register_input(lua_State *L);

// Event injection functions (called from kernel)
void luade_keyboard_event(char key, int scancode, bool pressed);
void luade_mouse_event(int x, int y, int button, bool pressed);

#endif // LUADE_INPUT_H

