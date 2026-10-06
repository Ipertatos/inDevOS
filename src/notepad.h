#ifndef NOTEPAD_H
#define NOTEPAD_H

#include <stdbool.h>
#include <stdint.h>
#include "desktop.h"

void notepad_open(void);
bool notepad_is_open(void);
bool notepad_is_window(const desktop_window_t *win);
void notepad_on_key(int32_t keycode, bool pressed);
void notepad_on_mouse(int rx, int ry, bool down);
void notepad_on_window_destroyed(desktop_window_t *win);
/* Blink caret / flush dirty pixels. Returns true if the window was redrawn. */
bool notepad_tick(void);

#endif /* NOTEPAD_H */
