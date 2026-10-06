#include "tty.h"
#include "keyboard.h"
#include "vconsole.h"
#include "string.h"
#include "mem.h"
#include "serial.h"
#include "string.h"
#include "zeus.h"

/*
 * TTY abstraction: mediates between keyboard/input and shell.
 *
 * Input path:  keyboard ISR → VC buffer → tty_feed_line → tty_submit_line
 * Dispatch:    tty_poll() fires the registered line_callback (currently Zeus)
 *
 * This decoupling is step 1 toward making Zeus a userspace process that
 * reads from /dev/tty via standard syscalls.
 */

typedef struct {
    char line_buf[TTY_LINE_MAX];
    uint32_t line_len;
    bool line_ready;
} tty_state_t;

static tty_state_t ttys[TTY_MAX_TTYS];
static tty_line_callback_t line_callback = NULL;

void tty_init(void) {
    memset(ttys, 0, sizeof(ttys));
}

void tty_set_line_callback(tty_line_callback_t cb) {
    line_callback = cb;
}

void tty_feed_char(int tty_id, char c) {
    if (tty_id < 0 || tty_id >= TTY_MAX_TTYS) return;
    tty_state_t *t = &ttys[tty_id];
    if (t->line_len < TTY_LINE_MAX - 1) {
        t->line_buf[t->line_len++] = c;
    }
}

void tty_feed_line(int tty_id, const char *str) {
    if (tty_id < 0 || tty_id >= TTY_MAX_TTYS || !str) return;
    tty_state_t *t = &ttys[tty_id];
    uint32_t slen = (uint32_t)strlen(str);
    if (slen >= TTY_LINE_MAX) slen = TTY_LINE_MAX - 1;
    memcpy(t->line_buf, str, slen);
    t->line_buf[slen] = '\0';
    t->line_len = slen;
}

void tty_submit_line(int tty_id) {
    if (tty_id < 0 || tty_id >= TTY_MAX_TTYS) return;
    tty_state_t *t = &ttys[tty_id];
    t->line_buf[t->line_len] = '\0';
    t->line_ready = true;
}

int tty_read_line(int tty_id, char *buf, size_t max) {
    if (tty_id < 0 || tty_id >= TTY_MAX_TTYS) return -1;
    tty_state_t *t = &ttys[tty_id];
    if (!t->line_ready) return 0;

    uint32_t copy_len = t->line_len;
    if (copy_len >= max) copy_len = (uint32_t)(max - 1);
    memcpy(buf, t->line_buf, copy_len);
    buf[copy_len] = '\0';

    t->line_len = 0;
    t->line_ready = false;
    return (int)copy_len;
}

void tty_poll(void) {
    for (int i = 0; i < TTY_MAX_TTYS; i++) {
        tty_state_t *t = &ttys[i];
        if (t->line_ready && line_callback) {
            t->line_buf[t->line_len] = '\0';
            line_callback(i, t->line_buf, t->line_len);
            t->line_len = 0;
            t->line_ready = false;
        }
    }
}

const char *tty_get_prompt(void) {
    return zeus_get_prompt();
}
