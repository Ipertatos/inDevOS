#include "kterm.h"
#include "render.h"
#include "zeus.h"
#include "term.h"
#include "string.h"
#include "serial.h"
#include "utils.h"
#include "../include/SDL.h"

#define KT_W 680
#define KT_H 440
#define KT_ROWS 180
#define KT_COLS 100
#define KT_LINE 16
#define KT_HIST 24

static desktop_window_t *kt_win;
static char kt_lines[KT_ROWS][KT_COLS];
static int kt_nlines;
static int kt_scroll;
static char kt_in[160];
static int kt_inlen;
static bool kt_shift;
static bool kt_need;
static char kt_hist[KT_HIST][160];
static int kt_nhist;
static int kt_hpos;

static void kt_request(void) { kt_need = true; }

static int kt_vis(void) {
    if (!kt_win) return 1;
    int h = kt_win->content_h - 8 - 28;
    if (h < KT_LINE) return 1;
    return h / KT_LINE;
}

static void kt_follow(void) {
    int vis = kt_vis();
    int top = kt_nlines - vis;
    if (top < 0) top = 0;
    kt_scroll = top;
}

static void kt_add_line(const char *s) {
    if (kt_nlines >= KT_ROWS) {
        for (int i = 1; i < KT_ROWS; i++) {
            int j = 0;
            while (j < KT_COLS) {
                kt_lines[i - 1][j] = kt_lines[i][j];
                j++;
            }
        }
        kt_nlines = KT_ROWS - 1;
    }
    int n = 0;
    if (s) {
        while (s[n] && n < KT_COLS - 1) {
            kt_lines[kt_nlines][n] = s[n];
            n++;
        }
    }
    kt_lines[kt_nlines][n] = '\0';
    kt_nlines++;
    kt_follow();
}

static void kt_add_text(const char *s) {
    char line[KT_COLS];
    int n = 0;
    if (!s) return;
    while (*s) {
        if (*s == '\r') { s++; continue; }
        if (*s == '\n' || n >= KT_COLS - 1) {
            line[n] = '\0';
            kt_add_line(line);
            n = 0;
            if (*s == '\n') s++;
            continue;
        }
        line[n++] = *s++;
    }
    if (n) {
        line[n] = '\0';
        kt_add_line(line);
    }
}

static char kt_shifted(char c) {
    if (c >= 'a' && c <= 'z') return (char)(c - 32);
    switch (c) {
    case '1': return '!'; case '2': return '@'; case '3': return '#';
    case '4': return '$'; case '5': return '%'; case '6': return '^';
    case '7': return '&'; case '8': return '*'; case '9': return '(';
    case '0': return ')'; case '-': return '_'; case '=': return '+';
    case '[': return '{'; case ']': return '}'; case '\\': return '|';
    case ';': return ':'; case '\'': return '"'; case ',': return '<';
    case '.': return '>'; case '/': return '?'; case '`': return '~';
    default: return c;
    }
}

static void kt_redraw(void) {
    if (!kt_win || !kt_win->pixels) return;
    int w = kt_win->content_w;
    int h = kt_win->content_h;
    r_target_t t;
    r_target_init(&t, kt_win->pixels, w, h, w);
    r_fill(&t, 0, 0, w, h, 0xFF0B1020);
    int vis = kt_vis();
    int y = 6;
    for (int i = 0; i < vis; i++) {
        int idx = kt_scroll + i;
        if (idx >= 0 && idx < kt_nlines)
            r_text(&t, 8, y, kt_lines[idx], 0xFFD1FAE5);
        y += KT_LINE;
    }
    r_fill(&t, 0, h - 26, w, 26, 0xFF141824);
    r_fill(&t, 0, h - 26, w, 1, 0xFF3DDC97);
    char prompt[200];
    const char *cwd = zeus_get_cwd();
    if (!cwd) cwd = "/";
    int n = 0;
    const char *pre = "zeus:";
    while (*pre && n < 180) prompt[n++] = *pre++;
    while (*cwd && n < 160) prompt[n++] = *cwd++;
    if (n < 190) prompt[n++] = '>';
    if (n < 190) prompt[n++] = ' ';
    for (int i = 0; i < kt_inlen && n < 190; i++) prompt[n++] = kt_in[i];
    if (n < 198) prompt[n++] = '_';
    prompt[n] = '\0';
    r_text(&t, 8, h - 20, prompt, 0xFFF4F4F5);
    kt_win->dirty = true;
    kt_need = false;
}

static int kt_first_word_is(const char *line, const char *word) {
    while (*line == ' ') line++;
    int i = 0;
    while (word[i] && line[i] == word[i]) i++;
    if (word[i]) return 0;
    return line[i] == '\0' || line[i] == ' ';
}

static void kt_run(const char *line) {
    while (*line == ' ') line++;
    if (*line == '\0') return;
    if (kt_nhist < KT_HIST) {
        int i = 0;
        while (line[i] && i < 159) { kt_hist[kt_nhist][i] = line[i]; i++; }
        kt_hist[kt_nhist][i] = '\0';
        kt_nhist++;
    }
    kt_hpos = kt_nhist;
    {
        char shown[200];
        int n = 0;
        const char *cwd = zeus_get_cwd();
        const char *pre = "zeus:";
        while (*pre && n < 170) shown[n++] = *pre++;
        if (cwd) while (*cwd && n < 170) shown[n++] = *cwd++;
        if (n < 190) shown[n++] = '>';
        if (n < 190) shown[n++] = ' ';
        while (*line && n < 190) shown[n++] = *line++;
        shown[n] = '\0';
        /* line advanced; reload pointer */
    }
    /* rebuild shown from the original by walking kt_in which still holds it
     * until the caller clears it. Use kt_in. */
    {
        char shown[220];
        int n = 0;
        const char *cwd = zeus_get_cwd();
        const char *p = "zeus:";
        while (*p && n < 180) shown[n++] = *p++;
        if (cwd) while (*cwd && n < 180) shown[n++] = *cwd++;
        if (n < 200) shown[n++] = '>';
        if (n < 200) shown[n++] = ' ';
        for (int i = 0; i < kt_inlen && n < 210; i++) shown[n++] = kt_in[i];
        shown[n] = '\0';
        kt_add_line(shown);
    }
    if (kt_first_word_is(kt_in, "clear")) {
        kt_nlines = 0;
        kt_scroll = 0;
        return;
    }
    static char cap[12 * 1024];
    term_serial_suppress(true);
    term_capture_start(cap, sizeof(cap), true);
    zeus_set_no_external(1);
    zeus_process_command_service(kt_in);
    zeus_set_no_external(0);
    term_capture_stop();
    term_serial_suppress(false);
    if (cap[0]) kt_add_text(cap);
}

void kterm_open(void) {
    if (kt_win && kt_win->id) {
        desktop_focus_window(kt_win);
        kt_request();
        return;
    }
    int sw = (int)fb_get_width();
    int sh = (int)fb_get_height();
    int x = (sw - KT_W) / 2;
    int y = (sh - KT_H) / 4;
    if (x < 12) x = 12;
    if (y < 40) y = 40;
    kt_win = desktop_create_window(0, x, y, KT_W, KT_H, "Terminal");
    if (!kt_win) return;
    if (kt_nlines == 0) {
        kt_add_line("inDevOS terminal");
        kt_add_line("Zeus shell in this window. It does not switch TTYs.");
        kt_add_line("Try: help, ls, pwd, cd, cat, echo, date.  clear wipes this view.");
    }
    spinlock_lock(&kt_win->content_lock);
    kt_redraw();
    spinlock_unlock(&kt_win->content_lock);
    serial_puts("[kterm] opened\n");
}

bool kterm_is_open(void) { return kt_win && kt_win->id != 0; }
bool kterm_is_window(const desktop_window_t *win) {
    return win && kt_win && win == kt_win && kt_win->id != 0;
}
void kterm_on_window_destroyed(desktop_window_t *win) {
    if (win == kt_win) kt_win = NULL;
}

void kterm_on_key(int32_t keycode, bool pressed) {
    if (!kterm_is_open()) return;
    if (keycode == SDLK_LSHIFT || keycode == SDLK_RSHIFT) {
        kt_shift = pressed;
        return;
    }
    if (!pressed) return;
    if (keycode == SDLK_BACKSPACE) {
        if (kt_inlen > 0) kt_in[--kt_inlen] = '\0';
        kt_request();
        return;
    }
    if (keycode == SDLK_RETURN || keycode == '\n') {
        kt_in[kt_inlen] = '\0';
        kt_run(kt_in);
        kt_inlen = 0;
        kt_in[0] = '\0';
        kt_request();
        return;
    }
    if (keycode == SDLK_UP) {
        if (kt_hpos > 0) {
            kt_hpos--;
            int i = 0;
            while (kt_hist[kt_hpos][i] && i < 159) {
                kt_in[i] = kt_hist[kt_hpos][i];
                i++;
            }
            kt_in[i] = '\0';
            kt_inlen = i;
            kt_request();
        }
        return;
    }
    if (keycode == SDLK_DOWN) {
        if (kt_hpos < kt_nhist) {
            kt_hpos++;
            if (kt_hpos >= kt_nhist) {
                kt_inlen = 0;
                kt_in[0] = '\0';
            } else {
                int i = 0;
                while (kt_hist[kt_hpos][i] && i < 159) {
                    kt_in[i] = kt_hist[kt_hpos][i];
                    i++;
                }
                kt_in[i] = '\0';
                kt_inlen = i;
            }
            kt_request();
        }
        return;
    }
    if (keycode == SDLK_PAGEUP) {
        kt_scroll -= kt_vis() - 1;
        if (kt_scroll < 0) kt_scroll = 0;
        kt_request();
        return;
    }
    if (keycode == SDLK_PAGEDOWN) {
        kt_follow();
        kt_request();
        return;
    }
    if (keycode >= 32 && keycode < 127 && kt_inlen < 158) {
        char c = (char)keycode;
        if (kt_shift) c = kt_shifted(c);
        kt_in[kt_inlen++] = c;
        kt_in[kt_inlen] = '\0';
        kt_request();
    }
}

void kterm_on_mouse(int rx, int ry, bool down) {
    (void)rx; (void)ry; (void)down;
}

bool kterm_tick(void) {
    if (!kterm_is_open() || !kt_need) return false;
    spinlock_lock(&kt_win->content_lock);
    kt_redraw();
    spinlock_unlock(&kt_win->content_lock);
    return true;
}
