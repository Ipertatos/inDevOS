/**
 * Shared SDL_Event layout — must match kernel (include/SDL.h) and user apps
 * that dequeue events via SYS_SDL_POLL_EVENT.
 *
 * sizeof(SDL_Event) is used by syscall copy_to/copy_from user buffers.
 */
#ifndef SDL_EVENT_TYPES_H
#define SDL_EVENT_TYPES_H

#include <stdint.h>

/* sym matches SDL_Keycode (enum / int) in kernel SDL.h */
typedef struct {
    uint32_t type;
    uint32_t timestamp;
    int32_t sym;
    uint16_t mod;
    uint8_t repeat;
} SDL_KeyboardEvent;

typedef struct {
    uint32_t type;
    uint32_t timestamp;
    uint32_t windowID;
    uint8_t button;
    uint8_t state;
    uint8_t clicks;
    int32_t x, y;
} SDL_MouseButtonEvent;

typedef struct {
    uint32_t type;
    uint32_t timestamp;
    uint32_t windowID;
    int32_t x, y;
    int32_t xrel, yrel;
} SDL_MouseMotionEvent;

typedef struct {
    uint32_t type;
    uint32_t timestamp;
    uint32_t windowID;
    int32_t x;
    int32_t y;
} SDL_MouseWheelEvent;

typedef struct {
    uint32_t type;
    uint32_t timestamp;
} SDL_QuitEvent;

typedef union SDL_Event {
    uint32_t type;
    SDL_QuitEvent quit;
    SDL_KeyboardEvent key;
    SDL_MouseButtonEvent button;
    SDL_MouseMotionEvent motion;
    SDL_MouseWheelEvent wheel;
} SDL_Event;

#endif /* SDL_EVENT_TYPES_H */
