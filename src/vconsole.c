#include "vconsole.h"
#include "utils.h"
#include "term.h"
#include "serial.h"
#include "zeus.h"
#include "atomic.h"
#include "string.h"
#include "syscall.h"
#include "string.h"
#include "keyboard.h"
#include "desktop.h"
#include <limine.h>

extern void fb_console_save_state(void *chars, void *fg, void *bg,
                                   uint32_t *cx, uint32_t *cy,
                                   uint32_t *fgc, uint32_t *bgc,
                                   uint32_t *attrs,
                                   uint32_t *dfg, uint32_t *dbg);
extern void fb_console_restore_state(const void *chars, const void *fg, const void *bg,
                                      uint32_t cx, uint32_t cy,
                                      uint32_t fgc, uint32_t bgc,
                                      uint32_t attrs,
                                      uint32_t dfg, uint32_t dbg);
extern void fb_console_redraw_from_chargrid(void);

static vconsole_t consoles[MAX_VCONSOLES];
static volatile int active_console = 0;
static volatile int requested_console = -1; /* -1 none, -2 cycle, 0..N-1 direct */
static bool vc_initialized = false;

void vconsole_init(void) {
    if (vc_initialized) return;

    for (int i = 0; i < MAX_VCONSOLES; i++) {
        vconsole_t *vc = &consoles[i];
        vc->id = i;
        vc->cursor_x = 0;
        vc->cursor_y = 0;
        vc->fg_color = 0xFFFFFF;
        vc->bg_color = 0x000000;
        vc->text_attrs = 0;
        vc->default_fg = 0xFFFFFF;
        vc->default_bg = 0x000000;
        vc->input_len = 0;
        vc->input_ready = false;
        vc->out_head = 0;
        vc->out_tail = 0;
        vc->shell_thread = NULL;
        vc->state = VC_STATE_FREE;
        vc->initialized = false;

        for (int r = 0; r < VC_ROWS; r++) {
            for (int c = 0; c < VC_COLS; c++) {
                vc->screen_chars[r][c] = ' ';
                vc->screen_fg[r][c] = 0xFFFFFF;
                vc->screen_bg[r][c] = 0x000000;
            }
        }
        memset(vc->input_buf, 0, VC_INPUT_SIZE);

        memset(vc->kbd_buf, 0, sizeof(vc->kbd_buf));
        vc->kbd_len          = 0;
        vc->kbd_cursor       = 0;
        vc->kbd_dirty        = false;
        vc->kbd_execute      = false;
        vc->terminate_flag   = false;
        vc->force_kill_flag  = false;
        vc->program_running  = false;
        vc->raw_mode         = false;
        vc->last_ctrlc_tick  = 0;
    }

    consoles[0].state = VC_STATE_SHELL;
    consoles[0].initialized = true;
    active_console = 0;
    vc_initialized = true;

    serial_printf("[vconsole] Virtual console system initialized (%d consoles)\n",
                  MAX_VCONSOLES);
}

/* ---------- output ring helpers (for future background use) ---------- */

void vconsole_buffer_output(int id, const char *buf, size_t len) {
    if (id < 0 || id >= MAX_VCONSOLES) return;
    vconsole_t *vc = &consoles[id];

    for (size_t i = 0; i < len; i++) {
        uint32_t next = (vc->out_head + 1) % VC_OUTPUT_RING;
        if (next == vc->out_tail) {
            vc->out_tail = (vc->out_tail + 1) % VC_OUTPUT_RING;
        }
        vc->output_ring[vc->out_head] = buf[i];
        vc->out_head = next;
    }
}

void vconsole_flush_output(int id) {
    if (id < 0 || id >= MAX_VCONSOLES) return;
    vconsole_t *vc = &consoles[id];

    while (vc->out_tail != vc->out_head) {
        char ch = vc->output_ring[vc->out_tail];
        vc->out_tail = (vc->out_tail + 1) % VC_OUTPUT_RING;
        fb_console_putchar(ch);
    }
}

/* ---------- input queue ---------- */

void vconsole_submit_input(const char *buf, uint32_t len) {
    (void)buf; (void)len;
}

bool vconsole_has_input(int id) {
    if (id < 0 || id >= MAX_VCONSOLES) return false;
    return consoles[id].kbd_len > 0;
}

void vconsole_get_input(int id, char *dst, uint32_t max_len) {
    (void)id; (void)dst; (void)max_len;
}

vconsole_t *vconsole_get_by_id(int id) {
    if (id < 0 || id >= MAX_VCONSOLES) return &consoles[0];
    return &consoles[id];
}

/* ---------- switching ---------- */

int vconsole_get_active(void) {
    return active_console;
}

int vconsole_get_thread_console(void) {
    process_t *p = get_current_process();
    if (!p) {
        return vconsole_get_active();
    }
    if (p->cont_tty >= 0 && p->cont_tty < MAX_VCONSOLES) {
        return p->cont_tty;
    }
    return vconsole_get_active();
}

static void vc_save_current(void) {
    vconsole_t *vc = &consoles[active_console];
    fb_console_save_state(
        vc->screen_chars, vc->screen_fg, vc->screen_bg,
        &vc->cursor_x, &vc->cursor_y,
        &vc->fg_color, &vc->bg_color,
        &vc->text_attrs,
        &vc->default_fg, &vc->default_bg);
}

static void vc_restore(int id) {
    vconsole_t *vc = &consoles[id];
    fb_console_restore_state(
        vc->screen_chars, vc->screen_fg, vc->screen_bg,
        vc->cursor_x, vc->cursor_y,
        vc->fg_color, vc->bg_color,
        vc->text_attrs,
        vc->default_fg, vc->default_bg);
    fb_console_redraw_from_chargrid();
}

void vconsole_switch(int id) {
    if (!vc_initialized) return;
    if (id < 0 || id >= MAX_VCONSOLES) return;
    if (id == active_console) return;

    uint64_t flags = irq_save();

    vc_save_current();

    int prev = active_console;
    active_console = id;

    if (!consoles[id].initialized) {
        consoles[id].initialized = true;
        consoles[id].state = VC_STATE_SHELL;
    }

    vc_restore(id);
    vconsole_flush_output(id);
    vconsole_draw_status_bar();

    irq_restore(flags);

    if (prev != id) {
        serial_printf("[vconsole] Switched %d -> %d\n", prev, id);
        if (id == 0)
            desktop_request_redraw();
        /* Print a welcome + prompt the first time a console is visited */
        if (consoles[id].cursor_x == 0 && consoles[id].cursor_y == 0 && id != 0) {
            printf("--- tty%d  (Ctrl+F1 desktop, Ctrl+F2-F4 ttys, Ctrl+Q cycle) ---\n", id);
            printf("%s", zeus_get_prompt());
        }
    }
}

void vconsole_switch_next(void) {
    int next = (active_console + 1) % MAX_VCONSOLES;
    vconsole_switch(next);
}

void vconsole_request_switch(int id) {
    requested_console = id;
}

bool vconsole_graphics_active(void) {
    return !vc_initialized || active_console == 0;
}

void vconsole_poll_switch(void) {
    int req = requested_console;
    requested_console = -1;
    if (keyboard_get_vconsole_switch()) {
        keyboard_clear_vconsole_switch();
        if (req < 0)
            req = -2;
    }
    if (req < 0 && req != -2)
        return;
    if (req == -2)
        vconsole_switch_next();
    else
        vconsole_switch(req);
}

/* ---------- status bar ---------- */

void vconsole_draw_status_bar(void) {
    extern bool desktop_is_running(void);
    if (desktop_is_running() && vconsole_graphics_active()) return;
    extern struct limine_framebuffer *fb;
    if (!fb) return;

    uint32_t bar_height = 18;
    uint32_t bar_y = fb->height - bar_height;
    uint32_t bytes_per_pixel = fb->bpp / 8;
    uint32_t pixels_per_line = fb->pitch / bytes_per_pixel;

    for (uint32_t y = bar_y; y < fb->height; y++) {
        for (uint32_t x = 0; x < fb->width; x++) {
            uint32_t *px = (uint32_t *)fb->address + y * pixels_per_line + x;
            *px = 0x1A1A2E;
        }
    }

    uint32_t tab_w = 60;
    uint32_t gap = 4;
    uint32_t start_x = 8;

    for (int i = 0; i < MAX_VCONSOLES; i++) {
        uint32_t tx = start_x + (uint32_t)i * (tab_w + gap);
        uint32_t color = (i == active_console) ? 0x00CC66 : 0x444466;
        uint32_t text_color = (i == active_console) ? 0x000000 : 0x888899;

        for (uint32_t y = bar_y + 2; y < fb->height - 2; y++) {
            for (uint32_t x = tx; x < tx + tab_w && x < fb->width; x++) {
                uint32_t *px = (uint32_t *)fb->address + y * pixels_per_line + x;
                *px = color;
            }
        }

        extern void fb_draw_char_at_px(uint32_t px, uint32_t py, char c,
                                       uint32_t fg, uint32_t bg);
        uint32_t cx = tx + 6;
        uint32_t cy = bar_y + 2;
        fb_draw_char_at_px(cx,      cy, 't', text_color, color);
        fb_draw_char_at_px(cx + 8,  cy, 't', text_color, color);
        fb_draw_char_at_px(cx + 16, cy, 'y', text_color, color);
        fb_draw_char_at_px(cx + 24, cy, '0' + (char)i, text_color, color);
    }

    if (fb) {
        fb_mark_dirty_rows(bar_y, bar_height);
        fb_flush();
        fb_present_scanout();
    }
}
