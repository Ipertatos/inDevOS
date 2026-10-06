#ifndef TTY_H
#define TTY_H

#include "types.h"

/*
 * TTY abstraction layer
 *
 * Decouples the shell (Zeus) from the kernel main loop by providing
 * a clean interface between the input subsystem and command consumers.
 *
 * In a mature OS the shell is a userspace process reading from /dev/tty.
 * This layer provides that abstraction inside the kernel so the shell
 * logic no longer directly accesses keyboard buffers and ISR state.
 *
 * Migration path:
 *   Phase 1 (current): TTY mediates between keyboard ISR and kernel shell
 *   Phase 2: TTY exposes read/write via VFS (/dev/tty0..ttyN)
 *   Phase 3: Shell moves to userspace, reads from tty fd via syscalls
 */

#define TTY_LINE_MAX 256
#define TTY_MAX_TTYS 4

typedef void (*tty_line_callback_t)(int tty_id, const char *line, uint32_t len);

void tty_init(void);

void tty_set_line_callback(tty_line_callback_t cb);

void tty_feed_char(int tty_id, char c);

void tty_feed_line(int tty_id, const char *str);

void tty_submit_line(int tty_id);

int tty_read_line(int tty_id, char *buf, size_t max);

void tty_poll(void);

const char *tty_get_prompt(void);

#endif /* TTY_H */
