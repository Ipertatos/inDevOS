#ifndef KTERM_H
#define KTERM_H

#include "desktop.h"
#include <stdbool.h>
#include <stdint.h>

void kterm_open(void);
bool kterm_is_open(void);
bool kterm_is_window(const desktop_window_t *win);
void kterm_on_key(int32_t keycode, bool pressed);
void kterm_on_mouse(int rx, int ry, bool down);
void kterm_on_window_destroyed(desktop_window_t *win);
bool kterm_tick(void);

#endif
