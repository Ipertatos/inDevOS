#pragma once

#include "types.h"

#define MAX_VCONSOLES    4
#define VC_ROWS          128
#define VC_COLS          256
#define VC_INPUT_SIZE    256
#define VC_OUTPUT_RING   (64 * 1024)

typedef enum {
    VC_STATE_FREE,
    VC_STATE_SHELL,
    VC_STATE_RUNNING
} vconsole_state_t;

/* Forward declare — only used as pointer inside the struct */
struct thread;

typedef struct {
    char     screen_chars[VC_ROWS][VC_COLS];
    uint32_t screen_fg[VC_ROWS][VC_COLS];
    uint32_t screen_bg[VC_ROWS][VC_COLS];
    uint32_t cursor_x, cursor_y;
    uint32_t fg_color, bg_color;
    uint32_t text_attrs;
    uint32_t default_fg, default_bg;

    char     input_buf[VC_INPUT_SIZE];
    volatile uint32_t input_len;
    volatile bool     input_ready;

    char     output_ring[VC_OUTPUT_RING];
    volatile uint32_t out_head;
    volatile uint32_t out_tail;

    struct thread *shell_thread;
    vconsole_state_t state;
    bool     initialized;
    int      id;

    /* Per-VC keyboard input buffer (replaces the old global keyboard buffer).
     * The keyboard ISR writes into the active VC's buffer; consumers read
     * from the process's VC buffer via keyboard_consume_char(). */
    char     kbd_buf[256];
    volatile uint32_t kbd_len;
    volatile uint32_t kbd_cursor;   /* insertion point (0..kbd_len) */
    volatile bool     kbd_dirty;    /* mid-line edit: reprint from prompt */
    volatile bool     kbd_execute;

    /* Per-VC session control flags.  Replaces the former global
     * terminate_flag / force_kill_flag / is_program_running in utils.c.
     * Ctrl+C on VC N only affects processes attached to VC N. */
    volatile bool     terminate_flag;
    volatile bool     force_kill_flag;
    volatile bool     program_running;
    volatile bool     raw_mode;
    volatile uint64_t last_ctrlc_tick;
} vconsole_t;

void vconsole_init(void);
void vconsole_switch_next(void);
void vconsole_switch(int id);
/** Request a switch (0..MAX-1, or -1 to cycle). Safe from ISR. */
void vconsole_request_switch(int id);
/** True when the graphical desktop may own the framebuffer (tty0). */
bool vconsole_graphics_active(void);
int  vconsole_get_active(void);
/* Session TTY: process_t.cont_tty for current task, else active VC (see term_write). */
int  vconsole_get_thread_console(void);

void vconsole_submit_input(const char *buf, uint32_t len);
bool vconsole_has_input(int id);
void vconsole_get_input(int id, char *dst, uint32_t max_len);

void vconsole_buffer_output(int id, const char *buf, size_t len);
void vconsole_flush_output(int id);

void vconsole_draw_status_bar(void);

/** Process a deferred Ctrl+Q virtual-console switch request. */
void vconsole_poll_switch(void);

vconsole_t *vconsole_get_by_id(int id);
