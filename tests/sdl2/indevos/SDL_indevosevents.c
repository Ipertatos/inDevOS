/*
  inDevOS video driver for SDL2: /dev/wm events -> SDL events.

  Key events carry the inDevOS desktop keycode (the kernel's SDL-1-style
  table: ASCII for printable keys, 0x100.. arrows/navigation, 0x200..
  modifiers, 0x300.. keypad).  They are translated to SDL2 scancodes (US
  layout); SDL_TEXTINPUT is synthesised from the key and the shift state.

  Copyright (C) 2026 Georgios Markakis.  zlib license (same as SDL).
*/
#include "../../SDL_internal.h"

#ifdef SDL_VIDEO_DRIVER_INDEVOS

#include <errno.h>
#include <poll.h>
#include <unistd.h>

#include "../../events/SDL_events_c.h"
#include "../../events/SDL_keyboard_c.h"
#include "../../events/SDL_mouse_c.h"
#include "../../events/SDL_windowevents_c.h"

#include "SDL_indevosvideo.h"
#include "wm_abi.h"

static SDL_Scancode KeyToScancode(int k)
{
    if (k >= 'a' && k <= 'z') {
        return (SDL_Scancode)(SDL_SCANCODE_A + (k - 'a'));
    }
    if (k >= 'A' && k <= 'Z') {
        return (SDL_Scancode)(SDL_SCANCODE_A + (k - 'A'));
    }
    if (k >= '1' && k <= '9') {
        return (SDL_Scancode)(SDL_SCANCODE_1 + (k - '1'));
    }
    if (k >= 0x301 && k <= 0x30a) {               /* keypad 0..9 */
        return k == 0x301 ? SDL_SCANCODE_KP_0 : (SDL_Scancode)(SDL_SCANCODE_KP_1 + (k - 0x302));
    }
    switch (k) {
    case '0': return SDL_SCANCODE_0;
    case ' ': return SDL_SCANCODE_SPACE;
    case '\r':
    case '\n': return SDL_SCANCODE_RETURN;
    case 0x1b: return SDL_SCANCODE_ESCAPE;
    case '\b': return SDL_SCANCODE_BACKSPACE;
    case '\t': return SDL_SCANCODE_TAB;
    case 127: return SDL_SCANCODE_DELETE;
    case '-': return SDL_SCANCODE_MINUS;
    case '=': return SDL_SCANCODE_EQUALS;
    case ',': return SDL_SCANCODE_COMMA;
    case '.': return SDL_SCANCODE_PERIOD;
    case '/': return SDL_SCANCODE_SLASH;
    case ';': return SDL_SCANCODE_SEMICOLON;
    case '\'': return SDL_SCANCODE_APOSTROPHE;
    case '`': return SDL_SCANCODE_GRAVE;
    case '[': return SDL_SCANCODE_LEFTBRACKET;
    case ']': return SDL_SCANCODE_RIGHTBRACKET;
    case '\\': return SDL_SCANCODE_BACKSLASH;
    case 0x100: return SDL_SCANCODE_UP;
    case 0x101: return SDL_SCANCODE_DOWN;
    case 0x102: return SDL_SCANCODE_LEFT;
    case 0x103: return SDL_SCANCODE_RIGHT;
    case 0x104: return SDL_SCANCODE_PAGEUP;
    case 0x105: return SDL_SCANCODE_PAGEDOWN;
    case 0x106: return SDL_SCANCODE_HOME;
    case 0x107: return SDL_SCANCODE_END;
    case 0x108: return SDL_SCANCODE_F4;
    case 0x200: return SDL_SCANCODE_LSHIFT;
    case 0x201: return SDL_SCANCODE_RSHIFT;
    case 0x202: return SDL_SCANCODE_LCTRL;
    case 0x203: return SDL_SCANCODE_RCTRL;
    case 0x204: return SDL_SCANCODE_LALT;
    case 0x205: return SDL_SCANCODE_RALT;
    case 0x206: return SDL_SCANCODE_LGUI;
    case 0x300: return SDL_SCANCODE_KP_ENTER;
    default: return SDL_SCANCODE_UNKNOWN;
    }
}

/* Character a key types on a US keyboard, 0 if none. */
static char KeyToChar(int k, Uint32 mods)
{
    static const char unshifted[] = "1234567890-=[]\\;',./`";
    static const char shifted[]   = "!@#$%^&*()_+{}|:\"<>?~";
    int shift = (mods & WM_MOD_SHIFT) != 0;
    const char *p;

    if (mods & (WM_MOD_CTRL | WM_MOD_ALT | WM_MOD_SUPER)) {
        return 0;
    }
    if (k >= 0x301 && k <= 0x30a) {
        return (char)('0' + (k - 0x301));
    }
    if (k >= 'a' && k <= 'z') {
        return (char)(shift ? k - 'a' + 'A' : k);
    }
    if (k >= 'A' && k <= 'Z') {
        return (char)(shift ? k : k - 'A' + 'a');
    }
    if (k == ' ') {
        return ' ';
    }
    if (k > 0 && k < 0x7f && (p = SDL_strchr(unshifted, k)) != NULL) {
        return shift ? shifted[p - unshifted] : (char)k;
    }
    return 0;
}

static Uint8 MapButton(int b)
{
    switch (b) {          /* WM: 1 left, 2 right, 3 middle */
    case 2: return SDL_BUTTON_RIGHT;
    case 3: return SDL_BUTTON_MIDDLE;
    default: return SDL_BUTTON_LEFT;
    }
}

void INDEVOS_InitKeyboard(void)
{
    SDL_Keycode keymap[SDL_NUM_SCANCODES];
    SDL_GetDefaultKeymap(keymap);       /* US layout */
    SDL_SetKeymap(0, keymap, SDL_NUM_SCANCODES, SDL_FALSE);
}

static void HandleEvent(_THIS, const struct wm_event *ev)
{
    SDL_Window *window = INDEVOS_FindWindow(_this, ev->win);
    SDL_WindowData *data;

    if (!window) {
        return;                         /* stale id (window recreated) */
    }
    data = (SDL_WindowData *)window->driverdata;
    switch (ev->type) {
    case WM_EV_MOTION:
        SDL_SendMouseMotion(window, 0, 0, ev->x, ev->y);
        break;
    case WM_EV_BUTTON_DOWN:
    case WM_EV_BUTTON_UP:
        SDL_SendMouseMotion(window, 0, 0, ev->x, ev->y);
        SDL_SendMouseButtonClicks(window, 0, ev->type == WM_EV_BUTTON_DOWN ? SDL_PRESSED : SDL_RELEASED,
                                  MapButton(ev->button), ev->clicks > 0 ? ev->clicks : 1);
        break;
    case WM_EV_WHEEL:
        if (ev->wheel) {
            SDL_SendMouseWheel(window, 0, 0.0f, (float)ev->wheel, SDL_MOUSEWHEEL_NORMAL);
        }
        break;
    case WM_EV_KEY_DOWN:
    case WM_EV_KEY_UP: {
        SDL_Scancode sc = KeyToScancode(ev->key);
        if (SDL_GetKeyboardFocus() != window) {
            SDL_SetKeyboardFocus(window);
        }
        if (sc != SDL_SCANCODE_UNKNOWN) {
            SDL_SendKeyboardKey(ev->type == WM_EV_KEY_DOWN ? SDL_PRESSED : SDL_RELEASED, sc);
        }
        if (ev->type == WM_EV_KEY_DOWN && SDL_EventState(SDL_TEXTINPUT, SDL_QUERY)) {
            char text[2] = { KeyToChar(ev->key, ev->mods), 0 };
            if (text[0]) {
                SDL_SendKeyboardText(text);
            }
        }
        break;
    }
    case WM_EV_FOCUS_IN:
        SDL_SetKeyboardFocus(window);
        break;
    case WM_EV_FOCUS_OUT:
        if (SDL_GetKeyboardFocus() == window) {
            SDL_SetKeyboardFocus(NULL);
        }
        if (SDL_GetMouseFocus() == window) {
            SDL_SetMouseFocus(NULL);
        }
        break;
    case WM_EV_CLOSE_REQUEST:
        SDL_SendWindowEvent(window, SDL_WINDOWEVENT_CLOSE, 0, 0);
        break;
    case WM_EV_RESIZE:
        if (ev->w > 0 && ev->h > 0) {
            /* acknowledged when SDL asks for the new framebuffer */
            data->pending_w = (int)ev->w;
            data->pending_h = (int)ev->h;
            SDL_SendWindowEvent(window, SDL_WINDOWEVENT_RESIZED, (int)ev->w, (int)ev->h);
        }
        break;
    default:
        break;
    }
}

/* Drain every queued /dev/wm event; returns how many were handled. */
static int DrainEvents(_THIS)
{
    SDL_VideoData *vd = (SDL_VideoData *)_this->driverdata;
    struct wm_event evs[16];
    int total = 0;

    for (;;) {
        ssize_t n = read(vd->fd, evs, sizeof(evs));
        int i, k;
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < (ssize_t)sizeof(evs[0])) {
            break;                       /* EAGAIN, error or nothing */
        }
        k = (int)(n / (ssize_t)sizeof(evs[0]));
        for (i = 0; i < k; i++) {
            HandleEvent(_this, &evs[i]);
        }
        total += k;
        if (k < (int)SDL_arraysize(evs)) {
            break;
        }
    }
    return total;
}

void INDEVOS_PumpEvents(_THIS)
{
    DrainEvents(_this);
}

int INDEVOS_WaitEventTimeout(_THIS, int timeout)
{
    SDL_VideoData *vd = (SDL_VideoData *)_this->driverdata;
    struct pollfd p[2];
    int r;

    p[0].fd = vd->fd;
    p[0].events = POLLIN;
    p[0].revents = 0;
    p[1].fd = vd->wake[0];
    p[1].events = POLLIN;
    p[1].revents = 0;
    r = poll(p, 2, timeout < 0 ? -1 : timeout);
    if (r < 0) {
        return errno == EINTR ? 1 : -1;   /* signal: let SDL look again */
    }
    if (r == 0) {
        return 0;
    }
    if (p[1].revents & POLLIN) {
        char buf[64];
        while (read(vd->wake[0], buf, sizeof(buf)) > 0) {
        }
    }
    if (p[0].revents & POLLIN) {
        DrainEvents(_this);
    }
    return 1;
}

void INDEVOS_SendWakeupEvent(_THIS, SDL_Window *window)
{
    SDL_VideoData *vd = (SDL_VideoData *)_this->driverdata;
    char c = 1;
    (void)window;
    if (write(vd->wake[1], &c, 1) < 0) {
        /* pipe full: a wakeup is already pending */
    }
}

#endif /* SDL_VIDEO_DRIVER_INDEVOS */

/* vi: set ts=4 sw=4 expandtab: */
