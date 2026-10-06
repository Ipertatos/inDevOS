/*
 * tui.c - Text User Interface Menu System for inDevOS
 *
 * Renders menuconfig-style TUI menus using ANSI escape sequences.
 * Handles keyboard input via raw mode for responsive navigation.
 */

#include "tui.h"
#include "keyboard.h"
#include "mem.h"
#include "utils.h"
#include "serial.h"
#include "term.h"
#include "string.h"
#include <stdarg.h>
#include "stdio.h"

/* Forward declarations for kernel functions */
extern int snprintf(char *str, size_t size, const char *fmt, ...);
extern int vsnprintf(char *str, size_t size, const char *fmt, va_list ap);
extern int printf(const char *fmt, ...);
extern void *memset(void *s, int c, size_t n);
extern void *memcpy(void *dest, const void *src, size_t n);
extern size_t strlen(const char *s);
extern void term_write(const char *buf, size_t len);

/* ================================================================
 * Buffered output (avoids thousands of printf calls = faster redraws)
 * ================================================================ */
#define TUI_BUF_SIZE 16384
static char tui_out_buf[TUI_BUF_SIZE];
static int  tui_out_pos = 0;

static void tui_buf_flush(void) {
    if (tui_out_pos > 0) {
        term_write(tui_out_buf, (size_t)tui_out_pos);
        tui_out_pos = 0;
    }
}

static void tui_buf_emit(const char *s, int len) {
    if (len <= 0) return;
    int space = TUI_BUF_SIZE - tui_out_pos;
    if (len > space) {
        tui_buf_flush();
        space = TUI_BUF_SIZE;
    }
    if (len > space) len = space;
    for (int i = 0; i < len; i++)
        tui_out_buf[tui_out_pos++] = s[i];
}

static void tui_buf_printf(const char *fmt, ...) {
    char tmp[128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) tui_buf_emit(tmp, n);
}

static void tui_buf_repeat(char c, int n) {
    int space = TUI_BUF_SIZE - tui_out_pos;
    if (n > space) {
        tui_buf_flush();
        space = TUI_BUF_SIZE;
    }
    if (n > space) n = space;
    for (int i = 0; i < n; i++)
        tui_out_buf[tui_out_pos++] = c;
}

/* Forward decl for tui_strlen (used in buffered helpers) */
static int tui_strlen(const char *s);

/* Buffered ANSI helpers (used during render) */
static void tui_buf_goto(int row, int col) {
    tui_buf_printf("\033[%d;%dH", row, col);
}
static void tui_buf_set_colors(int fg, int bg) {
    int fg_code = (fg < 8) ? (30 + fg) : (90 + fg - 8);
    int bg_code = (bg < 8) ? (40 + bg) : (100 + bg - 8);
    tui_buf_printf("\033[%d;%dm", fg_code, bg_code);
}
static void tui_buf_set_bold(void) { tui_buf_emit("\033[1m", 4); }
static void tui_buf_reset_attrs(void) { tui_buf_emit("\033[0m", 4); }
static void tui_buf_fill_row(int row, int col, int width, char ch, int fg, int bg) {
    tui_buf_set_colors(fg, bg);
    tui_buf_goto(row, col);
    tui_buf_repeat(ch, width);
}
static void tui_buf_draw_hline(int row, int col, int width, int fg, int bg) {
    tui_buf_set_colors(fg, bg);
    tui_buf_goto(row, col);
    tui_buf_emit("+", 1);
    tui_buf_repeat('-', width - 2);
    tui_buf_emit("+", 1);
}
static void tui_buf_print_padded(int row, int col, const char *text, int width,
                                  int fg, int bg, bool center) {
    tui_buf_set_colors(fg, bg);
    tui_buf_goto(row, col);
    int text_len = tui_strlen(text);
    if (text_len > width) text_len = width;
    if (center) {
        int pad_left = (width - text_len) / 2;
        int pad_right = width - text_len - pad_left;
        tui_buf_repeat(' ', pad_left);
        tui_buf_emit(text, text_len);
        tui_buf_repeat(' ', pad_right);
    } else {
        tui_buf_emit(text, text_len);
        tui_buf_repeat(' ', width - text_len);
    }
}

/* ================================================================
 * ANSI escape helpers (unbuffered - for setup/teardown)
 * ================================================================ */

static void tui_goto(int row, int col) {
    printf("\033[%d;%dH", row, col);
}

static void tui_clear_screen(void) {
    printf("\033[2J");
}

static void tui_reset_attrs(void) {
    printf("\033[0m");
}

static void tui_set_bold(void) {
    printf("\033[1m");
}

/* Set foreground + background using ANSI 16-color codes.
 * fg/bg are ANSI color numbers: 0-7 normal, 8-15 bright. */
static void tui_set_colors(int fg, int bg) {
    /* Map 0-7 to 30-37, 8-15 to 90-97 for foreground */
    int fg_code = (fg < 8) ? (30 + fg) : (90 + fg - 8);
    int bg_code = (bg < 8) ? (40 + bg) : (100 + bg - 8);
    printf("\033[%d;%dm", fg_code, bg_code);
}

/* Hide / show cursor */
static void tui_hide_cursor(void) {
    printf("\033[?25l");
}

static void tui_show_cursor(void) {
    printf("\033[?25h");
}

/* Save / restore cursor and screen */
static void tui_save_screen(void) {
    printf("\033[?1049h");  /* Switch to alternate screen buffer */
}

static void tui_restore_screen(void) {
    printf("\033[?1049l");  /* Switch back to main screen buffer */
}

/* ================================================================
 * ANSI color constants for themes
 * ================================================================ */
enum {
    CLR_BLACK   = 0,  CLR_RED     = 1,  CLR_GREEN   = 2,
    CLR_YELLOW  = 3,  CLR_BLUE    = 4,  CLR_MAGENTA = 5,
    CLR_CYAN    = 6,  CLR_WHITE   = 7,
    CLR_BRIGHT_BLACK = 8,  CLR_BRIGHT_RED    = 9,
    CLR_BRIGHT_GREEN = 10, CLR_BRIGHT_YELLOW = 11,
    CLR_BRIGHT_BLUE  = 12, CLR_BRIGHT_MAGENTA= 13,
    CLR_BRIGHT_CYAN  = 14, CLR_BRIGHT_WHITE  = 15,
};

/* ================================================================
 * Theme presets
 * ================================================================ */

static tui_theme_t theme_blue = {
    .bg         = CLR_BLUE,
    .border_fg  = CLR_BRIGHT_WHITE, .border_bg  = CLR_CYAN,
    .title_fg   = CLR_BRIGHT_WHITE, .title_bg   = CLR_CYAN,
    .item_fg    = CLR_BLACK,        .item_bg    = CLR_WHITE,
    .sel_fg     = CLR_BRIGHT_WHITE, .sel_bg     = CLR_BLUE,
    .disabled_fg= CLR_BRIGHT_BLACK,
    .help_fg    = CLR_BRIGHT_WHITE, .help_bg    = CLR_BLUE,
    .btn_fg     = CLR_BRIGHT_WHITE, .btn_bg     = CLR_CYAN,
    .btn_sel_fg = CLR_BRIGHT_WHITE, .btn_sel_bg = CLR_BLACK,
    .tag_fg     = CLR_BRIGHT_YELLOW,
};

static tui_theme_t theme_cyan = {
    .bg         = CLR_CYAN,
    .border_fg  = CLR_WHITE,        .border_bg  = CLR_BRIGHT_CYAN,
    .title_fg   = CLR_BRIGHT_WHITE, .title_bg   = CLR_BRIGHT_CYAN,
    .item_fg    = CLR_BLACK,        .item_bg    = CLR_BRIGHT_WHITE,
    .sel_fg     = CLR_BRIGHT_WHITE, .sel_bg     = CLR_CYAN,
    .disabled_fg= CLR_BRIGHT_BLACK,
    .help_fg    = CLR_BRIGHT_WHITE, .help_bg    = CLR_CYAN,
    .btn_fg     = CLR_BLACK,        .btn_bg     = CLR_BRIGHT_CYAN,
    .btn_sel_fg = CLR_BRIGHT_WHITE, .btn_sel_bg = CLR_BLACK,
    .tag_fg     = CLR_BRIGHT_YELLOW,
};

static tui_theme_t theme_dark = {
    .bg         = CLR_BLACK,
    .border_fg  = CLR_BRIGHT_WHITE, .border_bg  = CLR_BRIGHT_BLACK,
    .title_fg   = CLR_BRIGHT_CYAN,  .title_bg   = CLR_BRIGHT_BLACK,
    .item_fg    = CLR_WHITE,        .item_bg    = CLR_BRIGHT_BLACK,
    .sel_fg     = CLR_BLACK,        .sel_bg     = CLR_CYAN,
    .disabled_fg= CLR_BRIGHT_BLACK,
    .help_fg    = CLR_BRIGHT_WHITE, .help_bg    = CLR_BLACK,
    .btn_fg     = CLR_WHITE,        .btn_bg     = CLR_BRIGHT_BLACK,
    .btn_sel_fg = CLR_BLACK,        .btn_sel_bg = CLR_CYAN,
    .tag_fg     = CLR_BRIGHT_GREEN,
};

static tui_theme_t theme_green = {
    .bg         = CLR_BLACK,
    .border_fg  = CLR_BRIGHT_GREEN, .border_bg  = CLR_BLACK,
    .title_fg   = CLR_BRIGHT_GREEN, .title_bg   = CLR_BLACK,
    .item_fg    = CLR_GREEN,        .item_bg    = CLR_BLACK,
    .sel_fg     = CLR_BLACK,        .sel_bg     = CLR_GREEN,
    .disabled_fg= CLR_BRIGHT_BLACK,
    .help_fg    = CLR_GREEN,        .help_bg    = CLR_BLACK,
    .btn_fg     = CLR_GREEN,        .btn_bg     = CLR_BLACK,
    .btn_sel_fg = CLR_BLACK,        .btn_sel_bg = CLR_GREEN,
    .tag_fg     = CLR_BRIGHT_GREEN,
};

/* ================================================================
 * Key reading
 * ================================================================ */

int tui_poll_key(void) {
    /* Check special keys first */
    if (keyboard_get_escape())      { keyboard_clear_escape();      return TUI_KEY_ESCAPE; }
    if (keyboard_get_arrow_up())    { keyboard_clear_arrow_up();    return TUI_KEY_UP; }
    if (keyboard_get_arrow_down())  { keyboard_clear_arrow_down();  return TUI_KEY_DOWN; }
    if (keyboard_get_arrow_left())  { keyboard_clear_arrow_left();  return TUI_KEY_LEFT; }
    if (keyboard_get_arrow_right()) { keyboard_clear_arrow_right(); return TUI_KEY_RIGHT; }
    if (keyboard_get_pgup_pressed()){ keyboard_clear_pgup();        return TUI_KEY_PGUP; }
    if (keyboard_get_pgdn_pressed()){ keyboard_clear_pgdn();        return TUI_KEY_PGDN; }
    if (keyboard_get_home())        { keyboard_clear_home();        return TUI_KEY_HOME; }
    if (keyboard_get_end())         { keyboard_clear_end();         return TUI_KEY_END; }
    if (keyboard_get_f1())          { keyboard_clear_f1();          return TUI_KEY_F1; }
    if (keyboard_get_f5())          { keyboard_clear_f5();          return TUI_KEY_F5; }
    if (keyboard_get_tab_requested()){ keyboard_clear_tab_requested(); return TUI_KEY_TAB; }

    /* Check for ASCII characters in the buffer */
    int ch = keyboard_consume_char();
    if (ch < 0) return TUI_KEY_NONE;

    if (ch == '\n' || ch == '\r') return TUI_KEY_ENTER;
    if (ch == ' ')                return TUI_KEY_SPACE;
    if (ch == '\b' || ch == 127)  return TUI_KEY_BACKSPACE;
    if (ch == '\t')               return TUI_KEY_TAB;

    /* Return ASCII directly (a-z, 0-9, etc.) */
    return ch;
}

int tui_read_key(void) {
    bool was_raw = keyboard_get_raw_mode();
    if (!was_raw) keyboard_set_raw_mode(true);

    int key;
    while ((key = tui_poll_key()) == TUI_KEY_NONE) {
        __asm__ volatile("hlt");  /* sleep until next interrupt */
    }

    if (!was_raw) keyboard_set_raw_mode(false);
    return key;
}

/* ================================================================
 * String helpers
 * ================================================================ */

static int tui_strlen(const char *s) {
    if (!s) return 0;
    int len = 0;
    while (s[len]) len++;
    return len;
}

static void tui_strncpy(char *dst, const char *src, int max) {
    int i;
    for (i = 0; i < max - 1 && src && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/* ================================================================
 * Menu lifecycle
 * ================================================================ */

tui_menu_t *tui_menu_create(const char *title, const char *subtitle) {
    tui_menu_t *menu = (tui_menu_t *)kalloc(sizeof(tui_menu_t));
    if (!menu) return NULL;

    memset(menu, 0, sizeof(tui_menu_t));
    tui_strncpy(menu->title,    title    ? title    : "", TUI_MAX_TITLE);
    tui_strncpy(menu->subtitle, subtitle ? subtitle : "", TUI_MAX_HELP);
    menu->item_count    = 0;
    menu->selected      = 0;
    menu->scroll_offset = 0;
    menu->button_count  = 0;
    menu->active_button = 0;

    /* Apply default blue theme */
    memcpy(&menu->theme, &theme_blue, sizeof(tui_theme_t));

    return menu;
}

void tui_menu_destroy(tui_menu_t *menu) {
    if (menu) kfree(menu);
}

/* ================================================================
 * Adding items
 * ================================================================ */

int tui_menu_add(tui_menu_t *menu, const char *label, int type, int tag) {
    if (!menu || menu->item_count >= TUI_MAX_ITEMS) return -1;

    int idx = menu->item_count++;
    tui_item_t *item = &menu->items[idx];
    memset(item, 0, sizeof(tui_item_t));
    tui_strncpy(item->label, label ? label : "", TUI_MAX_LABEL);
    item->type    = type;
    item->tag     = tag;
    item->value   = 0;
    item->group   = 0;
    item->enabled = (type != TUI_ITEM_SEPARATOR && type != TUI_ITEM_LABEL);
    item->submenu = NULL;
    item->userdata= NULL;
    return idx;
}

int tui_menu_add_submenu(tui_menu_t *menu, const char *label,
                         tui_menu_t *submenu, int tag) {
    int idx = tui_menu_add(menu, label, TUI_ITEM_SUBMENU, tag);
    if (idx >= 0) {
        menu->items[idx].submenu = submenu;
    }
    return idx;
}

int tui_menu_add_separator(tui_menu_t *menu) {
    return tui_menu_add(menu, "", TUI_ITEM_SEPARATOR, -1);
}

void tui_menu_set_help(tui_menu_t *menu, int index, const char *help) {
    if (!menu || index < 0 || index >= menu->item_count) return;
    tui_strncpy(menu->items[index].help, help ? help : "", TUI_MAX_HELP);
}

void tui_menu_set_group(tui_menu_t *menu, int index, int group) {
    if (!menu || index < 0 || index >= menu->item_count) return;
    menu->items[index].group = group;
}

void tui_menu_set_value(tui_menu_t *menu, int index, int value) {
    if (!menu || index < 0 || index >= menu->item_count) return;
    menu->items[index].value = value;
}

void tui_menu_set_enabled(tui_menu_t *menu, int index, bool enabled) {
    if (!menu || index < 0 || index >= menu->item_count) return;
    menu->items[index].enabled = enabled;
}

/* ================================================================
 * Buttons
 * ================================================================ */

void tui_menu_add_button(tui_menu_t *menu, const char *label, int key) {
    if (!menu || menu->button_count >= TUI_MAX_BUTTONS) return;
    tui_button_t *btn = &menu->buttons[menu->button_count++];
    tui_strncpy(btn->label, label ? label : "", TUI_MAX_BUTTON_LEN);
    btn->key = key;
}

void tui_menu_add_default_buttons(tui_menu_t *menu) {
    tui_menu_add_button(menu, "Select", TUI_KEY_ENTER);
    tui_menu_add_button(menu, "Exit",   TUI_KEY_ESCAPE);
    tui_menu_add_button(menu, "Help",   TUI_KEY_F1);
}

/* ================================================================
 * Theming
 * ================================================================ */

void tui_menu_set_theme(tui_menu_t *menu, int theme_id) {
    if (!menu) return;
    switch (theme_id) {
        case TUI_THEME_BLUE:  memcpy(&menu->theme, &theme_blue,  sizeof(tui_theme_t)); break;
        case TUI_THEME_CYAN:  memcpy(&menu->theme, &theme_cyan,  sizeof(tui_theme_t)); break;
        case TUI_THEME_DARK:  memcpy(&menu->theme, &theme_dark,  sizeof(tui_theme_t)); break;
        case TUI_THEME_GREEN: memcpy(&menu->theme, &theme_green, sizeof(tui_theme_t)); break;
    }
}

tui_theme_t *tui_menu_get_theme(tui_menu_t *menu) {
    return menu ? &menu->theme : NULL;
}

/* ================================================================
 * Querying
 * ================================================================ */

int tui_menu_get_selected_tag(tui_menu_t *menu) {
    if (!menu || menu->selected < 0 || menu->selected >= menu->item_count)
        return -1;
    return menu->items[menu->selected].tag;
}

int tui_menu_get_selected_index(tui_menu_t *menu) {
    return menu ? menu->selected : -1;
}

int tui_menu_get_value(tui_menu_t *menu, int tag) {
    if (!menu) return -1;
    for (int i = 0; i < menu->item_count; i++) {
        if (menu->items[i].tag == tag)
            return menu->items[i].value;
    }
    return -1;
}

tui_item_t *tui_menu_get_item(tui_menu_t *menu, int tag) {
    if (!menu) return NULL;
    for (int i = 0; i < menu->item_count; i++) {
        if (menu->items[i].tag == tag)
            return &menu->items[i];
    }
    return NULL;
}

/* ================================================================
 * Rendering
 * ================================================================ */

/* Render the full menu to the screen (uses buffered output for speed) */
static void tui_render(tui_menu_t *menu) {
    tui_theme_t *t = &menu->theme;

    int screen_w = (int)console_get_width();
    int screen_h = (int)console_get_height();

    /* Calculate window dimensions */
    int win_w = screen_w - 4;  /* 2 char margin on each side */
    if (win_w > 76) win_w = 76;
    if (win_w < 30) win_w = 30;

    /* Calculate subtitle lines */
    int subtitle_lines = 0;
    if (menu->subtitle[0]) {
        subtitle_lines = 3;  /* border + text + border */
    }

    /* Calculate button row height */
    int button_rows = (menu->button_count > 0) ? 2 : 0; /* border + buttons */

    /* Available rows for items */
    int overhead = 2 /* top border + title */ + subtitle_lines
                 + 2 /* item area borders */ + button_rows + 1 /* bottom border */;
    int max_item_rows = screen_h - 4 - overhead; /* 2 char margin top+bottom */
    if (max_item_rows > menu->item_count) max_item_rows = menu->item_count;
    if (max_item_rows < 3) max_item_rows = 3;
    if (max_item_rows > 20) max_item_rows = 20;
    menu->visible_rows = max_item_rows;

    int win_h = overhead + max_item_rows;
    int win_x = (screen_w - win_w) / 2 + 1; /* 1-based column */
    int win_y = (screen_h - win_h) / 2 + 1; /* 1-based row    */
    if (win_y < 1) win_y = 1;

    menu->win_x = win_x;
    menu->win_y = win_y;
    menu->win_w = win_w;
    menu->win_h = win_h;

    /* Ensure scroll is valid */
    if (menu->selected < menu->scroll_offset)
        menu->scroll_offset = menu->selected;
    if (menu->selected >= menu->scroll_offset + max_item_rows)
        menu->scroll_offset = menu->selected - max_item_rows + 1;

    /* Fill background (one row at a time with buffered repeat) */
    tui_buf_set_colors(CLR_WHITE, t->bg);
    for (int r = 1; r <= screen_h; r++) {
        tui_buf_goto(r, 1);
        tui_buf_repeat(' ', screen_w);
    }

    int row = win_y;
    int inner_w = win_w - 2; /* Width inside the borders */

    /* ---- Top border + title ---- */
    tui_buf_draw_hline(row, win_x, win_w, t->border_fg, t->border_bg);
    /* Overlay title centered on top border */
    if (menu->title[0]) {
        int title_len = tui_strlen(menu->title);
        int title_x = win_x + (win_w - title_len - 2) / 2;
        tui_buf_set_colors(t->title_fg, t->title_bg);
        tui_buf_set_bold();
        tui_buf_goto(row, title_x);
        tui_buf_emit(" ", 1);
        tui_buf_emit(menu->title, title_len);
        tui_buf_emit(" ", 1);
        tui_buf_reset_attrs();
    }
    row++;

    /* ---- Subtitle / help text area ---- */
    if (subtitle_lines > 0) {
        /* Subtitle text row(s) */
        tui_buf_set_colors(t->border_fg, t->border_bg);
        tui_buf_goto(row, win_x);
        tui_buf_emit("|", 1);
        tui_buf_set_colors(t->help_fg, t->help_bg);
        tui_buf_print_padded(row, win_x + 1, menu->subtitle, inner_w, t->help_fg, t->help_bg, true);
        tui_buf_set_colors(t->border_fg, t->border_bg);
        tui_buf_goto(row, win_x + win_w - 1);
        tui_buf_emit("|", 1);
        row++;

        /* Show navigation instructions */
        tui_buf_set_colors(t->border_fg, t->border_bg);
        tui_buf_goto(row, win_x);
        tui_buf_emit("|", 1);
        tui_buf_set_colors(t->help_fg, t->help_bg);
        tui_buf_print_padded(row, win_x + 1, "Arrow keys navigate. Enter selects. Esc exits.",
                             inner_w, t->help_fg, t->help_bg, true);
        tui_buf_set_colors(t->border_fg, t->border_bg);
        tui_buf_goto(row, win_x + win_w - 1);
        tui_buf_emit("|", 1);
        row++;

        /* Separator line */
        tui_buf_draw_hline(row, win_x, win_w, t->border_fg, t->border_bg);
        row++;
    }

    /* ---- Item area ---- */
    /* Top border of item area */
    /* (already drawn as separator or top border) - draw another if no subtitle */
    if (subtitle_lines == 0) {
        /* Already drew top border, items start next line */
    }

    /* Render each visible item */
    for (int vi = 0; vi < max_item_rows; vi++) {
        int idx = menu->scroll_offset + vi;
        tui_buf_set_colors(t->border_fg, t->border_bg);
        tui_buf_goto(row, win_x);
        tui_buf_emit("|", 1);

        if (idx < menu->item_count) {
            tui_item_t *item = &menu->items[idx];
            bool is_selected = (idx == menu->selected);

            if (item->type == TUI_ITEM_SEPARATOR) {
                tui_buf_set_colors(t->border_fg, t->item_bg);
                tui_buf_repeat('-', inner_w);
            } else if (item->type == TUI_ITEM_LABEL) {
                tui_buf_set_colors(t->disabled_fg, t->item_bg);
                tui_buf_print_padded(row, win_x + 1, item->label, inner_w,
                                    t->disabled_fg, t->item_bg, true);
            } else {
                int fg = is_selected ? t->sel_fg : t->item_fg;
                int bg_c = is_selected ? t->sel_bg : t->item_bg;
                if (!item->enabled) fg = t->disabled_fg;
                tui_buf_set_colors(fg, bg_c);

                char prefix[8] = "    ";
                char suffix[8] = "";

                switch (item->type) {
                    case TUI_ITEM_CHECKBOX:
                        if (item->value)
                            prefix[0] = '[', prefix[1] = '*', prefix[2] = ']', prefix[3] = ' ';
                        else
                            prefix[0] = '[', prefix[1] = ' ', prefix[2] = ']', prefix[3] = ' ';
                        break;
                    case TUI_ITEM_RADIO:
                        if (item->value)
                            prefix[0] = '(', prefix[1] = 'X', prefix[2] = ')', prefix[3] = ' ';
                        else
                            prefix[0] = '(', prefix[1] = ' ', prefix[2] = ')', prefix[3] = ' ';
                        break;
                    case TUI_ITEM_SUBMENU:
                        suffix[0] = ' '; suffix[1] = '-'; suffix[2] = '-';
                        suffix[3] = '-'; suffix[4] = '>'; suffix[5] = '\0';
                        break;
                    default:
                        prefix[0] = ' '; prefix[1] = ' ';
                        prefix[2] = ' '; prefix[3] = ' ';
                        break;
                }

                int prefix_len = tui_strlen(prefix);
                int suffix_len = tui_strlen(suffix);
                int label_len  = tui_strlen(item->label);
                int avail = inner_w - prefix_len - suffix_len;
                if (label_len > avail) label_len = avail;
                int pad = inner_w - prefix_len - label_len - suffix_len;

                if (item->type == TUI_ITEM_CHECKBOX || item->type == TUI_ITEM_RADIO) {
                    if (is_selected) tui_buf_set_colors(t->tag_fg, bg_c);
                    tui_buf_emit(prefix, 3);
                    tui_buf_set_colors(fg, bg_c);
                    tui_buf_emit(prefix + 3, 1);
                } else {
                    tui_buf_emit(prefix, prefix_len);
                }
                tui_buf_emit(item->label, label_len);
                tui_buf_repeat(' ', pad);
                tui_buf_emit(suffix, suffix_len);
            }
        } else {
            tui_buf_set_colors(t->item_fg, t->item_bg);
            tui_buf_repeat(' ', inner_w);
        }

        tui_buf_set_colors(t->border_fg, t->border_bg);
        tui_buf_goto(row, win_x + win_w - 1);
        tui_buf_emit("|", 1);
        row++;
    }

    /* Scroll indicators */
    if (menu->scroll_offset > 0) {
        int arrow_row = win_y + (subtitle_lines > 0 ? subtitle_lines + 1 : 1);
        tui_buf_set_colors(t->border_fg, t->border_bg);
        tui_buf_goto(arrow_row, win_x + win_w - 2);
        tui_buf_emit("^", 1);
    }
    if (menu->scroll_offset + max_item_rows < menu->item_count) {
        tui_buf_set_colors(t->border_fg, t->border_bg);
        tui_buf_goto(row - 1, win_x + win_w - 2);
        tui_buf_emit("v", 1);
    }

    /* ---- Bottom border of item area ---- */
    tui_buf_draw_hline(row, win_x, win_w, t->border_fg, t->border_bg);
    row++;

    /* ---- Button bar ---- */
    if (menu->button_count > 0) {
        tui_buf_set_colors(t->border_fg, t->border_bg);
        tui_buf_goto(row, win_x);
        tui_buf_emit("|", 1);

        int total_btn_w = 0;
        for (int b = 0; b < menu->button_count; b++)
            total_btn_w += tui_strlen(menu->buttons[b].label) + 4;
        total_btn_w += (menu->button_count - 1) * 2;
        int btn_pad = (inner_w - total_btn_w) / 2;
        if (btn_pad < 0) btn_pad = 0;

        tui_buf_set_colors(t->btn_fg, t->border_bg);
        tui_buf_repeat(' ', btn_pad);

        for (int b = 0; b < menu->button_count; b++) {
            bool is_active = (b == menu->active_button);
            if (is_active)
                tui_buf_set_colors(t->btn_sel_fg, t->btn_sel_bg);
            else
                tui_buf_set_colors(t->btn_fg, t->btn_bg);
            tui_buf_emit("< ", 2);
            tui_buf_emit(menu->buttons[b].label, tui_strlen(menu->buttons[b].label));
            tui_buf_emit(" >", 2);
            tui_buf_set_colors(t->btn_fg, t->border_bg);
            if (b < menu->button_count - 1) tui_buf_emit("  ", 2);
        }

        int used = btn_pad;
        for (int b = 0; b < menu->button_count; b++)
            used += tui_strlen(menu->buttons[b].label) + 4;
        used += (menu->button_count - 1) * 2;
        tui_buf_repeat(' ', inner_w - used > 0 ? inner_w - used : 0);

        tui_buf_set_colors(t->border_fg, t->border_bg);
        tui_buf_goto(row, win_x + win_w - 1);
        tui_buf_emit("|", 1);
        row++;

        tui_buf_draw_hline(row, win_x, win_w, t->border_fg, t->border_bg);
        row++;
    }

    /* ---- Help text for selected item ---- */
    if (menu->selected >= 0 && menu->selected < menu->item_count) {
        tui_item_t *sel = &menu->items[menu->selected];
        if (sel->help[0]) {
            tui_buf_set_colors(t->help_fg, t->bg);
            tui_buf_print_padded(row + 1, win_x, sel->help, win_w, t->help_fg, t->bg, true);
        }
    }

    tui_buf_reset_attrs();
    tui_buf_flush();
}

/* ================================================================
 * Navigation helpers
 * ================================================================ */

/* Move selection to the next selectable item downward */
static void tui_move_down(tui_menu_t *menu) {
    int start = menu->selected;
    int idx = start + 1;
    while (idx < menu->item_count) {
        if (menu->items[idx].enabled) {
            menu->selected = idx;
            return;
        }
        idx++;
    }
    /* Wrap around or stay */
}

/* Move selection to the previous selectable item upward */
static void tui_move_up(tui_menu_t *menu) {
    int idx = menu->selected - 1;
    while (idx >= 0) {
        if (menu->items[idx].enabled) {
            menu->selected = idx;
            return;
        }
        idx--;
    }
}

/* Move to the first selectable item */
static void tui_move_home(tui_menu_t *menu) {
    for (int i = 0; i < menu->item_count; i++) {
        if (menu->items[i].enabled) {
            menu->selected = i;
            return;
        }
    }
}

/* Move to the last selectable item */
static void tui_move_end(tui_menu_t *menu) {
    for (int i = menu->item_count - 1; i >= 0; i--) {
        if (menu->items[i].enabled) {
            menu->selected = i;
            return;
        }
    }
}

/* Toggle checkbox or radio */
static void tui_toggle_item(tui_menu_t *menu) {
    if (menu->selected < 0 || menu->selected >= menu->item_count) return;
    tui_item_t *item = &menu->items[menu->selected];

    if (item->type == TUI_ITEM_CHECKBOX) {
        item->value = !item->value;
    } else if (item->type == TUI_ITEM_RADIO) {
        /* Turn off all items in the same group, turn on this one */
        for (int i = 0; i < menu->item_count; i++) {
            if (menu->items[i].type == TUI_ITEM_RADIO &&
                menu->items[i].group == item->group) {
                menu->items[i].value = 0;
            }
        }
        item->value = 1;
    }
}

/* ================================================================
 * Main menu loop
 * ================================================================ */

int tui_menu_run(tui_menu_t *menu) {
    if (!menu) return TUI_RESULT_EXIT;

    /* Add default buttons if none set */
    if (menu->button_count == 0) {
        tui_menu_add_default_buttons(menu);
    }

    /* Ensure selection is on a valid item */
    if (menu->item_count > 0) {
        tui_move_home(menu);
    }

    /* Enter raw mode and save screen; suppress serial to avoid spam */
    term_serial_suppress(true);
    keyboard_set_raw_mode(true);
    tui_save_screen();
    tui_hide_cursor();

    bool running = true;
    menu->result = TUI_RESULT_EXIT;

    /* Initial render */
    tui_render(menu);

    while (running) {
        int key = tui_read_key();

        switch (key) {
            case TUI_KEY_UP:
            case 'k':
                tui_move_up(menu);
                tui_render(menu);
                break;

            case TUI_KEY_DOWN:
            case 'j':
                tui_move_down(menu);
                tui_render(menu);
                break;

            case TUI_KEY_HOME:
                tui_move_home(menu);
                tui_render(menu);
                break;

            case TUI_KEY_END:
                tui_move_end(menu);
                tui_render(menu);
                break;

            case TUI_KEY_PGUP: {
                for (int i = 0; i < menu->visible_rows; i++)
                    tui_move_up(menu);
                tui_render(menu);
                break;
            }

            case TUI_KEY_PGDN: {
                for (int i = 0; i < menu->visible_rows; i++)
                    tui_move_down(menu);
                tui_render(menu);
                break;
            }

            case TUI_KEY_SPACE:
                tui_toggle_item(menu);
                tui_render(menu);
                break;

            case TUI_KEY_ENTER: {
                tui_item_t *sel = &menu->items[menu->selected];
                if (sel->type == TUI_ITEM_CHECKBOX || sel->type == TUI_ITEM_RADIO) {
                    tui_toggle_item(menu);
                    tui_render(menu);
                } else if (sel->type == TUI_ITEM_SUBMENU && sel->submenu) {
                    /* Recurse into submenu */
                    tui_menu_run((tui_menu_t *)sel->submenu);
                    /* Re-render this menu when we return */
                    tui_render(menu);
                } else {
                    menu->result = TUI_RESULT_SELECT;
                    running = false;
                }
                break;
            }

            case TUI_KEY_ESCAPE:
            case 'q':
                menu->result = TUI_RESULT_EXIT;
                running = false;
                break;

            case TUI_KEY_F1:
            case 'h':
            case '?':
                menu->result = TUI_RESULT_HELP;
                running = false;
                break;

            case TUI_KEY_LEFT:
                /* Move active button left */
                if (menu->active_button > 0) {
                    menu->active_button--;
                    tui_render(menu);
                }
                break;

            case TUI_KEY_RIGHT:
                /* Move active button right */
                if (menu->active_button < menu->button_count - 1) {
                    menu->active_button++;
                    tui_render(menu);
                }
                break;

            case TUI_KEY_TAB:
                /* Cycle active button */
                menu->active_button = (menu->active_button + 1) % menu->button_count;
                tui_render(menu);
                break;

            default:
                /* Ignore other keys */
                break;
        }
    }

    /* Restore screen and keyboard */
    tui_show_cursor();
    tui_restore_screen();
    keyboard_set_raw_mode(false);
    term_serial_suppress(false);

    return menu->result;
}

/* ================================================================
 * Convenience dialogs
 * ================================================================ */

void tui_message_box(const char *title, const char *message) {
    tui_menu_t *dlg = tui_menu_create(title, message);
    if (!dlg) return;

    tui_menu_add(dlg, "OK", TUI_ITEM_TEXT, 0);
    dlg->button_count = 0;
    tui_menu_add_button(dlg, "OK", TUI_KEY_ENTER);

    tui_menu_run(dlg);
    tui_menu_destroy(dlg);
}

int tui_yes_no(const char *title, const char *question) {
    tui_menu_t *dlg = tui_menu_create(title, question);
    if (!dlg) return 0;

    tui_menu_add(dlg, "Yes", TUI_ITEM_TEXT, 1);
    tui_menu_add(dlg, "No",  TUI_ITEM_TEXT, 0);
    dlg->button_count = 0;
    tui_menu_add_button(dlg, "Yes", TUI_KEY_ENTER);
    tui_menu_add_button(dlg, "No",  TUI_KEY_ESCAPE);

    tui_menu_run(dlg);
    int result = tui_menu_get_selected_tag(dlg);
    tui_menu_destroy(dlg);
    return result;
}

int tui_input_box(const char *title, const char *prompt,
                  char *buffer, int max_len) {
    if (!buffer || max_len < 2) return 0;

    /* Use a simple inline input with the TUI rendering */
    term_serial_suppress(true);
    keyboard_set_raw_mode(true);
    tui_save_screen();
    tui_hide_cursor();

    tui_theme_t *t = &theme_blue;

    int screen_w = (int)console_get_width();
    int screen_h = (int)console_get_height();

    int win_w = 50;
    if (win_w > screen_w - 4) win_w = screen_w - 4;
    int win_h = 8;
    int win_x = (screen_w - win_w) / 2 + 1;
    int win_y = (screen_h - win_h) / 2 + 1;
    int inner_w = win_w - 2;

    int buf_len = tui_strlen(buffer);
    bool done = false;
    int result = 0;

    while (!done) {
        /* Fill background */
        tui_buf_set_colors(CLR_WHITE, t->bg);
        for (int r = 1; r <= screen_h; r++) {
            tui_buf_goto(r, 1);
            tui_buf_repeat(' ', screen_w);
        }

        int row = win_y;

        /* Top border + title */
        tui_buf_draw_hline(row, win_x, win_w, t->border_fg, t->border_bg);
        if (title && title[0]) {
            int tl = tui_strlen(title);
            int tx = win_x + (win_w - tl - 2) / 2;
            tui_buf_set_colors(t->title_fg, t->title_bg);
            tui_buf_set_bold();
            tui_buf_goto(row, tx);
            tui_buf_emit(" ", 1);
            tui_buf_emit(title, tl);
            tui_buf_emit(" ", 1);
            tui_buf_reset_attrs();
        }
        row++;

        /* Prompt line */
        tui_buf_set_colors(t->border_fg, t->border_bg);
        tui_buf_goto(row, win_x);
        tui_buf_emit("|", 1);
        tui_buf_print_padded(row, win_x + 1, prompt ? prompt : "", inner_w,
                             t->item_fg, t->item_bg, false);
        tui_buf_set_colors(t->border_fg, t->border_bg);
        tui_buf_goto(row, win_x + win_w - 1);
        tui_buf_emit("|", 1);
        row++;

        /* Input field */
        tui_buf_set_colors(t->border_fg, t->border_bg);
        tui_buf_goto(row, win_x);
        tui_buf_emit("|", 1);
        tui_buf_set_colors(CLR_BRIGHT_WHITE, CLR_BLACK);
        tui_buf_emit(" ", 1);
        int field_w = inner_w - 2;
        int disp_start = 0;
        if (buf_len > field_w) disp_start = buf_len - field_w;
        int vis_len = buf_len - disp_start;
        if (vis_len > field_w) vis_len = field_w;
        tui_buf_emit(buffer + disp_start, vis_len);
        tui_buf_repeat(' ', field_w - vis_len);
        tui_buf_emit(" ", 1);
        tui_buf_set_colors(t->border_fg, t->border_bg);
        tui_buf_goto(row, win_x + win_w - 1);
        tui_buf_emit("|", 1);
        row++;

        /* Empty line */
        tui_buf_set_colors(t->border_fg, t->border_bg);
        tui_buf_goto(row, win_x);
        tui_buf_emit("|", 1);
        tui_buf_set_colors(t->item_fg, t->item_bg);
        tui_buf_repeat(' ', inner_w);
        tui_buf_set_colors(t->border_fg, t->border_bg);
        tui_buf_goto(row, win_x + win_w - 1);
        tui_buf_emit("|", 1);
        row++;

        /* Button row */
        tui_buf_set_colors(t->border_fg, t->border_bg);
        tui_buf_goto(row, win_x);
        tui_buf_emit("|", 1);
        tui_buf_set_colors(t->btn_fg, t->border_bg);
        int ok_x = inner_w / 2 - 8;
        tui_buf_repeat(' ', ok_x > 0 ? ok_x : 0);
        tui_buf_set_colors(t->btn_sel_fg, t->btn_sel_bg);
        tui_buf_emit("< OK >", 6);
        tui_buf_set_colors(t->btn_fg, t->border_bg);
        tui_buf_emit("  ", 2);
        tui_buf_set_colors(t->btn_fg, t->btn_bg);
        tui_buf_emit("< Cancel >", 10);
        tui_buf_set_colors(t->btn_fg, t->border_bg);
        int used2 = ok_x + 6 + 2 + 10;
        tui_buf_repeat(' ', inner_w - used2 > 0 ? inner_w - used2 : 0);
        tui_buf_set_colors(t->border_fg, t->border_bg);
        tui_buf_goto(row, win_x + win_w - 1);
        tui_buf_emit("|", 1);
        row++;

        /* Bottom border */
        tui_buf_draw_hline(row, win_x, win_w, t->border_fg, t->border_bg);
        tui_buf_flush();

        /* Show blinking cursor on input field */
        tui_show_cursor();
        int cursor_col = win_x + 2 + (buf_len - disp_start);
        if (cursor_col > win_x + win_w - 3)
            cursor_col = win_x + win_w - 3;
        tui_goto(win_y + 2, cursor_col);

        tui_reset_attrs();

        /* Read key */
        int key = tui_read_key();
        switch (key) {
            case TUI_KEY_ENTER:
                result = 1;
                done = true;
                break;
            case TUI_KEY_ESCAPE:
                result = 0;
                done = true;
                break;
            case TUI_KEY_BACKSPACE:
                if (buf_len > 0) {
                    buffer[--buf_len] = '\0';
                }
                break;
            default:
                /* Printable ASCII */
                if (key >= 32 && key < 127 && buf_len < max_len - 1) {
                    buffer[buf_len++] = (char)key;
                    buffer[buf_len] = '\0';
                }
                break;
        }
    }

    tui_hide_cursor();
    tui_show_cursor();
    tui_restore_screen();
    keyboard_set_raw_mode(false);
    term_serial_suppress(false);

    return result;
}

/* ================================================================
 * Installer progress screen
 * ================================================================ */
#define TUI_PROGRESS_MAX_STEPS 12
#define TUI_PROGRESS_DESC_LEN  64
static char tui_progress_desc[TUI_PROGRESS_MAX_STEPS][TUI_PROGRESS_DESC_LEN];
static int  tui_progress_total = 0;

void tui_progress_begin(const char *title) {
    term_serial_suppress(true);
    keyboard_set_raw_mode(true);
    tui_save_screen();
    tui_hide_cursor();
    tui_clear_screen();
    memset(tui_progress_desc, 0, sizeof(tui_progress_desc));
    tui_progress_total = 0;

    tui_theme_t *t = &theme_cyan;
    int screen_w = (int)console_get_width();
    int screen_h = (int)console_get_height();
    int win_w = 54;
    if (win_w > screen_w - 4) win_w = screen_w - 4;
    int win_x = (screen_w - win_w) / 2 + 1;
    int win_y = (screen_h - 12) / 2 + 1;
    if (win_y < 2) win_y = 2;

    tui_buf_set_colors(CLR_WHITE, t->bg);
    for (int r = 1; r <= screen_h; r++) {
        tui_buf_goto(r, 1);
        tui_buf_repeat(' ', screen_w);
    }
    int row = win_y;
    tui_buf_draw_hline(row, win_x, win_w, t->border_fg, t->border_bg);
    if (title && title[0]) {
        int tl = tui_strlen(title);
        int tx = win_x + (win_w - tl - 2) / 2;
        tui_buf_set_colors(t->title_fg, t->title_bg);
        tui_buf_set_bold();
        tui_buf_goto(row, tx);
        tui_buf_emit(" ", 1);
        tui_buf_emit(title, tl);
        tui_buf_emit(" ", 1);
        tui_buf_reset_attrs();
    }
    row++;
    for (int i = 0; i < TUI_PROGRESS_MAX_STEPS; i++) {
        tui_buf_set_colors(t->border_fg, t->border_bg);
        tui_buf_goto(row, win_x);
        tui_buf_emit("|", 1);
        tui_buf_set_colors(t->item_fg, t->item_bg);
        tui_buf_repeat(' ', win_w - 2);
        tui_buf_set_colors(t->border_fg, t->border_bg);
        tui_buf_goto(row, win_x + win_w - 1);
        tui_buf_emit("|", 1);
        row++;
    }
    tui_buf_draw_hline(row, win_x, win_w, t->border_fg, t->border_bg);
    tui_buf_flush();
}

void tui_progress_step(int step, int total, const char *description) {
    if (step < 1 || step > TUI_PROGRESS_MAX_STEPS || total < 1) return;
    if (description) {
        int len = tui_strlen(description);
        if (len >= TUI_PROGRESS_DESC_LEN) len = TUI_PROGRESS_DESC_LEN - 1;
        memcpy(tui_progress_desc[step - 1], description, len);
        tui_progress_desc[step - 1][len] = '\0';
    }
    tui_progress_total = total;

    tui_theme_t *t = &theme_cyan;
    int screen_w = (int)console_get_width();
    int screen_h = (int)console_get_height();
    int win_w = 54;
    if (win_w > screen_w - 4) win_w = screen_w - 4;
    int win_x = (screen_w - win_w) / 2 + 1;
    int win_y = (screen_h - 12) / 2 + 1;
    if (win_y < 2) win_y = 2;
    int inner_w = win_w - 2;

    int row = win_y + 1;
    for (int i = 0; i < total && i < TUI_PROGRESS_MAX_STEPS; i++) {
        int n = i + 1;
        bool is_current = (n == step);
        bool is_done = (n < step);

        tui_buf_set_colors(t->border_fg, t->border_bg);
        tui_buf_goto(row, win_x);
        tui_buf_emit("|", 1);

        if (is_current) {
            tui_buf_set_colors(t->sel_fg, t->sel_bg);
            tui_buf_set_bold();
        } else if (is_done) {
            tui_buf_set_colors(CLR_BRIGHT_GREEN, t->item_bg);
        } else {
            tui_buf_set_colors(t->disabled_fg, t->item_bg);
        }

        char line[80];
        int pos = 0;
        if (is_done)
            pos = snprintf(line, sizeof(line), "[*] ");
        else
            pos = snprintf(line, sizeof(line), "[%d] ", n);
        if (tui_progress_desc[i][0])
            snprintf(line + pos, sizeof(line) - pos, "%s", tui_progress_desc[i]);
        else
            snprintf(line + pos, sizeof(line) - pos, "Step %d", n);

        int line_len = tui_strlen(line);
        if (line_len > inner_w - 2) line_len = inner_w - 2;
        tui_buf_emit(" ", 1);
        tui_buf_emit(line, line_len);
        tui_buf_repeat(' ', inner_w - 1 - line_len);
        tui_buf_reset_attrs();

        tui_buf_set_colors(t->border_fg, t->border_bg);
        tui_buf_goto(row, win_x + win_w - 1);
        tui_buf_emit("|", 1);
        row++;
    }
    tui_buf_flush();
}

void tui_progress_finish(const char *message) {
    tui_theme_t *t = &theme_cyan;
    int screen_w = (int)console_get_width();
    int screen_h = (int)console_get_height();
    int win_w = 54;
    if (win_w > screen_w - 4) win_w = screen_w - 4;
    int win_x = (screen_w - win_w) / 2 + 1;
    int win_y = (screen_h - 12) / 2 + 1;
    if (win_y < 2) win_y = 2;
    int inner_w = win_w - 2;

    int row = win_y + 1 + (tui_progress_total < TUI_PROGRESS_MAX_STEPS ? tui_progress_total : TUI_PROGRESS_MAX_STEPS);

    tui_buf_set_colors(t->border_fg, t->border_bg);
    tui_buf_goto(row, win_x);
    tui_buf_emit("|", 1);
    tui_buf_set_colors(t->sel_fg, t->item_bg);
    if (message && message[0]) {
        int len = tui_strlen(message);
        if (len > inner_w - 2) len = inner_w - 2;
        tui_buf_emit(" ", 1);
        tui_buf_emit(message, len);
        tui_buf_repeat(' ', inner_w - 1 - len);
    } else {
        tui_buf_repeat(' ', inner_w);
    }
    tui_buf_set_colors(t->border_fg, t->border_bg);
    tui_buf_goto(row, win_x + win_w - 1);
    tui_buf_emit("|", 1);
    row++;

    tui_buf_set_colors(t->border_fg, t->border_bg);
    tui_buf_goto(row, win_x);
    tui_buf_emit("|", 1);
    tui_buf_set_colors(t->btn_fg, t->border_bg);
    tui_buf_emit("  Press any key to continue  ", 30);
    tui_buf_set_colors(t->border_fg, t->border_bg);
    tui_buf_repeat(' ', inner_w - 30);
    tui_buf_goto(row, win_x + win_w - 1);
    tui_buf_emit("|", 1);
    tui_buf_flush();

    tui_show_cursor();
    tui_read_key();
    tui_hide_cursor();
    tui_restore_screen();
    tui_show_cursor();
    keyboard_set_raw_mode(false);
    term_serial_suppress(false);
}
