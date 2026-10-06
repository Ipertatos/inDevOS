#ifndef SDL_H
#define SDL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// SDL Version
#define SDL_MAJOR_VERSION   2
#define SDL_MINOR_VERSION   0
#define SDL_PATCHLEVEL      0

// SDL Subsystem flags
#define SDL_INIT_TIMER      0x00000001
#define SDL_INIT_AUDIO      0x00000010
#define SDL_INIT_VIDEO      0x00000020
#define SDL_INIT_EVENTS     0x00000040
#define SDL_INIT_EVERYTHING 0x0000FFFF

// SDL Return values
#define SDL_SUCCESS         0
#define SDL_ERROR          -1

// SDL Event types
typedef enum {
    SDL_QUIT = 0x100,
    SDL_KEYDOWN = 0x300,
    SDL_KEYUP,
    SDL_MOUSEBUTTONDOWN = 0x401,
    SDL_MOUSEBUTTONUP,
    SDL_MOUSEMOTION,
    SDL_MOUSEWHEEL
} SDL_EventType;

// SDL Key codes (simplified set)
typedef enum {
    SDLK_UNKNOWN = 0,
    SDLK_a = 'a',
    SDLK_b = 'b',
    SDLK_c = 'c',
    SDLK_d = 'd',
    SDLK_e = 'e',
    SDLK_f = 'f',
    SDLK_g = 'g',
    SDLK_h = 'h',
    SDLK_i = 'i',
    SDLK_j = 'j',
    SDLK_k = 'k',
    SDLK_l = 'l',
    SDLK_m = 'm',
    SDLK_n = 'n',
    SDLK_o = 'o',
    SDLK_p = 'p',
    SDLK_q = 'q',
    SDLK_r = 'r',
    SDLK_s = 's',
    SDLK_t = 't',
    SDLK_u = 'u',
    SDLK_v = 'v',
    SDLK_w = 'w',
    SDLK_x = 'x',
    SDLK_y = 'y',
    SDLK_z = 'z',
    SDLK_0 = '0',
    SDLK_1 = '1',
    SDLK_2 = '2',
    SDLK_3 = '3',
    SDLK_4 = '4',
    SDLK_5 = '5',
    SDLK_6 = '6',
    SDLK_7 = '7',
    SDLK_8 = '8',
    SDLK_9 = '9',
    SDLK_SPACE = ' ',
    SDLK_RETURN = '\r',
    SDLK_ESCAPE = 0x1B,
    SDLK_BACKSPACE = '\b',
    SDLK_TAB = '\t',
    SDLK_UP = 0x100,
    SDLK_DOWN,
    SDLK_LEFT,
    SDLK_RIGHT,
    SDLK_PAGEUP = 0x104,
    SDLK_PAGEDOWN = 0x105,
    SDLK_HOME = 0x106,
    SDLK_END = 0x107,
    SDLK_F4 = 0x108,
    SDLK_LSHIFT = 0x200,
    SDLK_RSHIFT,
    SDLK_LCTRL,
    SDLK_RCTRL,
    SDLK_LALT,
    SDLK_RALT,
    SDLK_LGUI,
    // Additional punctuation keys
    SDLK_PERIOD = '.',
    SDLK_COMMA = ',',
    SDLK_SEMICOLON = ';',
    SDLK_COLON = ':',
    SDLK_SLASH = '/',
    SDLK_BACKSLASH = '\\',
    SDLK_LEFTBRACKET = '[',
    SDLK_RIGHTBRACKET = ']',
    SDLK_MINUS = '-',
    SDLK_EQUALS = '=',
    SDLK_QUOTE = '\'',
    SDLK_DOUBLEQUOTE = '"',
    SDLK_LEFTPAREN = '(',
    SDLK_RIGHTPAREN = ')',
    SDLK_EXCLAIM = '!',
    SDLK_AT = '@',
    SDLK_HASH = '#',
    SDLK_DOLLAR = '$',
    SDLK_PERCENT = '%',
    SDLK_CARET = '^',
    SDLK_AMPERSAND = '&',
    SDLK_ASTERISK = '*',
    SDLK_PLUS = '+',
    SDLK_LESS = '<',
    SDLK_GREATER = '>',
    SDLK_QUESTION = '?',
    SDLK_UNDERSCORE = '_',
    // Keypad
    SDLK_KP_ENTER = 0x300,
    SDLK_KP_0 = 0x301,
    SDLK_KP_1,
    SDLK_KP_2,
    SDLK_KP_3,
    SDLK_KP_4,
    SDLK_KP_5,
    SDLK_KP_6,
    SDLK_KP_7,
    SDLK_KP_8,
    SDLK_KP_9
} SDL_Keycode;

// Key modifiers
#define KMOD_NONE   0x0000
#define KMOD_LSHIFT 0x0001
#define KMOD_RSHIFT 0x0002
#define KMOD_LCTRL  0x0040
#define KMOD_RCTRL  0x0080
#define KMOD_LALT   0x0100
#define KMOD_RALT   0x0200
#define KMOD_SHIFT  (KMOD_LSHIFT | KMOD_RSHIFT)
#define KMOD_CTRL   (KMOD_LCTRL | KMOD_RCTRL)
#define KMOD_ALT    (KMOD_LALT | KMOD_RALT)

// SDL Mouse buttons
typedef enum {
    SDL_BUTTON_LEFT = 1,
    SDL_BUTTON_MIDDLE = 2,
    SDL_BUTTON_RIGHT = 3
} SDL_MouseButton;

// SDL Pixel format
typedef struct {
    uint32_t format;
    uint8_t BitsPerPixel;
    uint8_t BytesPerPixel;
    uint32_t Rmask, Gmask, Bmask, Amask;
    uint8_t Rshift, Gshift, Bshift, Ashift;
} SDL_PixelFormat;

// SDL Rectangle
typedef struct {
    int x, y;
    int w, h;
} SDL_Rect;

// SDL Point
typedef struct {
    int x, y;
} SDL_Point;

// SDL Color
typedef struct {
    uint8_t r, g, b, a;
} SDL_Color;

// SDL Surface
typedef struct SDL_Surface {
    uint32_t flags;
    SDL_PixelFormat *format;
    int w, h;
    int pitch;
    void *pixels;
    void *userdata;
    int locked;
    int refcount;
    SDL_Rect clip_rect;
} SDL_Surface;

// SDL Window (simplified)
typedef struct SDL_Window {
    uint32_t id;
    int x, y;
    int w, h;
    uint32_t flags;
    SDL_Surface *surface;
    struct SDL_Window *next; /* kernel: intrusive list for multi-window */
} SDL_Window;

// SDL Renderer (simplified)
typedef struct SDL_Renderer {
    SDL_Window *window;
    uint32_t flags;
    SDL_Color draw_color;
} SDL_Renderer;

// SDL Texture (simplified)
typedef struct SDL_Texture {
    SDL_PixelFormat *format;
    int w, h;
    void *pixels;
} SDL_Texture;

// SDL Event structures (shared with user apps — see sdl_event_types.h)
#include "sdl_event_types.h"

// SDL Window flags
#define SDL_WINDOW_FULLSCREEN    0x00000001
#define SDL_WINDOW_OPENGL        0x00000002
#define SDL_WINDOW_SHOWN         0x00000004
#define SDL_WINDOW_HIDDEN        0x00000008
#define SDL_WINDOW_BORDERLESS    0x00000010
#define SDL_WINDOW_RESIZABLE     0x00000020
#define SDL_WINDOW_MINIMIZED     0x00000040
#define SDL_WINDOW_MAXIMIZED     0x00000080

// SDL Renderer flags
#define SDL_RENDERER_SOFTWARE    0x00000001
#define SDL_RENDERER_ACCELERATED 0x00000002
#define SDL_RENDERER_PRESENTVSYNC 0x00000004

// SDL Pixel formats
#define SDL_PIXELFORMAT_RGB24    0x17101803
#define SDL_PIXELFORMAT_RGB888   0x16161804
#define SDL_PIXELFORMAT_RGBA8888 0x16462004
#define SDL_PIXELFORMAT_BGRA8888 0x16762004

// SDL Color macros
#define SDL_ALPHA_OPAQUE 255
#define SDL_ALPHA_TRANSPARENT 0

// Core SDL functions
int SDL_Init(uint32_t flags);
void SDL_Quit(void);
uint32_t SDL_GetTicks(void);
void SDL_Delay(uint32_t ms);
const char* SDL_GetError(void);

// Video functions
SDL_Window* SDL_CreateWindow(const char* title, int x, int y, int w, int h, uint32_t flags);
void SDL_DestroyWindow(SDL_Window* window);
SDL_Surface* SDL_GetWindowSurface(SDL_Window* window);
int SDL_UpdateWindowSurface(SDL_Window* window);
void SDL_GetWindowSize(SDL_Window* window, int* w, int* h);

// Renderer functions
SDL_Renderer* SDL_CreateRenderer(SDL_Window* window, int index, uint32_t flags);
void SDL_DestroyRenderer(SDL_Renderer* renderer);
int SDL_SetRenderDrawColor(SDL_Renderer* renderer, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
int SDL_RenderClear(SDL_Renderer* renderer);
int SDL_RenderDrawPoint(SDL_Renderer* renderer, int x, int y);
int SDL_RenderDrawLine(SDL_Renderer* renderer, int x1, int y1, int x2, int y2);
int SDL_RenderDrawRect(SDL_Renderer* renderer, const SDL_Rect* rect);
int SDL_RenderFillRect(SDL_Renderer* renderer, const SDL_Rect* rect);
void SDL_RenderPresent(SDL_Renderer* renderer);

// Surface functions
SDL_Surface* SDL_CreateRGBSurface(uint32_t flags, int width, int height, int depth,
                                  uint32_t Rmask, uint32_t Gmask, uint32_t Bmask, uint32_t Amask);
void SDL_FreeSurface(SDL_Surface* surface);
int SDL_BlitSurface(SDL_Surface* src, const SDL_Rect* srcrect, SDL_Surface* dst, SDL_Rect* dstrect);
int SDL_FillRect(SDL_Surface* dst, const SDL_Rect* rect, uint32_t color);
uint32_t SDL_MapRGB(const SDL_PixelFormat* format, uint8_t r, uint8_t g, uint8_t b);
uint32_t SDL_MapRGBA(const SDL_PixelFormat* format, uint8_t r, uint8_t g, uint8_t b, uint8_t a);

// Event functions
int SDL_PollEvent(SDL_Event* event);
int SDL_WaitEvent(SDL_Event* event);
void SDL_PumpEvents(void);
SDL_Keycode SDL_Ps2ScancodeToKeycode(uint8_t scancode);

// Timer functions
uint32_t SDL_GetTicks(void);
void SDL_Delay(uint32_t ms);

// Utility functions
uint32_t SDL_GetPixel(SDL_Surface* surface, int x, int y);
void SDL_PutPixel(SDL_Surface* surface, int x, int y, uint32_t pixel);

#endif // SDL_H




