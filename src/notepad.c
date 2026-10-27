/**
 * @file notepad.c
 * @brief Kernel-side desktop notepad (edit, open, save)
 */
#include "notepad.h"
#include "render.h"
#include "mem.h"
#include "vfs.h"
#include "clipboard.h"
#include "hpet.h"
#include "serial.h"
#include "string.h"
#include "utils.h"
#include "../include/SDL.h"

#define NP_MAX       32768
#define NP_PATH_MAX  128
#define NP_DEFAULT_PATH "/home/user/Documents/notes.txt"
#define NP_WIN_W     640
#define NP_WIN_H     420
#define NP_TOOL_H    36
#define NP_STATUS_H  22
#define NP_PAD       12
#define NP_LINE_H    18
#define NP_COLS_MIN  8

#define NP_COL_BG      0xFF141824
#define NP_COL_TOOL    0xFF1A2035
#define NP_COL_TEXT    0xFFF4F4F5
#define NP_COL_MUTED   0xFF94A3B8
#define NP_COL_ACCENT  0xFF3DDC97
#define NP_COL_BTN     0xFF1C2436
#define NP_COL_BTN_ON  0xFF243044
#define NP_COL_SEL     0xFF1E3A4A
#define NP_COL_CARET   0xFF3DDC97
#define NP_COL_LINE    0xFF2A3148
#define NP_COL_PATH    0xFF0E121A

enum { NP_MODE_EDIT = 0, NP_MODE_PATH = 1 };

typedef struct {
    int start;
    int len;
} np_line_t;

static desktop_window_t *np_win;
static char  np_buf[NP_MAX];
static int   np_len;
static int   np_cur;
static int   np_sel;          /* selection anchor; == np_cur means none */
static int   np_scroll;
static char  np_path[NP_PATH_MAX];
static char  np_status[80];
static int   np_mode;
static bool  np_modified;
static bool  np_shift;
static bool  np_ctrl;
static bool  np_caret_on;
static bool  np_need_draw;
static uint64_t np_last_blink;

extern uint32_t fb_get_width(void);
extern uint32_t fb_get_height(void);

static void np_request_draw(void)
{
    np_need_draw = true;
    if (np_win) np_win->dirty = true;
}

static void np_set_status(const char *s)
{
    int i = 0;
    if (!s) { np_status[0] = '\0'; return; }
    while (s[i] && i < (int)sizeof(np_status) - 1) {
        np_status[i] = s[i];
        i++;
    }
    np_status[i] = '\0';
}

static void np_copy_path(const char *s)
{
    int i = 0;
    if (!s) s = NP_DEFAULT_PATH;
    while (s[i] && i < NP_PATH_MAX - 1) {
        np_path[i] = s[i];
        i++;
    }
    np_path[i] = '\0';
}

static int np_sel_lo(void) { return np_cur < np_sel ? np_cur : np_sel; }
static int np_sel_hi(void) { return np_cur > np_sel ? np_cur : np_sel; }
static bool np_has_sel(void) { return np_cur != np_sel; }

static int np_cols(void)
{
    if (!np_win) return 60;
    int c = (np_win->content_w - 2 * NP_PAD) / 8;
    return c < NP_COLS_MIN ? NP_COLS_MIN : c;
}

static int np_vis_rows(void)
{
    if (!np_win) return 16;
    int h = np_win->content_h - NP_TOOL_H - NP_STATUS_H;
    int rows = h / NP_LINE_H;
    return rows < 1 ? 1 : rows;
}

static int np_build_lines(np_line_t *out, int max)
{
    int cols = np_cols();
    int n = 0;
    int i = 0;
    while (i <= np_len && n < max) {
        int start = i;
        int col = 0;
        while (i < np_len && np_buf[i] != '\n' && col < cols) {
            i++;
            col++;
        }
        out[n].start = start;
        out[n].len = i - start;
        n++;
        if (i < np_len && np_buf[i] == '\n')
            i++;
        else if (i >= np_len)
            break;
    }
    if (n == 0 && max > 0) {
        out[0].start = 0;
        out[0].len = 0;
        n = 1;
    }
    return n;
}

static int np_line_at(int pos, const np_line_t *lines, int nlines)
{
    int last = 0;
    for (int i = 0; i < nlines; i++) {
        int end = lines[i].start + lines[i].len;
        if (pos < end || (pos == end && pos <= lines[i].start + lines[i].len))
            return i;
        if (pos >= lines[i].start)
            last = i;
    }
    return last;
}

static void np_ensure_caret_visible(const np_line_t *lines, int nlines)
{
    int li = np_line_at(np_cur, lines, nlines);
    int rows = np_vis_rows();
    if (li < np_scroll) np_scroll = li;
    if (li >= np_scroll + rows) np_scroll = li - rows + 1;
    if (np_scroll < 0) np_scroll = 0;
}

static void np_delete_range(int lo, int hi)
{
    if (lo < 0) lo = 0;
    if (hi > np_len) hi = np_len;
    if (hi <= lo) return;
    memmove(np_buf + lo, np_buf + hi, (size_t)(np_len - hi));
    np_len -= (hi - lo);
    np_buf[np_len] = '\0';
    np_cur = lo;
    np_sel = lo;
    np_modified = true;
}

static void np_insert(const char *s, int n)
{
    if (!s || n <= 0) return;
    if (np_has_sel())
        np_delete_range(np_sel_lo(), np_sel_hi());
    if (np_len + n >= NP_MAX)
        n = NP_MAX - 1 - np_len;
    if (n <= 0) return;
    memmove(np_buf + np_cur + n, np_buf + np_cur, (size_t)(np_len - np_cur));
    memcpy(np_buf + np_cur, s, (size_t)n);
    np_len += n;
    np_cur += n;
    np_sel = np_cur;
    np_buf[np_len] = '\0';
    np_modified = true;
}

static void np_insert_char(char c)
{
    np_insert(&c, 1);
}

static void np_new_doc(void)
{
    np_len = 0;
    np_cur = 0;
    np_sel = 0;
    np_scroll = 0;
    np_buf[0] = '\0';
    np_modified = false;
    np_copy_path(NP_DEFAULT_PATH);
    np_set_status("New file");
    np_mode = NP_MODE_EDIT;
    np_request_draw();
}

static void np_do_open(void)
{
    void *raw = NULL;
    int n = vfs_read_file_alloc(np_path, &raw, NP_MAX - 1);
    if (n < 0 || !raw) {
        np_set_status("Open failed");
        np_request_draw();
        return;
    }
    int w = 0;
    const char *s = (const char *)raw;
    for (int i = 0; i < n && w < NP_MAX - 1; i++) {
        if (s[i] == '\r') continue;
        np_buf[w++] = s[i];
    }
    np_buf[w] = '\0';
    np_len = w;
    np_cur = 0;
    np_sel = 0;
    np_scroll = 0;
    np_modified = false;
    kfree(raw);
    np_set_status("Opened");
    np_mode = NP_MODE_EDIT;
    np_request_draw();
}

static void np_do_save(void)
{
    if (np_path[0] == '\0')
        np_copy_path(NP_DEFAULT_PATH);
    if (vfs_ensure_parent(np_path) < 0) {
        np_set_status("Cannot create folders");
        np_request_draw();
        return;
    }
    if (vfs_write_file(np_path, np_buf, (uint32_t)np_len) == 0) {
        np_modified = false;
        np_set_status("Saved");
    } else {
        np_set_status("Save failed");
    }
    np_mode = NP_MODE_EDIT;
    np_request_draw();
}

static void np_copy_sel(void)
{
    int lo = np_has_sel() ? np_sel_lo() : 0;
    int hi = np_has_sel() ? np_sel_hi() : np_len;
    if (hi <= lo) return;
    char tmp = np_buf[hi];
    np_buf[hi] = '\0';
    clipboard_set_text(np_buf + lo);
    np_buf[hi] = tmp;
    np_set_status("Copied");
    np_request_draw();
}

static void np_paste(void)
{
    char tmp[4096];
    int n = clipboard_get_text(tmp, sizeof(tmp));
    if (n <= 0) {
        np_set_status("Clipboard empty");
        np_request_draw();
        return;
    }
    np_insert(tmp, n);
    np_set_status("Pasted");
    np_request_draw();
}

static void np_move_h(int dir, bool extend)
{
    int next = np_cur + dir;
    if (next < 0) next = 0;
    if (next > np_len) next = np_len;
    np_cur = next;
    if (!extend) np_sel = np_cur;
}

static void np_move_v(int dir, bool extend)
{
    np_line_t lines[1024];
    int n = np_build_lines(lines, 1024);
    int li = np_line_at(np_cur, lines, n);
    int col = np_cur - lines[li].start;
    li += dir;
    if (li < 0) li = 0;
    if (li >= n) li = n - 1;
    if (col > lines[li].len) col = lines[li].len;
    np_cur = lines[li].start + col;
    if (!extend) np_sel = np_cur;
}

static char np_shifted(char c)
{
    if (c >= 'a' && c <= 'z') return (char)(c - 32);
    switch (c) {
    case '1': return '!';
    case '2': return '@';
    case '3': return '#';
    case '4': return '$';
    case '5': return '%';
    case '6': return '^';
    case '7': return '&';
    case '8': return '*';
    case '9': return '(';
    case '0': return ')';
    case '-': return '_';
    case '=': return '+';
    case '[': return '{';
    case ']': return '}';
    case '\\': return '|';
    case ';': return ':';
    case '\'': return '"';
    case ',': return '<';
    case '.': return '>';
    case '/': return '?';
    case '`': return '~';
    default: return c;
    }
}

static void np_draw_btn(r_target_t *t, int x, int y, int w, int h,
                        const char *label, bool hot)
{
    r_rrect(t, x, y, w, h, 5, hot ? NP_COL_BTN_ON : NP_COL_BTN);
    r_rrect_outline(t, x, y, w, h, 5, hot ? NP_COL_ACCENT : NP_COL_LINE);
    int tw = r_text_width(label);
    r_text(t, x + (w - tw) / 2, y + (h - 16) / 2, label, NP_COL_TEXT);
}

static void np_redraw(void)
{
    if (!np_win || !np_win->pixels) return;

    int w = np_win->content_w;
    int h = np_win->content_h;
    r_target_t t;
    r_target_init(&t, np_win->pixels, w, h, w);

    r_fill(&t, 0, 0, w, h, NP_COL_BG);
    r_fill(&t, 0, 0, w, NP_TOOL_H, NP_COL_TOOL);
    r_fill(&t, 0, NP_TOOL_H - 1, w, 1, NP_COL_LINE);

    np_draw_btn(&t, 8, 7, 52, 22, "New", false);
    np_draw_btn(&t, 64, 7, 52, 22, "Open", false);
    np_draw_btn(&t, 120, 7, 52, 22, "Save", false);

    int path_x = 180;
    int path_w = w - path_x - 8;
    if (path_w < 80) path_w = 80;
    r_rrect(&t, path_x, 7, path_w, 22, 5,
            np_mode == NP_MODE_PATH ? 0xFF1E2740 : NP_COL_PATH);
    r_rrect_outline(&t, path_x, 7, path_w, 22, 5,
                    np_mode == NP_MODE_PATH ? NP_COL_ACCENT : NP_COL_LINE);
    r_text(&t, path_x + 6, 10, np_path, NP_COL_TEXT);

    np_line_t lines[1024];
    int nlines = np_build_lines(lines, 1024);
    np_ensure_caret_visible(lines, nlines);

    int rows = np_vis_rows();
    int text_y = NP_TOOL_H + 4;
    int lo = np_sel_lo();
    int hi = np_sel_hi();

    for (int r = 0; r < rows; r++) {
        int li = np_scroll + r;
        if (li >= nlines) break;
        int y = text_y + r * NP_LINE_H;
        const np_line_t *ln = &lines[li];
        if (np_has_sel()) {
            int a = ln->start;
            int b = ln->start + ln->len;
            int s0 = lo > a ? lo : a;
            int s1 = hi < b ? hi : b;
            if (s1 > s0) {
                r_fill(&t, NP_PAD + (s0 - ln->start) * 8, y,
                       (s1 - s0) * 8, NP_LINE_H - 2, NP_COL_SEL);
            }
        }
        char tmp[128];
        int n = ln->len;
        if (n > 127) n = 127;
        memcpy(tmp, np_buf + ln->start, (size_t)n);
        tmp[n] = '\0';
        r_text(&t, NP_PAD, y, tmp, NP_COL_TEXT);

        if (np_caret_on && np_mode == NP_MODE_EDIT &&
            np_cur >= ln->start && np_cur <= ln->start + ln->len) {
            int cx = NP_PAD + (np_cur - ln->start) * 8;
            r_fill(&t, cx, y, 2, 16, NP_COL_CARET);
        }
    }

    r_fill(&t, 0, h - NP_STATUS_H, w, NP_STATUS_H, NP_COL_TOOL);
    r_fill(&t, 0, h - NP_STATUS_H, w, 1, NP_COL_LINE);

    int line = np_line_at(np_cur, lines, nlines) + 1;
    int col = np_cur - lines[np_line_at(np_cur, lines, nlines)].start + 1;
    char meta[64];
    int mi = 0;
    const char *p = np_modified ? "modified  " : "saved     ";
    while (*p) meta[mi++] = *p++;
    meta[mi++] = 'L';
    /* small itoa */
    {
        int v = line;
        char d[8]; int di = 0;
        if (v <= 0) d[di++] = '0';
        while (v > 0 && di < 7) { d[di++] = (char)('0' + (v % 10)); v /= 10; }
        while (di--) meta[mi++] = d[di];
    }
    meta[mi++] = ':';
    {
        int v = col;
        char d[8]; int di = 0;
        if (v <= 0) d[di++] = '0';
        while (v > 0 && di < 7) { d[di++] = (char)('0' + (v % 10)); v /= 10; }
        while (di--) meta[mi++] = d[di];
    }
    meta[mi] = '\0';
    r_text(&t, NP_PAD, h - NP_STATUS_H + 3, meta, NP_COL_MUTED);
    if (np_status[0])
        r_text(&t, w / 2, h - NP_STATUS_H + 3, np_status, NP_COL_ACCENT);

    np_need_draw = false;
    np_win->dirty = true;
}

void notepad_open(void)
{
    if (np_win && np_win->id != 0) {
        desktop_focus_window(np_win);
        np_request_draw();
        return;
    }

    int sw = (int)fb_get_width();
    int sh = (int)fb_get_height();
    int x = (sw - NP_WIN_W) / 2;
    int y = (sh - NP_WIN_H) / 3;
    if (x < 16) x = 16;
    if (y < 40) y = 40;

    np_win = desktop_create_window(0, x, y, NP_WIN_W, NP_WIN_H, "Notepad");
    if (!np_win) {
        serial_puts("[notepad] could not create window\n");
        return;
    }
    if (np_len == 0 && np_path[0] == '\0')
        np_new_doc();
    np_caret_on = true;
    np_last_blink = hpet_get_ticks();
    np_set_status("Ready  Ctrl+S save  Ctrl+O open");
    np_redraw();
    serial_puts("[notepad] opened\n");
}

bool notepad_is_open(void)
{
    return np_win && np_win->id != 0;
}

bool notepad_is_window(const desktop_window_t *win)
{
    return win && np_win && win == np_win && np_win->id != 0;
}

void notepad_on_window_destroyed(desktop_window_t *win)
{
    if (win == np_win)
        np_win = NULL;
}

void notepad_on_key(int32_t keycode, bool pressed)
{
    if (!notepad_is_open()) return;

    if (keycode == SDLK_LSHIFT || keycode == SDLK_RSHIFT) {
        np_shift = pressed;
        return;
    }
    if (keycode == SDLK_LCTRL || keycode == SDLK_RCTRL) {
        np_ctrl = pressed;
        return;
    }
    if (!pressed) return;

    if (np_mode == NP_MODE_PATH) {
        if (keycode == SDLK_RETURN || keycode == SDLK_KP_ENTER) {
            np_mode = NP_MODE_EDIT;
            np_request_draw();
            return;
        }
        if (keycode == SDLK_ESCAPE) {
            np_mode = NP_MODE_EDIT;
            np_request_draw();
            return;
        }
        if (keycode == SDLK_BACKSPACE) {
            int n = 0;
            while (np_path[n]) n++;
            if (n > 0) np_path[n - 1] = '\0';
            np_request_draw();
            return;
        }
        if (keycode >= 32 && keycode < 127) {
            char c = (char)keycode;
            if (np_shift) c = np_shifted(c);
            int n = 0;
            while (np_path[n]) n++;
            if (n < NP_PATH_MAX - 1) {
                np_path[n] = c;
                np_path[n + 1] = '\0';
            }
            np_request_draw();
        }
        return;
    }

    if (np_ctrl) {
        if (keycode == 's') { np_do_save(); return; }
        if (keycode == 'o') { np_do_open(); return; }
        if (keycode == 'n') { np_new_doc(); return; }
        if (keycode == 'c') { np_copy_sel(); return; }
        if (keycode == 'x') {
            np_copy_sel();
            if (np_has_sel()) np_delete_range(np_sel_lo(), np_sel_hi());
            np_request_draw();
            return;
        }
        if (keycode == 'v') { np_paste(); return; }
        if (keycode == 'a') {
            np_sel = 0;
            np_cur = np_len;
            np_request_draw();
            return;
        }
        return;
    }

    if (keycode == SDLK_BACKSPACE) {
        if (np_has_sel())
            np_delete_range(np_sel_lo(), np_sel_hi());
        else if (np_cur > 0)
            np_delete_range(np_cur - 1, np_cur);
        np_request_draw();
        return;
    }
    if (keycode == SDLK_RETURN || keycode == SDLK_KP_ENTER) {
        np_insert_char('\n');
        np_request_draw();
        return;
    }
    if (keycode == SDLK_TAB) {
        np_insert("    ", 4);
        np_request_draw();
        return;
    }
    if (keycode == SDLK_LEFT)  { np_move_h(-1, np_shift); np_request_draw(); return; }
    if (keycode == SDLK_RIGHT) { np_move_h(1, np_shift); np_request_draw(); return; }
    if (keycode == SDLK_UP)    { np_move_v(-1, np_shift); np_request_draw(); return; }
    if (keycode == SDLK_DOWN)  { np_move_v(1, np_shift); np_request_draw(); return; }
    if (keycode == SDLK_HOME) {
        np_line_t lines[1024];
        int n = np_build_lines(lines, 1024);
        int li = np_line_at(np_cur, lines, n);
        np_cur = lines[li].start;
        if (!np_shift) np_sel = np_cur;
        np_request_draw();
        return;
    }
    if (keycode == SDLK_END) {
        np_line_t lines[1024];
        int n = np_build_lines(lines, 1024);
        int li = np_line_at(np_cur, lines, n);
        np_cur = lines[li].start + lines[li].len;
        if (!np_shift) np_sel = np_cur;
        np_request_draw();
        return;
    }
    if (keycode == SDLK_PAGEUP)   { np_move_v(-np_vis_rows(), np_shift); np_request_draw(); return; }
    if (keycode == SDLK_PAGEDOWN) { np_move_v(np_vis_rows(), np_shift); np_request_draw(); return; }

    if (keycode >= 32 && keycode < 127) {
        char c = (char)keycode;
        if (np_shift) c = np_shifted(c);
        np_insert_char(c);
        np_request_draw();
    }
}

void notepad_on_mouse(int rx, int ry, bool down)
{
    if (!notepad_is_open() || !down) return;

    if (ry < NP_TOOL_H) {
        if (rx >= 8 && rx < 60) { np_new_doc(); return; }
        if (rx >= 64 && rx < 116) { np_do_open(); return; }
        if (rx >= 120 && rx < 172) { np_do_save(); return; }
        if (rx >= 180) {
            np_mode = NP_MODE_PATH;
            np_request_draw();
            return;
        }
        return;
    }
    if (ry >= (np_win->content_h - NP_STATUS_H))
        return;

    np_mode = NP_MODE_EDIT;
    np_line_t lines[1024];
    int nlines = np_build_lines(lines, 1024);
    int row = (ry - NP_TOOL_H - 4) / NP_LINE_H;
    int li = np_scroll + row;
    if (li < 0) li = 0;
    if (li >= nlines) li = nlines - 1;
    int col = (rx - NP_PAD) / 8;
    if (col < 0) col = 0;
    if (col > lines[li].len) col = lines[li].len;
    np_cur = lines[li].start + col;
    if (!np_shift) np_sel = np_cur;
    np_caret_on = true;
    np_request_draw();
}

bool notepad_tick(void)
{
    if (!notepad_is_open()) return false;
    uint64_t now = hpet_get_ticks();
    if (now - np_last_blink >= 500) {
        np_last_blink = now;
        np_caret_on = !np_caret_on;
        np_need_draw = true;
    }
    if (!np_need_draw) return false;
    klock(&np_win->content_lock);
    np_redraw();
    kunlock(&np_win->content_lock);
    return true;
}

/* Open @path in the notepad window (launch service).  Refuses to replace a
 * document with unsaved changes. */
void notepad_open_path(const char *path)
{
    if (np_modified && np_win && np_win->id) {
        desktop_toast("Notepad has unsaved changes; save first (Ctrl+S)");
        desktop_focus_window(np_win);
        return;
    }
    np_copy_path(path);
    notepad_open();
    np_do_open();
    serial_printf("[notepad] open %s (%d bytes)\n", np_path, np_len);
}
