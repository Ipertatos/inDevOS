#include "../include/sdl_kernel_log.h"
#include "serial.h"
#include "utils.h"
#include "desktop.h"
#include <stdarg.h>
#include <stddef.h>

extern int vsnprintf(char *str, size_t size, const char *fmt, va_list ap);

#define SDL_LOG_BUF 512

void sdl_kernel_log(const char *fmt, ...) {
    char buf[SDL_LOG_BUF];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0)
        return;
    if (n >= (int)sizeof(buf))
        n = (int)sizeof(buf) - 1;
    buf[n] = '\0';

    /* COM1 only once — do not use printf/term_write here (they also emit to serial when !suppress). */
    serial_puts(buf);
    if (buf[n - 1] != '\n') {
        serial_putc('\r');
        serial_putc('\n');
    }

    /* The desktop compositor owns the framebuffer; keep SDL logs on serial. */
    if (desktop_is_running())
        return;

    /* On-screen console only (no second serial copy). */
    fb_console_write(buf, (size_t)n);
    if (buf[n - 1] != '\n')
        fb_console_write("\n", 1);
}
