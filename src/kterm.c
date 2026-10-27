#include "kterm.h"
#include "pipe.h"
#include "pty.h"
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
#define KT_GR 80      /* max grid rows */
#define KT_GC 160     /* max grid cols */

/* ---- Per-window instance state ----------------------------------------
 * Every terminal window has its own shell, pipes, pty, VT grid, scrollback
 * and history. They live in a fixed pool (not kmalloc'd): the shell's pipes
 * keep a pointer to the instance's pty, and children can hold those pipe
 * ends after the window is gone. K is the instance the code below works on;
 * every public entry point selects it while holding kt_busy. */
#define KT_MAX 6
#define KT_ADEF 0x88   /* fg 8 / bg 8 = default colours */
struct kt_inst {
    bool     used;
    volatile bool closing;           /* window destroyed; pump ends the shell */
    char     txq[1024];              /* typed input not yet in the shell pipe */
    int      txlen;
    pipe_t  *sh_in;                  /* we write: shell stdin  */
    pipe_t  *sh_out;                 /* we read:  shell stdout+stderr */
    int      sh_pid;
    bool     sh_dead;                /* shell exited; empty Enter restarts */
    int      esc;                    /* VT parser state (kt_vt_feed) */
    kpty_t   pty;                    /* terminal state of the shell pipes */
    char     env_lines[16], env_cols[16];
    int      ws_last_rows, ws_last_cols;
    desktop_window_t *win;
    char     lines[KT_ROWS][KT_COLS];
    int      nlines, scroll;
    char     in[160];
    int      inlen;
    bool     need;
    char     hist[KT_HIST][160];
    int      nhist, hpos;
    char     g[KT_GR][KT_GC];
    uint16_t ga[KT_GR][KT_GC];
    int      gr, gc;
    int      cy, cx;
    bool     wrapnext;
    uint16_t attr;
    int      top, bot;
    int      sy, sx;
    uint16_t sattr;
    bool     curvis, appcur;
    int      cs[2];                  /* G0/G1: 0 ASCII, 1 DEC line drawing */
    int      gl;                     /* 0 = G0 active, 1 = G1 (SO) */
    int      view;                   /* scrollback lines shown (0 = live) */
    int      par[8], npar;
    char     priv;
    bool     vt_inited;
};
static struct kt_inst  kt_pool[KT_MAX];
static struct kt_inst *K = &kt_pool[0];
static struct kt_inst *kt_last;      /* most recently opened */
static volatile int    kt_busy;      /* one CPU in kterm code at a time */
#define kt_txq (K->txq)
#define kt_txlen (K->txlen)
#define kt_sh_in (K->sh_in)
#define kt_sh_out (K->sh_out)
#define kt_sh_pid (K->sh_pid)
#define kt_sh_dead (K->sh_dead)
#define kt_esc (K->esc)
#define kt_pty (K->pty)
#define kt_env_lines (K->env_lines)
#define kt_env_cols (K->env_cols)
#define kt_ws_last_rows (K->ws_last_rows)
#define kt_ws_last_cols (K->ws_last_cols)
#define kt_win (K->win)
#define kt_lines (K->lines)
#define kt_nlines (K->nlines)
#define kt_scroll (K->scroll)
#define kt_in (K->in)
#define kt_inlen (K->inlen)
#define kt_need (K->need)
#define kt_hist (K->hist)
#define kt_nhist (K->nhist)
#define kt_hpos (K->hpos)
#define kt_g (K->g)
#define kt_ga (K->ga)
#define kt_gr (K->gr)
#define kt_gc (K->gc)
#define kt_cy (K->cy)
#define kt_cx (K->cx)
#define kt_wrapnext (K->wrapnext)
#define kt_attr (K->attr)
#define kt_top (K->top)
#define kt_bot (K->bot)
#define kt_sy (K->sy)
#define kt_sx (K->sx)
#define kt_sattr (K->sattr)
#define kt_curvis (K->curvis)
#define kt_appcur (K->appcur)
#define kt_cs (K->cs)
#define kt_gl (K->gl)
#define kt_view (K->view)
#define kt_par (K->par)
#define kt_npar (K->npar)
#define kt_priv (K->priv)
#define kt_vt_inited (K->vt_inited)

/* Shell mode: the window runs BusyBox ash with stdin/stdout/stderr on two
 * kernel pipes (pty-like, no line discipline). The window does the line
 * editing and local echo, then writes whole lines to the shell. When no
 * shell can be started (disk not mounted yet), it falls back to running
 * Zeus built-ins in-kernel as before. */
extern int  process_signal_tree(uint32_t root_pid, int sig, bool include_root);
extern bool process_pid_alive(uint32_t pid);
extern int  pipe_try_read(pipe_t *pipe, void *buf, size_t count);
extern int  pipe_try_write(pipe_t *pipe, const void *buf, size_t count);
static void    kt_tx_flush(void);
static bool    kt_ctrl;
static char    kt_start_dir[256];  /* cwd for the next shell (kterm_open_in) */


static bool kt_shift;

static void kt_request(void) { kt_need = true; }
static void kt_vt_puts(const char *s);
static void kt_vt_feed(const char *buf, int n);
static void kt_vt_resize(int rows, int cols);
static void kt_vt_to_sb(void);
static void kt_add_line(const char *s);

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

static void kt_sb_add(const char *s) {
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

/* Shell mode text goes onto the VT grid; Zeus mode onto the scrollback. */
static void kt_add_line(const char *s) {
    if (kt_shell_mode()) { kt_vt_puts(s); kt_vt_puts("\r\n"); }
    else kt_sb_add(s);
}

static int kt_grows(void) {
    if (!kt_win) return 24;
    int r = (kt_win->content_h - 8) / KT_LINE;
    if (r > KT_GR) r = KT_GR;
    return r < 2 ? 2 : r;
}

static int kt_ws_cols(void) {
    if (!kt_win) return 80;
    int c = (kt_win->content_w - 16) / 8;
    if (c > KT_GC) c = KT_GC;
    return c < 10 ? 10 : c;
}

static void kt_utoa_env(char *dst, const char *pre, int v) {
    int n = 0;
    while (*pre) dst[n++] = *pre++;
    char tmp[8]; int k = 0;
    if (v <= 0) tmp[k++] = '0';
    while (v > 0 && k < 7) { tmp[k++] = (char)('0' + v % 10); v /= 10; }
    while (k) dst[n++] = tmp[--k];
    dst[n] = '\0';
}

/* Track the window size; SIGWINCH the shell's tree when it changes. */
static void kt_ws_update(void) {
    if (!kt_win) return;
    int rows = kt_grows(), cols = kt_ws_cols();
    if (rows == kt_ws_last_rows && cols == kt_ws_last_cols) return;
    kt_ws_last_rows = rows;
    kt_ws_last_cols = cols;
    kt_pty.rows = (uint16_t)rows;
    kt_pty.cols = (uint16_t)cols;
    kt_vt_resize(rows, cols);
    kt_request();
    if (kt_sh_pid > 0 && kt_shell_mode()) {
        int nsig = process_signal_tree((uint32_t)kt_sh_pid, 28 /*SIGWINCH*/, true);
        serial_printf("[kterm] winsize %dx%d, SIGWINCH -> %d\n", cols, rows, nsig);
    }
}

static bool kt_raw(void) {
    return kt_sh_in && !(kt_pty.lflag & KPTY_ICANON);
}

static void kt_shell_close(void) {
    if (kt_sh_pid > 0)
        process_signal_tree((uint32_t)kt_sh_pid, 1 /*SIGHUP*/, true);
    if (kt_sh_in)  pipe_close_write(kt_sh_in);
    if (kt_sh_out) pipe_close_read(kt_sh_out);
    kt_sh_in = kt_sh_out = NULL;
    kt_sh_pid = 0;
    kt_txlen = 0;
    if (kt_vt_inited) kt_vt_to_sb();
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
        /* pty-like: both pipes share one terminal state (termios+winsize) */
        kpty_default(&kt_pty);
        kt_ws_last_rows = kt_grows();
        kt_ws_last_cols = kt_ws_cols();
        kt_pty.rows = (uint16_t)kt_ws_last_rows;
        kt_pty.cols = (uint16_t)kt_ws_last_cols;
        kt_vt_resize(kt_ws_last_rows, kt_ws_last_cols);
        in->pty = &kt_pty;
        out->pty = &kt_pty;
        kt_utoa_env(kt_env_lines, "LINES=", kt_ws_last_rows);
        kt_utoa_env(kt_env_cols, "COLUMNS=", kt_ws_last_cols);
        const char *envp[] = { "TERM=vt100", kt_env_lines, kt_env_cols, NULL };
        exec_spawn_set_stdio_pipes(in, out);
        int pid = exec_spawn_ex(paths[i], argv,
                                kt_start_dir[0] ? kt_start_dir : NULL, envp);
        if (pid > 0) {
            kt_sh_in = in;
            kt_sh_out = out;
            kt_sh_pid = pid;
            kt_esc = 0;
            serial_printf("[kterm] shell %s pid %d on pty %dx%d\n", paths[i], pid,
                          kt_ws_last_cols, kt_ws_last_rows);
            return true;
        }
        /* Spawn failed: nobody took the child ends. */
        pipe_close_read(in);  pipe_close_write(in);
        pipe_close_read(out); pipe_close_write(out);
    }
    return false;
}

/* Drain the current instance's shell output. */
static bool kt_pump_one(void) {
    kt_tx_flush();
    if (!kt_sh_out) return false;
    bool got = false;
    char buf[512];
    for (int round = 0; round < 8; round++) {
        int n = pipe_try_read(kt_sh_out, buf, sizeof(buf));
        if (n > 0) { kt_vt_feed(buf, n); got = true; continue; }
        if (n == 0) {                       /* EOF: shell and children gone */
            kt_vt_to_sb();
            kt_sb_add("[shell exited - press Enter to start a new one]");
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

/* ---- VT100 character grid (shell mode) --------------------------------
 * The shell and its programs draw on a fixed rows x cols grid with a
 * cursor; rows scrolled off the top of the full-screen region go to the
 * kt_lines scrollback (Shift+PgUp/PgDn, or PgUp/PgDn in cooked mode). */


#define KT_A_BOLD 0x100
#define KT_A_REV  0x200

static void kt_sb_add(const char *s);

static void kt_vt_clear(int r, int c0, int c1) {
    if (r < 0 || r >= KT_GR) return;
    uint16_t a = (uint16_t)((kt_attr & 0xF0) | 0x08);
    if (c0 < 0) c0 = 0;
    if (c1 > KT_GC) c1 = KT_GC;
    for (int c = c0; c < c1; c++) { kt_g[r][c] = ' '; kt_ga[r][c] = a; }
}

static void kt_vt_rowtext(int r, char *out, int max) {
    int n = kt_gc < max - 1 ? kt_gc : max - 1;
    for (int c = 0; c < n; c++) out[c] = kt_g[r][c] ? kt_g[r][c] : ' ';
    while (n > 0 && out[n - 1] == ' ') n--;
    out[n] = '\0';
}

static void kt_vt_row_to_sb(int r) {
    char line[KT_COLS];
    kt_vt_rowtext(r, line, KT_COLS);
    kt_sb_add(line);
}

static void kt_vt_scroll_up(int top, int bot, int n, bool to_sb) {
    if (top < 0 || bot >= kt_gr || top > bot) return;
    if (n > bot - top + 1) n = bot - top + 1;
    for (int k = 0; k < n; k++) {
        if (to_sb && top == 0) kt_vt_row_to_sb(0);
        for (int r = top; r < bot; r++) {
            memcpy(kt_g[r], kt_g[r + 1], KT_GC);
            memcpy(kt_ga[r], kt_ga[r + 1], sizeof(kt_ga[r]));
        }
        kt_vt_clear(bot, 0, KT_GC);
    }
}

static void kt_vt_scroll_down(int top, int bot, int n) {
    if (top < 0 || bot >= kt_gr || top > bot) return;
    if (n > bot - top + 1) n = bot - top + 1;
    for (int k = 0; k < n; k++) {
        for (int r = bot; r > top; r--) {
            memcpy(kt_g[r], kt_g[r - 1], KT_GC);
            memcpy(kt_ga[r], kt_ga[r - 1], sizeof(kt_ga[r]));
        }
        kt_vt_clear(top, 0, KT_GC);
    }
}

static void kt_vt_reset(void) {
    kt_attr = KT_ADEF;
    for (int r = 0; r < KT_GR; r++) kt_vt_clear(r, 0, KT_GC);
    kt_cy = kt_cx = 0;
    kt_wrapnext = false;
    kt_top = 0; kt_bot = kt_gr - 1;
    kt_sy = kt_sx = 0; kt_sattr = KT_ADEF;
    kt_curvis = true; kt_appcur = false;
    kt_cs[0] = kt_cs[1] = 0; kt_gl = 0;
    kt_vt_inited = true;
}

static void kt_vt_clamp(void) {
    if (kt_cy < 0) kt_cy = 0;
    if (kt_cy >= kt_gr) kt_cy = kt_gr - 1;
    if (kt_cx < 0) kt_cx = 0;
    if (kt_cx >= kt_gc) kt_cx = kt_gc - 1;
}

static void kt_vt_resize(int rows, int cols) {
    if (rows < 2) rows = 2;
    if (rows > KT_GR) rows = KT_GR;
    if (cols < 10) cols = 10;
    if (cols > KT_GC) cols = KT_GC;
    if (!kt_vt_inited) { kt_gr = rows; kt_gc = cols; kt_vt_reset(); return; }
    if (kt_cy >= rows) {                       /* keep the cursor row visible */
        int sh = kt_cy - (rows - 1);
        int oldbot = kt_gr - 1;
        kt_vt_scroll_up(0, oldbot, sh, true);
        kt_cy -= sh;
    }
    for (int r = 0; r < KT_GR; r++) {
        if (r >= kt_gr) kt_vt_clear(r, 0, KT_GC);
        else kt_vt_clear(r, kt_gc, KT_GC);
    }
    kt_gr = rows; kt_gc = cols;
    kt_top = 0; kt_bot = rows - 1;
    kt_wrapnext = false;
    kt_vt_clamp();
}

static void kt_vt_mirror_row(int r) {
    char line[KT_COLS];
    kt_vt_rowtext(r, line, KT_COLS);
    serial_printf("[kterm] | %s\n", line);    /* mirror for logs */
}

static void kt_vt_lf(void) {
    kt_vt_mirror_row(kt_cy);
    kt_wrapnext = false;
    if (kt_cy == kt_bot) kt_vt_scroll_up(kt_top, kt_bot, 1, true);
    else if (kt_cy < kt_gr - 1) kt_cy++;
}

static char kt_vt_acs(char c) {
    switch (c) {
    case 'j': case 'k': case 'l': case 'm': case 'n':
    case 't': case 'u': case 'v': case 'w': case '`': return '+';
    case 'q': case 'o': case 'p': case 'r': case 's': return '-';
    case 'x': return '|';
    case 'a': case '0': case 'h': return '#';
    case 'f': return '\'';
    case 'g': return '#';
    case '~': return 'o';
    case 'y': return '<';
    case 'z': return '>';
    case '{': return '*';
    case '|': return '!';
    case '}': return 'f';
    default: return c;
    }
}

static void kt_vt_put(char c) {
    if (kt_wrapnext) { kt_cx = 0; kt_vt_lf(); }
    if (kt_cs[kt_gl]) c = kt_vt_acs(c);
    kt_g[kt_cy][kt_cx] = c;
    kt_ga[kt_cy][kt_cx] = kt_attr;
    if (kt_cx >= kt_gc - 1) kt_wrapnext = true;
    else kt_cx++;
}

static void kt_vt_reply(const char *s) {
    int n = 0;
    while (s[n]) n++;
    kt_shell_send(s, n);
}

static void kt_vt_sgr(void) {
    if (kt_npar == 0) { kt_attr = KT_ADEF; return; }
    for (int i = 0; i < kt_npar; i++) {
        int p = kt_par[i];
        if (p == 0) kt_attr = KT_ADEF;
        else if (p == 1) kt_attr |= KT_A_BOLD;
        else if (p == 22) kt_attr &= (uint16_t)~KT_A_BOLD;
        else if (p == 7) kt_attr |= KT_A_REV;
        else if (p == 27) kt_attr &= (uint16_t)~KT_A_REV;
        else if (p >= 30 && p <= 37) kt_attr = (uint16_t)((kt_attr & ~0x0F) | (p - 30));
        else if (p == 39) kt_attr = (uint16_t)((kt_attr & ~0x0F) | 8);
        else if (p >= 40 && p <= 47) kt_attr = (uint16_t)((kt_attr & ~0xF0) | ((p - 40) << 4));
        else if (p == 49) kt_attr = (uint16_t)((kt_attr & ~0xF0) | 0x80);
        else if (p >= 90 && p <= 97) kt_attr = (uint16_t)((kt_attr & ~0x0F) | (p - 90) | KT_A_BOLD);
        else if (p >= 100 && p <= 107) kt_attr = (uint16_t)((kt_attr & ~0xF0) | ((p - 100) << 4));
        /* 4 underline, 5 blink, 38/48 extended: ignored */
        else if (p == 38 || p == 48) break;
    }
}

static void kt_vt_csi(char fin) {
    int p0 = kt_npar > 0 ? kt_par[0] : 0;
    int p1 = kt_npar > 1 ? kt_par[1] : 0;
    int n1 = p0 > 0 ? p0 : 1;
    if (kt_priv == '?') {
        if (fin == 'h' || fin == 'l') {
            bool on = fin == 'h';
            for (int i = 0; i < kt_npar; i++) {
                if (kt_par[i] == 25) kt_curvis = on;
                else if (kt_par[i] == 1) kt_appcur = on;
                else if (kt_par[i] == 1049 || kt_par[i] == 47 || kt_par[i] == 1047) {
                    /* no alternate screen: start/leave with a clean grid */
                    for (int r = 0; r < kt_gr; r++) kt_vt_clear(r, 0, KT_GC);
                }
            }
        }
        return;
    }
    if (kt_priv) {                               /* '>' / '=' queries */
        return;
    }
    switch (fin) {
    case 'A': kt_cy -= n1; if (kt_cy < kt_top && kt_cy + n1 >= kt_top) kt_cy = kt_top; break;
    case 'B': kt_cy += n1; if (kt_cy > kt_bot && kt_cy - n1 <= kt_bot) kt_cy = kt_bot; break;
    case 'C': kt_cx += n1; break;
    case 'D': if (kt_cx >= kt_gc) kt_cx = kt_gc - 1; kt_cx -= n1; break;
    case 'E': kt_cy += n1; kt_cx = 0; break;
    case 'F': kt_cy -= n1; kt_cx = 0; break;
    case 'G': case '`': kt_cx = n1 - 1; break;
    case 'd': kt_cy = n1 - 1; break;
    case 'H': case 'f':
        kt_cy = (p0 > 0 ? p0 : 1) - 1;
        kt_cx = (p1 > 0 ? p1 : 1) - 1;
        break;
    case 'J':
        if (p0 == 0) {
            kt_vt_clear(kt_cy, kt_cx, KT_GC);
            for (int r = kt_cy + 1; r < kt_gr; r++) kt_vt_clear(r, 0, KT_GC);
        } else if (p0 == 1) {
            for (int r = 0; r < kt_cy; r++) kt_vt_clear(r, 0, KT_GC);
            kt_vt_clear(kt_cy, 0, kt_cx + 1);
        } else {
            for (int r = 0; r < kt_gr; r++) kt_vt_clear(r, 0, KT_GC);
        }
        break;
    case 'K':
        if (p0 == 0) kt_vt_clear(kt_cy, kt_cx, KT_GC);
        else if (p0 == 1) kt_vt_clear(kt_cy, 0, kt_cx + 1);
        else kt_vt_clear(kt_cy, 0, KT_GC);
        break;
    case 'L': if (kt_cy >= kt_top && kt_cy <= kt_bot) kt_vt_scroll_down(kt_cy, kt_bot, n1); break;
    case 'M': if (kt_cy >= kt_top && kt_cy <= kt_bot) kt_vt_scroll_up(kt_cy, kt_bot, n1, false); break;
    case '@': {
        for (int c = kt_gc - 1; c >= kt_cx + n1; c--) { kt_g[kt_cy][c] = kt_g[kt_cy][c - n1]; kt_ga[kt_cy][c] = kt_ga[kt_cy][c - n1]; }
        kt_vt_clear(kt_cy, kt_cx, kt_cx + n1 < kt_gc ? kt_cx + n1 : kt_gc);
        break;
    }
    case 'P': {
        for (int c = kt_cx; c < kt_gc; c++) {
            if (c + n1 < kt_gc) { kt_g[kt_cy][c] = kt_g[kt_cy][c + n1]; kt_ga[kt_cy][c] = kt_ga[kt_cy][c + n1]; }
            else kt_vt_clear(kt_cy, c, c + 1);
        }
        break;
    }
    case 'X': kt_vt_clear(kt_cy, kt_cx, kt_cx + n1 < kt_gc ? kt_cx + n1 : kt_gc); break;
    case 'S': kt_vt_scroll_up(kt_top, kt_bot, n1, false); break;
    case 'T': kt_vt_scroll_down(kt_top, kt_bot, n1); break;
    case 'r': {
        int t = (p0 > 0 ? p0 : 1) - 1, b = (p1 > 0 ? p1 : kt_gr) - 1;
        if (b >= kt_gr) b = kt_gr - 1;
        if (t < b) { kt_top = t; kt_bot = b; } else { kt_top = 0; kt_bot = kt_gr - 1; }
        kt_cy = 0; kt_cx = 0;
        break;
    }
    case 's': kt_sy = kt_cy; kt_sx = kt_cx; kt_sattr = kt_attr; break;
    case 'u': kt_cy = kt_sy; kt_cx = kt_sx; kt_attr = kt_sattr; break;
    case 'm': kt_vt_sgr(); return;
    case 'n':
        if (p0 == 6) {
            char rb[24]; char *q = rb; int v;
            *q++ = 27; *q++ = '[';
            v = kt_cy + 1; if (v >= 10) *q++ = (char)('0' + v / 10); *q++ = (char)('0' + v % 10);
            *q++ = ';';
            v = kt_cx + 1; if (v >= 100) *q++ = (char)('0' + v / 100); if (v >= 10) *q++ = (char)('0' + (v / 10) % 10); *q++ = (char)('0' + v % 10);
            *q++ = 'R'; *q = '\0';
            kt_vt_reply(rb);
        } else if (p0 == 5) kt_vt_reply("\033[0n");
        return;
    case 'c': kt_vt_reply("\033[?1;2c"); return;
    default: return;                            /* unknown: ignored */
    }
    kt_wrapnext = false;
    kt_vt_clamp();
}

/* esc states: 0 text, 1 ESC, 2 CSI, 3 ESC ( , 4 ESC ) , 5 OSC, 6 OSC+ESC, 7 skip one */
static void kt_vt_feed(const char *buf, int n) {
    if (!kt_vt_inited) kt_vt_reset();
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (kt_esc == 2) {
            if (c >= '0' && c <= '9') {
                if (kt_npar == 0) { kt_npar = 1; kt_par[0] = 0; }
                if (kt_par[kt_npar - 1] < 10000) kt_par[kt_npar - 1] = kt_par[kt_npar - 1] * 10 + (c - '0');
            } else if (c == ';') {
                if (kt_npar == 0) { kt_npar = 1; kt_par[0] = 0; }
                if (kt_npar < 8) kt_par[kt_npar++] = 0;
            } else if (c == '?' || c == '>' || c == '=' || c == '<') {
                kt_priv = (char)c;
            } else if (c >= 0x40 && c <= 0x7E) {
                kt_esc = 0;
                kt_vt_csi((char)c);
            } else if (c == 27) {
                kt_esc = 1;
            } else if (c < 32) {
                /* control inside CSI: execute (rare) */
                if (c == '\r') { kt_cx = 0; kt_wrapnext = false; }
                else if (c == '\b' && kt_cx > 0) kt_cx--;
            }
            continue;
        }
        if (kt_esc == 1) {
            kt_esc = 0;
            switch (c) {
            case '[': kt_esc = 2; kt_npar = 0; kt_priv = 0; kt_par[0] = 0; break;
            case '7': kt_sy = kt_cy; kt_sx = kt_cx; kt_sattr = kt_attr; break;
            case '8': kt_cy = kt_sy; kt_cx = kt_sx; kt_attr = kt_sattr; kt_wrapnext = false; kt_vt_clamp(); break;
            case 'M':
                kt_wrapnext = false;
                if (kt_cy == kt_top) kt_vt_scroll_down(kt_top, kt_bot, 1);
                else if (kt_cy > 0) kt_cy--;
                break;
            case 'D': kt_vt_lf(); break;
            case 'E': kt_cx = 0; kt_vt_lf(); break;
            case 'c': kt_vt_reset(); break;
            case '(': kt_esc = 3; break;
            case ')': kt_esc = 4; break;
            case ']': case 'P': case '_': case '^': kt_esc = 5; break;
            case '#': case '%': kt_esc = 7; break;
            default: break;                     /* '=', '>', unknown: ignored */
            }
            continue;
        }
        if (kt_esc == 3 || kt_esc == 4) {
            kt_cs[kt_esc - 3] = (c == '0') ? 1 : 0;
            kt_esc = 0;
            continue;
        }
        if (kt_esc == 5) {                      /* OSC/DCS: until BEL or ST */
            if (c == 7) kt_esc = 0;
            else if (c == 27) kt_esc = 6;
            continue;
        }
        if (kt_esc == 6) { kt_esc = (c == '\\') ? 0 : 5; continue; }
        if (kt_esc == 7) { kt_esc = 0; continue; }
        switch (c) {
        case 27: kt_esc = 1; break;
        case '\n': case 11: case 12:
            kt_vt_lf();
            if ((kt_pty.oflag & 0x1) && (kt_pty.oflag & 0x4)) kt_cx = 0;   /* OPOST|ONLCR */
            break;
        case '\r': kt_cx = 0; kt_wrapnext = false; break;
        case '\b': if (kt_wrapnext) kt_wrapnext = false; else if (kt_cx > 0) kt_cx--; break;
        case '\t': {
            int nx = (kt_cx / 8 + 1) * 8;
            if (nx > kt_gc - 1) nx = kt_gc - 1;
            kt_cx = nx;
            break;
        }
        case 14: kt_gl = 1; break;              /* SO */
        case 15: kt_gl = 0; break;              /* SI */
        default:
            if (c >= 32 && c < 127) kt_vt_put((char)c);
            break;                              /* BEL, NUL, 8-bit: ignored */
        }
    }
}

static void kt_vt_puts(const char *s) {
    int n = 0;
    while (s[n]) n++;
    kt_vt_feed(s, n);
}

/* Move whatever the grid shows into scrollback (shell gone -> Zeus view). */
static void kt_vt_to_sb(void) {
    int last = kt_cy;
    for (int r = kt_gr - 1; r > last; r--) {
        char line[KT_COLS];
        kt_vt_rowtext(r, line, KT_COLS);
        if (line[0]) { last = r; break; }
    }
    for (int r = 0; r <= last && r < kt_gr; r++) kt_vt_row_to_sb(r);
    for (int r = 0; r < KT_GR; r++) kt_vt_clear(r, 0, KT_GC);
    kt_cy = kt_cx = 0;
}

static const uint32_t kt_pal[16] = {
    0xFF1A1E26, 0xFFE05561, 0xFF3DDC97, 0xFFE5C07B, 0xFF61AFEF, 0xFFC678DD, 0xFF56B6C2, 0xFFD1D5DB,
    0xFF5C6370, 0xFFFF7A85, 0xFF7CFFB2, 0xFFFFE08A, 0xFF8CC8FF, 0xFFE6A8FF, 0xFF8BEAF5, 0xFFFFFFFF };
#define KT_FG_DEF 0xFFD1FAE5
#define KT_BG_DEF 0xFF0B1020

static void kt_cell_colors(uint16_t a, uint32_t *fg, uint32_t *bg) {
    int f = a & 0x0F, b = (a >> 4) & 0x0F;
    uint32_t F = (f >= 8) ? ((a & KT_A_BOLD) ? 0xFFFFFFFF : KT_FG_DEF)
                          : kt_pal[f + ((a & KT_A_BOLD) ? 8 : 0)];
    uint32_t B = (b >= 8) ? KT_BG_DEF : kt_pal[b];
    if (a & KT_A_REV) { uint32_t t = F; F = (b >= 8) ? KT_BG_DEF : B; B = (f >= 8) ? KT_FG_DEF : t; }
    *fg = F; *bg = B;
}

static void kt_vt_draw(r_target_t *t, int w, int h) {
    (void)w; (void)h;
    int x0 = 8;
    int vo = kt_view;
    if (vo > kt_nlines) vo = kt_nlines;
    for (int i = 0; i < kt_gr; i++) {
        int y = 4 + i * KT_LINE;
        int idx = kt_nlines - vo + i;             /* combined sb+grid index */
        if (idx < kt_nlines) {
            if (idx >= 0) r_text(t, x0, y + 2, kt_lines[idx], 0xFF9CA3AF);
            continue;
        }
        int r = idx - kt_nlines;
        if (r < 0 || r >= kt_gr) continue;
        for (int c = 0; c < kt_gc; c++) {
            uint32_t fg, bg;
            kt_cell_colors(kt_ga[r][c], &fg, &bg);
            if (bg != KT_BG_DEF) r_fill(t, x0 + c * 8, y, 8, KT_LINE, bg);
            char ch = kt_g[r][c];
            if (ch > 32 && ch < 127) {
                char s[2] = { ch, 0 };
                r_text(t, x0 + c * 8, y + 2, s, fg);
            }
        }
    }
    if (vo > 0) {
        r_text(t, w - 8 * 16, 4, "[scrollback]", 0xFFE5C07B);
        return;
    }
    int cx = kt_cx < kt_gc ? kt_cx : kt_gc - 1;
    int cy = kt_cy;
    if (!kt_raw() && kt_inlen > 0) {            /* cooked: local line edit */
        for (int i = 0; i < kt_inlen; i++) {
            char s[2] = { kt_in[i], 0 };
            r_text(t, x0 + cx * 8, 4 + cy * KT_LINE + 2, s, 0xFFF4F4F5);
            if (cx < kt_gc - 1) cx++;
        }
    }
    if (kt_curvis)
        r_fill(t, x0 + cx * 8, 4 + cy * KT_LINE + 14, 8, 2, 0xFF3DDC97);
}

static void kt_redraw(void) {
    if (!kt_win || !kt_win->pixels) return;
    int w = kt_win->content_w;
    int h = kt_win->content_h;
    r_target_t t;
    r_target_init(&t, kt_win->pixels, w, h, w);
    r_fill(&t, 0, 0, w, h, 0xFF0B1020);
    if (kt_shell_mode()) {
        kt_vt_draw(&t, w, h);
        kt_win->dirty = true;
        kt_need = false;
        return;
    }
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
    if (kt_shell_mode()) {
        /* Echo the line after the shell's prompt, then hand it over. */
        if (kt_pty.lflag & KPTY_ECHO) {       /* cooked echo onto the grid */
            kt_in[kt_inlen] = '\0';
            kt_vt_puts(kt_in);
            kt_vt_puts("\r\n");
        }
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

/* ---- Instance pool / locking ----------------------------------------- */
static bool kt_try_enter(void) {
    return __atomic_exchange_n(&kt_busy, 1, __ATOMIC_ACQUIRE) == 0;
}
/* Bounded spin: the holder may be a preempted syscall-context compositor
 * pass on this CPU. Gives up (caller drops the event) rather than hang. */
static bool kt_enter(void) {
    for (uint32_t i = 0; i < 4000000u; i++) {
        if (kt_try_enter()) return true;
        __asm__ volatile("pause");
    }
    serial_puts("[kterm] busy: event dropped\n");
    return false;
}
static void kt_leave(void) { __atomic_store_n(&kt_busy, 0, __ATOMIC_RELEASE); }

static struct kt_inst *kt_find(const desktop_window_t *win) {
    if (!win) return NULL;
    for (int i = 0; i < KT_MAX; i++) {
        struct kt_inst *t = &kt_pool[i];
        if (t->used && !t->closing && t->win == win && win->id != 0)
            return t;
    }
    return NULL;
}

static int kt_count_open(void) {
    int n = 0;
    for (int i = 0; i < KT_MAX; i++)
        if (kt_pool[i].used && !kt_pool[i].closing && kt_pool[i].win) n++;
    return n;
}

/* A free slot, reset to power-on defaults; NULL when all are in use. */
static struct kt_inst *kt_alloc(void) {
    static int next;
    for (int k = 0; k < KT_MAX; k++) {
        int i = (next + k) % KT_MAX;
        struct kt_inst *t = &kt_pool[i];
        if (t->used) continue;
        memset(t, 0, sizeof(*t));
        t->used = true;
        t->gr = 24; t->gc = 80; t->bot = 23;
        t->attr = t->sattr = KT_ADEF;
        t->curvis = true;
        next = (i + 1) % KT_MAX;
        return t;
    }
    return NULL;
}

/* Window destroyed earlier (kterm_on_window_destroyed): end its shell and
 * free the slot. Caller holds kt_busy. */
static void kt_reap_closing(void) {
    for (int i = 0; i < KT_MAX; i++) {
        struct kt_inst *t = &kt_pool[i];
        if (!t->used || !t->closing) continue;
        K = t;
        kt_win = NULL;
        kt_shell_close();
        serial_printf("[kterm] instance %d closed\n", i);
        if (kt_last == t) kt_last = NULL;
        t->closing = false;
        t->used = false;
    }
}

/* Open a new terminal window with its own shell (in kt_start_dir when set).
 * Caller holds kt_busy. Returns false when the pool is full (the newest
 * terminal is focused instead) or the window could not be created. */
static bool kt_open_new(void) {
    kt_reap_closing();
    struct kt_inst *t = kt_alloc();
    if (!t) {
        serial_printf("[kterm] %d terminals open, focusing the newest\n", KT_MAX);
        if (kt_last && kt_last->win && kt_last->win->id)
            desktop_focus_window(kt_last->win);
        return false;
    }
    int nopen = kt_count_open();
    K = t;
    int sw = (int)fb_get_width();
    int sh = (int)fb_get_height();
    int x = (sw - KT_W) / 2 + nopen * 28;    /* cascade */
    int y = (sh - KT_H) / 4 + nopen * 28;
    if (x + KT_W > sw) x = sw - KT_W;
    if (y + KT_H > sh) y = sh - KT_H;
    if (x < 12) x = 12;
    if (y < 40) y = 40;
    kt_win = desktop_create_window(0, x, y, KT_W, KT_H, "Terminal");
    if (!kt_win) { t->used = false; return false; }
    kt_last = t;
    if (!kt_shell_mode()) {
        bool first = kt_nlines == 0 && !kt_vt_inited;
        if (kt_shell_start()) {
            if (first) kt_add_line("inDevOS terminal");
            kt_add_line("BusyBox ash in this window. Ctrl+C interrupts, Ctrl+D ends it.");
        } else {
            if (first) kt_add_line("inDevOS terminal");
            kt_add_line("No disk shell yet (mount 0 on a TTY, then type sh here).");
            kt_add_line("Zeus built-ins meanwhile: help, ls, pwd, cd, cat, echo, date.");
        }
    }
    klock(&kt_win->content_lock);
    kt_redraw();
    kunlock(&kt_win->content_lock);
    serial_printf("[kterm] opened (instance %d, window %u, %d open)\n",
                  (int)(t - kt_pool), kt_win->id, nopen + 1);
    return true;
}

/* Every call opens another terminal window (up to KT_MAX). */
void kterm_open(void) {
    if (!kt_enter()) return;
    kt_open_new();
    kt_leave();
}

bool kterm_is_open(void) { return kt_count_open() > 0; }

/* Also selects the instance for an immediately following on_key/on_mouse. */
bool kterm_is_window(const desktop_window_t *win) {
    return kt_find(win) != NULL;
}

/* Called from desktop_destroy_window, possibly with desktop_lock held:
 * only mark the slot; kterm_pump (under kt_busy) ends the shell. */
void kterm_on_window_destroyed(desktop_window_t *win) {
    if (!win) return;
    for (int i = 0; i < KT_MAX; i++) {
        struct kt_inst *t = &kt_pool[i];
        if (t->used && !t->closing && t->win == win) {
            t->win = NULL;
            t->closing = true;
        }
    }
}

static void kt_on_key(int32_t keycode, bool pressed);

/* Keys go to the focused terminal window. */
void kterm_on_key(int32_t keycode, bool pressed) {
    extern desktop_window_t *desktop_get_focused(void);
    if (!kt_enter()) return;
    struct kt_inst *t = kt_find(desktop_get_focused());
    if (t) {
        K = t;
        kt_on_key(keycode, pressed);
    }
    kt_leave();
}

static void kt_on_key(int32_t keycode, bool pressed) {
    if (keycode == SDLK_LSHIFT || keycode == SDLK_RSHIFT) {
        kt_shift = pressed;
        return;
    }
    if (keycode == SDLK_LCTRL || keycode == SDLK_RCTRL) {
        kt_ctrl = pressed;
        return;
    }
    if (!pressed) return;
    if (kt_shell_mode() && (keycode == SDLK_PAGEUP || keycode == SDLK_PAGEDOWN) &&
        (kt_shift || !kt_raw())) {               /* local scrollback view */
        int step = kt_gr > 2 ? kt_gr - 1 : 1;
        kt_view += keycode == SDLK_PAGEUP ? step : -step;
        if (kt_view > kt_nlines) kt_view = kt_nlines;
        if (kt_view < 0) kt_view = 0;
        kt_request();
        return;
    }
    if (kt_shell_mode()) kt_view = 0;
    if (kt_raw()) {
        /* non-canonical: send keys, the app echoes (PgUp/PgDn stay local
         * scrollback until the VT100 renderer job) */
        char kb[8];
        int kn = 0;
        if (kt_ctrl) {
            if (keycode == 'c' && (kt_pty.lflag & KPTY_ISIG) && kt_pty.cc[KPTY_VINTR] == 3) {
                int n = process_signal_tree((uint32_t)kt_sh_pid, 2, false);
                serial_printf("[kterm] Ctrl+C (raw) -> %d job process(es)\n", n);
                if (n == 0) kb[kn++] = 3;
            } else if (keycode >= 'a' && keycode <= 'z') {
                kb[kn++] = (char)(keycode & 0x1f);
            }
        } else if (keycode == SDLK_RETURN || keycode == '\n') {
            kb[kn++] = (kt_pty.iflag & KPTY_ICRNL) ? '\n' : '\r';
        } else if (keycode == SDLK_BACKSPACE) {
            kb[kn++] = kt_pty.cc[KPTY_VERASE] ? (char)kt_pty.cc[KPTY_VERASE] : 127;
        } else if (keycode == SDLK_TAB) {
            kb[kn++] = '\t';
        } else if (keycode == SDLK_ESCAPE) {
            kb[kn++] = 27;
        } else if (keycode == SDLK_UP || keycode == SDLK_DOWN ||
                   keycode == SDLK_RIGHT || keycode == SDLK_LEFT) {
            kb[kn++] = 27; kb[kn++] = kt_appcur ? 'O' : '[';
            kb[kn++] = keycode == SDLK_UP ? 'A' : keycode == SDLK_DOWN ? 'B' :
                       keycode == SDLK_RIGHT ? 'C' : 'D';
        } else if (keycode == SDLK_HOME || keycode == SDLK_END ||
                   keycode == SDLK_PAGEUP || keycode == SDLK_PAGEDOWN) {
            kb[kn++] = 27; kb[kn++] = '[';                 /* VT220 keys */
            kb[kn++] = keycode == SDLK_HOME ? '1' : keycode == SDLK_END ? '4' :
                       keycode == SDLK_PAGEUP ? '5' : '6';
            kb[kn++] = '~';
        } else if (keycode == SDLK_F4) {
            kb[kn++] = 27; kb[kn++] = 'O'; kb[kn++] = 'S';
        } else if (keycode >= 32 && keycode < 127) {
            char c = (char)keycode;
            if (kt_shift) c = kt_shifted(c);
            kb[kn++] = c;
        }
        if (kn) kt_shell_send(kb, kn);
        kt_follow();
        kt_request();
        return;
    }
    if (kt_ctrl && kt_shell_mode()) {
        if (keycode == 'c') {                 /* SIGINT to the shell's jobs */
            int n = process_signal_tree((uint32_t)kt_sh_pid, 2, false);
            kt_in[kt_inlen] = '\0';
            kt_vt_puts(kt_in);
            kt_vt_puts("^C\r\n");
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
        if (keycode == 'l') { kt_nlines = 0; kt_scroll = 0; kt_view = 0; kt_vt_puts("\033[H\033[2J"); kt_request(); return; }
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

/* Drain every terminal's shell output. Safe to call every main-loop pass;
 * skips the pass if another CPU is in kterm code. */
bool kterm_pump(void) {
    if (!kt_try_enter()) return false;
    kt_reap_closing();
    bool got = false;
    for (int i = 0; i < KT_MAX; i++) {
        if (!kt_pool[i].used || kt_pool[i].closing) continue;
        K = &kt_pool[i];
        if (kt_pump_one()) got = true;
    }
    kt_leave();
    return got;
}

bool kterm_tick(void) {
    if (!kt_try_enter()) return false;
    kt_reap_closing();
    bool drew = false;
    for (int i = 0; i < KT_MAX; i++) {
        if (!kt_pool[i].used || kt_pool[i].closing) continue;
        K = &kt_pool[i];
        kt_pump_one();
        kt_ws_update();
        if (!kt_win || kt_win->id == 0 || !kt_need) continue;
        klock(&kt_win->content_lock);
        kt_redraw();
        kunlock(&kt_win->content_lock);
        drew = true;
    }
    kt_leave();
    return drew;
}

/* Open a new terminal window whose shell starts in @dir. */
void kterm_open_in(const char *dir) {
    if (!kt_enter()) return;
    kt_start_dir[0] = '\0';
    if (dir && dir[0]) {
        kstrncpy(kt_start_dir, dir, sizeof(kt_start_dir));
        serial_printf("[kterm] open in %s\n", dir);
    }
    kt_open_new();
    kt_start_dir[0] = '\0';
    kt_leave();
}

/* Open a new terminal window in @dir and type @cmdline into its shell
 * (desktop shortcuts with Exec=terminal-run ...). Each shortcut gets its own
 * window, so a busy terminal (Doom running in it) no longer swallows it.
 * The shell is BusyBox ash on pipes, so full-screen programs should use
 * non-interactive modes for now. */
void kterm_run_in(const char *dir, const char *cmdline) {
    if (!kt_enter()) return;
    kt_start_dir[0] = '\0';
    if (dir && dir[0])
        kstrncpy(kt_start_dir, dir, sizeof(kt_start_dir));
    bool opened = kt_open_new();
    kt_start_dir[0] = '\0';
    if (opened && cmdline && cmdline[0]) {
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
    kt_leave();
}
