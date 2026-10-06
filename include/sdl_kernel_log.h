/**
 * SDL kernel logging — writes once to serial (COM1), then to the framebuffer
 * console via fb_console_write() only (does not use printf/term_write, which
 * would duplicate every line on serial when serial is not suppressed).
 */
#ifndef SDL_KERNEL_LOG_H
#define SDL_KERNEL_LOG_H

void sdl_kernel_log(const char *fmt, ...);

#endif
