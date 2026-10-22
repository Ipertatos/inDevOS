#include "kterm.h"
#include "pipe.h"
#include "../include/indevos_errno.h"
#include "exec.h"
#include "hpet.h"
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

/* Shell mode: the window runs BusyBox ash with stdin/stdout/stderr on two
 * kernel pipes (pty-like, no line discipline). The window does the line
 * editing and local echo, then writes whole lines to the shell. When no
 * shell can be started (disk not mounted yet), it falls back to running
 * Zeus built-ins in-kernel as before. */
extern int  process_signal_tree(uint32_t root_pid, int sig, bool include_root);
extern bool process_pid_alive(uint32_t pid);
extern int  pipe_try_read(pipe_t *pipe, void *buf, size_t count);
extern int  pipe_try_write(pipe_t *pipe, const void *buf, size_t count);
static char    kt_txq[1024];      /* typed input not yet in the shell pipe */
static int     kt_txlen;
static void    kt_tx_flush(void);
static pipe_t *kt_sh_in;    /* we write: shell stdin  */
static pipe_t *kt_sh_out;   /* we read:  shell stdout+stderr */
static int     kt_sh_pid;
static bool    kt_ctrl;
static bool    kt_sh_dead;   /* shell exited; empty Enter restarts */
static char    kt_part[KT_COLS];   /* current (unterminated) output line */
static int     kt_partlen;
static int     kt_esc;             /* 0 none, 1 got ESC, 2 in CSI */
static char    kt_start_dir[256];  /* cwd for the next shell (kterm_open_in) */
static char    kt_csi[16];
static int     kt_csilen;

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

static bool kt_shell_mode(void) { return kt_sh_in != NULL || kt_sh_out != NULL; }

static void kt_part_flush(void) {
    kt_part[kt_partlen] = '\0';
    if (kt_shell_mode())
        serial_printf("[kterm] | %s\n", kt_part);   /* mirror for logs */
    kt_add_line(kt_part);
    kt_partlen = 0;
    kt_part[0] = '\0';
}

static void kt_csi_done(char fin) {
    kt_csi[kt_csilen] = '\0';
    if (fin == 'J' && kt_csi[0] == '2') {        /* clear screen */
        kt_nlines = 0;
        kt_scroll = 0;
    } else if (fin == 'K') {                      /* erase line */
        kt_partlen = 0;
    }
    /* colours / cursor motion: ignored */
}

/* Feed shell output bytes: minimal VT100 (text, \n, \r, \b, \t, CSI). */
static void kt_feed(const char *buf, int n) {
    for (int i = 0; i < n; i++) {
        char c = buf[i];
        if (kt_esc == 1) {
            kt_esc = (c == '[') ? 2 : 0;
            kt_csilen = 0;
            continue;
        }
        if (kt_esc == 2) {
            if ((unsigned char)c >= 0x40 && (unsigned char)c <= 0x7E) {
                kt_esc = 0;
                kt_csi_done(c);
            } else if (kt_csilen < (int)sizeof(kt_csi) - 1) {
                kt_csi[kt_csilen++] = c;
            }
            continue;
        }
        if (c == 27) { kt_esc = 1; continue; }
        if (c == '\n') { kt_part_flush(); continue; }
        if (c == '\r') continue;
        if (c == '\b') { if (kt_partlen > 0) kt_partlen--; continue; }
        if (c == '\t') {
            do {
                if (kt_partlen >= KT_COLS - 1) kt_part_flush();
                kt_part[kt_partlen++] = ' ';
            } while (kt_partlen % 8);
            continue;
        }
        if ((unsigned char)c < 32 || (unsigned char)c > 126) continue;
        if (kt_partlen >= KT_COLS - 1) kt_part_flush();
        kt_part[kt_partlen++] = c;
    }
    kt_part[kt_partlen] = '\0';
    kt_follow();
}

static void kt_shell_close(void) {
    if (kt_sh_pid > 0)
        process_signal_tree((uint32_t)kt_sh_pid, 1 /*SIGHUP*/, true);
    if (kt_sh_in)  pipe_close_write(kt_sh_in);
    if (kt_sh_out) pipe_close_read(kt_sh_out);
    kt_sh_in = kt_sh_out = NULL;
    kt_sh_pid = 0;
    kt_txlen = 0;
}

/* Start BusyBox ash on a pipe pair. Returns true when the shell runs. */
static bool kt_shell_start(void) {
    static const char *const paths[] = {
        "/bin/busybox.exe", "/bin/busybox", "/busybox.exe", NULL };
    if (kt_shell_mode()) return true;
    for (int i = 0; paths[i]; i++) {
        pipe_t *in = pipe_create();
        pipe_t *out = pipe_create();
        if (!in || !out) {
            if (in)  { pipe_close_read(in);  pipe_close_write(in); }
            if (out) { pipe_close_read(out); pipe_close_write(out); }
            return false;
        }
        static char *argv[] = { "sh", "-i", NULL };
        exec_spawn_set_stdio_pipes(in, out);
        int pid = exec_spawn_ex(paths[i], argv,
                                kt_start_dir[0] ? kt_start_dir : NULL, NULL);
        if (pid > 0) {
            kt_sh_in = in;
            kt_sh_out = out;
            kt_sh_pid = pid;
            kt_partlen = 0;
            kt_esc = 0;
            serial_printf("[kterm] shell %s pid %d on pipes\n", paths[i], pid);
            return true;
        }
        /* Spawn failed: nobody took the child ends. */
        pipe_close_read(in);  pipe_close_write(in);
        pipe_close_read(out); pipe_close_write(out);
    }
    return false;
}

/* Drain shell output. Safe to call every main-loop pass. */
bool kterm_pump(void) {
    kt_tx_flush();
    if (!kt_sh_out) return false;
    bool got = false;
    char buf[512];
    for (int round = 0; round < 8; round++) {
        int n = pipe_try_read(kt_sh_out, buf, sizeof(buf));
        if (n > 0) { kt_feed(buf, n); got = true; continue; }
        if (n == 0) {                       /* EOF: shell and children gone */
            if (kt_partlen) kt_part_flush();
            kt_add_line("[shell exited - press Enter to start a new one]");
            serial_printf("[kterm] shell pid %d exited\n", kt_sh_pid);
            kt_sh_pid = 0;
            kt_sh_dead = true;
            if (kt_sh_in) pipe_close_write(kt_sh_in);
            pipe_close_read(kt_sh_out);
            kt_sh_in = kt_sh_out = NULL;
            got = true;
        }
        break;
    }
    if (got) kt_request();
    return got;
}

/* Push queued input into the shell's stdin without ever spinning on the
 * pipe lock (the main loop is not preemptible). */
static void kt_tx_flush(void) {
    if (!kt_sh_in || kt_txlen == 0) return;
    int w = pipe_try_write(kt_sh_in, kt_txq, (size_t)kt_txlen);
    if (w > 0) {
        memmove(kt_txq, kt_txq + w, (size_t)(kt_txlen - w));
        kt_txlen -= w;
    } else if (w == -EPIPE) {
        kt_txlen = 0;
    }
}

static void kt_shell_send(const char *s, int n) {
    if (!kt_sh_in) return;
    for (int i = 0; i < n && kt_txlen < (int)sizeof(kt_txq); i++)
        kt_txq[kt_txlen++] = s[i];
    kt_tx_flush();
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
    if (kt_shell_mode()) {
        int m = 0;
        for (int i = 0; i < kt_partlen && m < 150; i++) prompt[m++] = kt_part[i];
        for (int i = 0; i < kt_inlen && m < 196; i++) prompt[m++] = kt_in[i];
        if (m < 198) prompt[m++] = '_';
        prompt[m] = '\0';
        r_text(&t, 8, h - 20, prompt, 0xFFF4F4F5);
        kt_win->dirty = true;
        kt_need = false;
        return;
    }
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
    if (kt_shell_mode()) {
        /* Echo the line after the shell's prompt, then hand it over. */
        for (int i = 0; i < kt_inlen && kt_partlen < KT_COLS - 1; i++)
            kt_part[kt_partlen++] = kt_in[i];
        kt_part_flush();
        if (kt_inlen > 0 && kt_nhist < KT_HIST) {
            int i = 0;
            while (kt_in[i] && i < 159) { kt_hist[kt_nhist][i] = kt_in[i]; i++; }
            kt_hist[kt_nhist][i] = '\0';
            kt_nhist++;
        }
        kt_hpos = kt_nhist;
        kt_shell_send(kt_in, kt_inlen);
        kt_shell_send("\n", 1);
        return;
    }
    if (kt_first_word_is(line, "sh") || kt_first_word_is(line, "ash")) {
        if (kt_shell_start()) return;
        kt_add_line("No shell: /bin/busybox.exe not found (try 'mount 0' on a TTY).");
        return;
    }
    while (*line == ' ') line++;
    if (*line == '\0') {
        if (kt_sh_dead && kt_shell_start()) kt_sh_dead = false;
        return;
    }
    if (kt_nhist < KT_HIST) {
        int i = 0;
        while (line[i] && i < 159) { kt_hist[kt_nhist][i] = line[i]; i++; }
        kt_hist[kt_nhist][i] = '\0';
        kt_nhist++;
    }
    kt_hpos = kt_nhist;
    /* Build prompt+command line for history from kt_in. */
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
    if (kt_nlines == 0)
        kt_add_line("inDevOS terminal");
    if (!kt_shell_mode()) {
        if (kt_shell_start()) {
            kt_add_line("BusyBox ash in this window. Ctrl+C interrupts, Ctrl+D ends it.");
        } else {
            kt_add_line("No disk shell yet (mount 0 on a TTY, then type sh here).");
            kt_add_line("Zeus built-ins meanwhile: help, ls, pwd, cd, cat, echo, date.");
        }
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
    if (win == kt_win) {
        kt_win = NULL;
        kt_shell_close();
    }
}

void kterm_on_key(int32_t keycode, bool pressed) {
    if (!kterm_is_open()) return;
    if (keycode == SDLK_LSHIFT || keycode == SDLK_RSHIFT) {
        kt_shift = pressed;
        return;
    }
    if (keycode == SDLK_LCTRL || keycode == SDLK_RCTRL) {
        kt_ctrl = pressed;
        return;
    }
    if (!pressed) return;
    if (kt_ctrl && kt_shell_mode()) {
        if (keycode == 'c') {                 /* SIGINT to the shell's jobs */
            int n = process_signal_tree((uint32_t)kt_sh_pid, 2, false);
            for (int i = 0; i < kt_inlen && kt_partlen < KT_COLS - 3; i++)
                kt_part[kt_partlen++] = kt_in[i];
            kt_part[kt_partlen++] = '^';
            kt_part[kt_partlen++] = 'C';
            kt_part_flush();
            kt_inlen = 0;
            kt_in[0] = '\0';
            serial_printf("[kterm] Ctrl+C -> %d job process(es)\n", n);
            if (n == 0) kt_shell_send("\n", 1);   /* fresh prompt */
            kt_request();
            return;
        }
        if (keycode == 'd' && kt_inlen == 0) {  /* EOF on stdin */
            if (kt_sh_in) { pipe_close_write(kt_sh_in); kt_sh_in = NULL; }
            kt_request();
            return;
        }
        if (keycode == 'l') { kt_nlines = 0; kt_scroll = 0; kt_request(); return; }
        return;
    }
    if (keycode == SDLK_TAB && kt_shell_mode()) {
        if (kt_inlen < 158) { kt_in[kt_inlen++] = ' '; kt_in[kt_inlen] = '\0'; }
        kt_request();
        return;
    }
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
    kterm_pump();
    if (!kterm_is_open() || !kt_need) return false;
    spinlock_lock(&kt_win->content_lock);
    kt_redraw();
    spinlock_unlock(&kt_win->content_lock);
    return true;
}

/* Open the terminal with its shell in @dir: a new shell starts there; a
 * running one gets a cd. */
void kterm_open_in(const char *dir) {
    if (dir && dir[0]) {
        kstrncpy(kt_start_dir, dir, sizeof(kt_start_dir));
        if (kt_sh_in && kt_sh_pid > 0) {
            char cmd[300];
            int n = 0;
            const char *p = "cd '";
            while (*p) cmd[n++] = *p++;
            for (p = dir; *p && n < (int)sizeof(cmd) - 4; p++) cmd[n++] = *p;
            cmd[n++] = '\'';
            cmd[n++] = '\n';
            if (kt_txlen + n <= (int)sizeof(kt_txq)) {
                memcpy(kt_txq + kt_txlen, cmd, (size_t)n);
                kt_txlen += n;
            }
        }
        serial_printf("[kterm] open in %s\n", dir);
    }
    kterm_open();
}

/* Open the terminal in @dir and type @cmdline into its shell (desktop
 * shortcuts with Exec=terminal-run ...).  The shell is BusyBox ash on pipes,
 * so full-screen programs should use non-interactive modes for now. */
void kterm_run_in(const char *dir, const char *cmdline) {
    kterm_open_in(dir);
    if (!cmdline || !cmdline[0]) return;
    int n = 0;
    while (cmdline[n]) n++;
    if (kt_sh_in) {
        kt_shell_send(cmdline, n);
        kt_shell_send("\n", 1);
        serial_printf("[kterm] run: %s\n", cmdline);
    } else {
        kt_add_line("No disk shell: cannot run the shortcut command.");
    }
}
