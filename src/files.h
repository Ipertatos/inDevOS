#ifndef FILES_APP_H
#define FILES_APP_H

#include "desktop.h"
#include <stdbool.h>
#include <stdint.h>

void files_open(void);
bool files_is_open(void);
bool files_is_window(const desktop_window_t *win);
void files_on_key(int32_t keycode, bool pressed);
void files_on_mouse(int rx, int ry, bool down);
void files_on_window_destroyed(desktop_window_t *win);
bool files_tick(void);

#endif
