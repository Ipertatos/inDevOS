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
void files_on_pointer(const desktop_ptr_event_t *ev);
int  files_move_into(const char *src, const char *dstdir, char *msg, int cap);
void files_on_wheel(int steps);
void files_open_path(const char *path);
void files_on_window_destroyed(desktop_window_t *win);
bool files_tick(void);

#endif
