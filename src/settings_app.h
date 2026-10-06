#ifndef SETTINGS_APP_H
#define SETTINGS_APP_H

#include "desktop.h"
#include <stdbool.h>
#include <stdint.h>

void settings_app_open(void);
bool settings_app_is_open(void);
bool settings_app_is_window(const desktop_window_t *win);
void settings_app_on_key(int32_t keycode, bool pressed);
void settings_app_on_mouse(int rx, int ry, bool down);
void settings_app_on_window_destroyed(desktop_window_t *win);
bool settings_app_tick(void);

#endif
