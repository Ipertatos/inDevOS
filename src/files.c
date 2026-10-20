#include "files.h"
#include "render.h"
#include "vfs.h"
#include "string.h"
#include "serial.h"
#include "utils.h"
extern int snprintf(char *str, size_t size, const char *fmt, ...);
#include "../include/SDL.h"
#include "launch.h"
#include "mem.h"
#include "ktime.h"

#define FILES_W 560
#define FILES_H 420
#define FILES_ROW 22
#define FILES_TOP 36
#define FILES_MAX 1024          /* raw entries per folder (incl. . and ..) */

static desktop_window_t *fb_win;
static char fb_path[512];
static vfs_dirent_t *fb_ents;     /* kalloc(FILES_MAX) on first use */
static uint32_t fb_sig;           /* listing signature for auto-refresh */
static uint64_t fb_last_check;
static int fb_count;
static int fb_sel;
static int fb_scroll;
static char fb_status[80];
static bool fb_need;

/* Editable path bar */
#define FB_BAR_X 68
#define FB_TXT_X 74
static bool fb_edit;
static char fb_ebuf[512];
static int  fb_ecur, fb_eoff;
static bool fb_shift;

/* Drag-and-drop move between rows */
static int  fb_press_idx = -1;      /* armed by a left press on a row       */
static int  fb_press_x, fb_press_y;
static bool fb_dragging;
static int  fb_drop_idx = -1;       /* folder row under the pointer (target) */
static int  fb_ptr_x, fb_ptr_y;

static void fb_set_status(const char *s) {
    int i = 0;
    if (!s) s = "";
    while (s[i] && i < (int)sizeof(fb_status) - 1) {
        fb_status[i] = s[i];
        i++;
    }
    fb_status[i] = '\0';
}

static void fb_parent(void) {
    int n = 0;
    while (fb_path[n]) n++;
    if (n <= 1) {
        fb_path[0] = '/';
        fb_path[1] = '\0';
        return;
    }
    if (fb_path[n - 1] == '/') n--;
    while (n > 1 && fb_path[n - 1] != '/') n--;
    if (n <= 1) {
        fb_path[0] = '/';
        fb_path[1] = '\0';
    } else {
        fb_path[n - 1] = '\0';
    }
}

static void fb_join(const char *name) {
    char tmp[512];
    int i = 0, j = 0;
    if (fb_path[0] == '/' && fb_path[1] == '\0') {
        tmp[i++] = '/';
    } else {
        while (fb_path[j] && i < 500) tmp[i++] = fb_path[j++];
        if (i > 0 && tmp[i - 1] != '/' && i < 500) tmp[i++] = '/';
    }
    j = 0;
    while (name[j] && i < 500) tmp[i++] = name[j++];
    tmp[i] = '\0';
    i = 0;
    while (tmp[i] && i < (int)sizeof(fb_path) - 1) {
        fb_path[i] = tmp[i];
        i++;
    }
    fb_path[i] = '\0';
}

static uint32_t fb_signature(const vfs_dirent_t *e, int n) {
    uint32_t h = 2166136261u ^ (uint32_t)n;
    for (int i = 0; i < n; i++) {
        for (const char *c = e[i].name; *c; c++) h = (h ^ (uint8_t)*c) * 16777619u;
        h = (h ^ (uint32_t)e[i].type) * 16777619u;
    }
    return h;
}

static void fb_reload(void) {
    int n;
    fb_count = 0;
    fb_sel = 0;
    fb_scroll = 0;
    fb_press_idx = -1;
    fb_dragging = false;
    fb_drop_idx = -1;
    if (!fb_ents) fb_ents = (vfs_dirent_t *)kalloc(sizeof(vfs_dirent_t) * FILES_MAX);
    if (!fb_ents) { fb_set_status("Out of memory"); return; }
    n = vfs_readdir(fb_path, fb_ents, FILES_MAX);
    if (n >= 0) fb_sig = fb_signature(fb_ents, n);
    if (n < 0) {
        fb_set_status("Cannot list this folder");
        return;
    }
    for (int i = 0; i < n; i++) {
        if (fb_ents[i].name[0] == '.' && fb_ents[i].name[1] == '\0') continue;
        if (fb_ents[i].name[0] == '.' && fb_ents[i].name[1] == '.' &&
            fb_ents[i].name[2] == '\0')
            continue;
        if (fb_count != i)
            fb_ents[fb_count] = fb_ents[i];
        fb_count++;
    }
    for (int i = 0; i < fb_count; i++) {
        for (int j = i + 1; j < fb_count; j++) {
            int id = (fb_ents[i].type == VFS_DIRECTORY);
            int jd = (fb_ents[j].type == VFS_DIRECTORY);
            int swap = 0;
            if (jd && !id) swap = 1;
            else if (id == jd && strcmp(fb_ents[j].name, fb_ents[i].name) < 0)
                swap = 1;
            if (swap) {
                vfs_dirent_t t = fb_ents[i];
                fb_ents[i] = fb_ents[j];
                fb_ents[j] = t;
            }
        }
    }
    if (n >= FILES_MAX) {
        char m[80];
        snprintf(m, sizeof(m), "Showing the first %d entries", fb_count);
        fb_set_status(m);
    } else {
        fb_set_status(fb_count ? "Click a folder to open it" : "Empty folder");
    }
}

static int fb_rows_visible(void) {
    if (!fb_win) return 1;
    int h = fb_win->content_h - FILES_TOP - 28;
    if (h < FILES_ROW) return 1;
    return h / FILES_ROW;
}

static void fb_reveal_sel(void) {
    int vis = fb_rows_visible();
    if (fb_sel < fb_scroll) fb_scroll = fb_sel;
    if (fb_sel >= fb_scroll + vis) fb_scroll = fb_sel - vis + 1;
    if (fb_scroll < 0) fb_scroll = 0;
}

static void fb_enter_sel(void) {
    if (fb_sel < 0 || fb_sel >= fb_count) return;
    if (fb_ents[fb_sel].type != VFS_DIRECTORY) {
        char full[600];
        snprintf(full, sizeof(full), "%s%s%s", fb_path,
                 (fb_path[0] && fb_path[strlen(fb_path) - 1] == '/') ? "" : "/",
                 fb_ents[fb_sel].name);
        fb_set_status("Opening...");
        launch_open_path(full, false);
        return;
    }
    fb_join(fb_ents[fb_sel].name);
    fb_reload();
}

static void fb_redraw(void) {
    if (!fb_win || !fb_win->pixels) return;
    int w = fb_win->content_w;
    int h = fb_win->content_h;
    r_target_t t;
    r_target_init(&t, fb_win->pixels, w, h, w);
    r_fill(&t, 0, 0, w, h, 0xFF12161E);
    r_fill(&t, 0, 0, w, 32, 0xFF1A2035);
    r_rrect(&t, 8, 6, 52, 20, 4, 0xFF3DDC97);
    r_text(&t, 22, 8, "Up", 0xFF0B1020);
    {
        int bw = w - FB_BAR_X - 8;
        int maxch = (bw - 12) / 8;
        if (maxch < 4) maxch = 4;
        r_rrect(&t, FB_BAR_X, 5, bw, 22, 4, fb_edit ? 0xFF0B1020 : 0xFF141A2C);
        if (fb_edit) {
            r_rrect_outline(&t, FB_BAR_X, 5, bw, 22, 4, 0xFF3DDC97);
            if (fb_ecur < fb_eoff) fb_eoff = fb_ecur;
            if (fb_ecur > fb_eoff + maxch - 1) fb_eoff = fb_ecur - maxch + 1;
            if (fb_eoff < 0) fb_eoff = 0;
            char vis[160];
            int n = 0;
            for (int i = fb_eoff; fb_ebuf[i] && n < maxch && n < (int)sizeof(vis) - 1; i++)
                vis[n++] = fb_ebuf[i];
            vis[n] = '\0';
            r_text(&t, FB_TXT_X, 8, vis, 0xFFF4F4F5);
            int cx = FB_TXT_X + (fb_ecur - fb_eoff) * 8;
            r_fill(&t, cx, 8, 2, 16, 0xFF3DDC97);
        } else {
            char vis[160];
            int l = (int)strlen(fb_path), st = 0, n = 0;
            if (l > maxch) st = l - maxch;
            for (int i = st; fb_path[i] && n < (int)sizeof(vis) - 1; i++) vis[n++] = fb_path[i];
            vis[n] = '\0';
            r_text(&t, FB_TXT_X, 8, vis, 0xFFF4F4F5);
        }
    }

    int vis = fb_rows_visible();
    for (int row = 0; row < vis; row++) {
        int idx = fb_scroll + row;
        int y = FILES_TOP + row * FILES_ROW;
        if (idx >= fb_count) break;
        if (fb_dragging && idx == fb_drop_idx) {
            r_fill(&t, 8, y, w - 16, FILES_ROW - 2, 0xFF1F4E5F);
            r_rrect_outline(&t, 8, y, w - 16, FILES_ROW - 2, 3, 0xFF7DD3FC);
        } else if (idx == fb_sel)
            r_fill(&t, 8, y, w - 16, FILES_ROW - 2, 0xFF1E2740);
        const char *mark = (fb_ents[idx].type == VFS_DIRECTORY) ? "[dir]  " : "       ";
        char line[80];
        int n = 0;
        const char *s = mark;
        while (*s && n < 70) line[n++] = *s++;
        s = fb_ents[idx].name;
        while (*s && n < 70) line[n++] = *s++;
        line[n] = '\0';
        uint32_t col = (fb_ents[idx].type == VFS_DIRECTORY) ? 0xFF7DD3FC : 0xFFE5E9F0;
        r_text(&t, 16, y + 3, line, col);
    }
    r_fill(&t, 0, h - 24, w, 24, 0xFF0E121A);
    r_text(&t, 12, h - 18, fb_status, 0xFF94A3B8);
    if (fb_dragging && fb_press_idx >= 0 && fb_press_idx < fb_count) {
        char g[80];
        snprintf(g, sizeof(g), "%s %s", fb_drop_idx >= 0 ? "Move" : "  ", fb_ents[fb_press_idx].name);
        int gw = (int)strlen(g) * 8 + 12;
        int gx = fb_ptr_x + 14, gy = fb_ptr_y + 6;
        if (gx + gw > w) gx = w - gw;
        if (gy + 20 > h) gy = h - 20;
        r_rrect(&t, gx, gy, gw, 20, 4, fb_drop_idx >= 0 ? 0xFF1F4E5F : 0xFF2A3045);
        r_text(&t, gx + 6, gy + 2, g, 0xFFF4F4F5);
    }
    fb_win->dirty = true;
    fb_need = false;
}

static void fb_request(void) { fb_need = true; }

void files_open(void) {
    if (fb_win && fb_win->id) {
        desktop_focus_window(fb_win);
        fb_request();
        return;
    }
    int sw = (int)fb_get_width();
    int sh = (int)fb_get_height();
    int x = (sw - FILES_W) / 2;
    int y = (sh - FILES_H) / 3;
    if (x < 16) x = 16;
    if (y < 40) y = 40;
    if (fb_path[0] == '\0') {
        fb_path[0] = '/';
        fb_path[1] = '\0';
    }
    fb_win = desktop_create_window(0, x, y, FILES_W, FILES_H, "Files");
    if (!fb_win) return;
    fb_reload();
    spinlock_lock(&fb_win->content_lock);
    fb_redraw();
    spinlock_unlock(&fb_win->content_lock);
    serial_puts("[files] opened\n");
}

/* Open (or retarget) the Files window at folder @path. */
void files_open_path(const char *path) {
    if (path && path[0]) {
        kstrncpy(fb_path, path, sizeof(fb_path));
        int l = (int)strlen(fb_path);
        while (l > 1 && fb_path[l - 1] == '/') fb_path[--l] = '\0';
    }
    serial_printf("[files] open %s\n", fb_path);
    if (fb_win && fb_win->id) {
        fb_reload();
        fb_request();
        desktop_focus_window(fb_win);
        return;
    }
    files_open();
}

bool files_is_open(void) { return fb_win && fb_win->id != 0; }
bool files_is_window(const desktop_window_t *win) {
    return win && fb_win && win == fb_win && fb_win->id != 0;
}
void files_on_window_destroyed(desktop_window_t *win) {
    if (win == fb_win) fb_win = NULL;
}

static char fb_shifted(char c) {
    if (c >= 'a' && c <= 'z') return (char)(c - 32);
    switch (c) {
    case '1': return '!'; case '2': return '@'; case '3': return '#'; case '4': return '$';
    case '5': return '%'; case '6': return '^'; case '7': return '&'; case '8': return '*';
    case '9': return '('; case '0': return ')'; case '-': return '_'; case '=': return '+';
    case '[': return '{'; case ']': return '}'; case '\\': return '|'; case ';': return ':';
    case '\'': return '"'; case ',': return '<'; case '.': return '>'; case '/': return '?';
    case '`': return '~';
    default: return c;
    }
}

static void fb_edit_begin(int rx) {
    fb_edit = true;
    kstrncpy(fb_ebuf, fb_path, sizeof(fb_ebuf));
    int l = (int)strlen(fb_ebuf);
    int maxch = ((fb_win ? fb_win->content_w : FILES_W) - FB_BAR_X - 8 - 12) / 8;
    fb_eoff = l > maxch - 1 ? l - maxch + 1 : 0;
    int c = fb_eoff + (rx - FB_TXT_X + 4) / 8;
    if (c < 0) c = 0;
    if (c > l) c = l;
    fb_ecur = c;
    fb_set_status("Edit the path: Enter opens, Esc cancels");
}

static void fb_edit_end(void) {
    fb_edit = false;
    fb_set_status(fb_count ? "Click a folder to open it" : "Empty folder");
}

/* Normalize @in (absolute, or relative to the current folder) into @out:
 * collapses //, ., .. and strips the trailing slash. */
static void fb_normalize(const char *in, char *out, int cap) {
    char tmp[1024];
    int n = 0;
    if (in[0] != '/') {
        for (int i = 0; fb_path[i] && n < 1000; i++) tmp[n++] = fb_path[i];
        if (n < 1000) tmp[n++] = '/';
    }
    for (int i = 0; in[i] && n < 1020; i++) tmp[n++] = in[i];
    tmp[n] = '\0';
    int o = 0;
    out[o++] = '/';
    int i = 0;
    while (tmp[i]) {
        while (tmp[i] == '/') i++;
        if (!tmp[i]) break;
        int s = i;
        while (tmp[i] && tmp[i] != '/') i++;
        int len = i - s;
        if (len == 1 && tmp[s] == '.') continue;
        if (len == 2 && tmp[s] == '.' && tmp[s + 1] == '.') {
            if (o > 1) { o--; while (o > 1 && out[o - 1] != '/') o--; if (o > 1) o--; }
            if (o < 1) o = 1;
            continue;
        }
        if (o > 1 && o < cap - 1) out[o++] = '/';
        for (int k = 0; k < len && o < cap - 1; k++) out[o++] = tmp[s + k];
    }
    out[o] = '\0';
}

static void fb_edit_commit(void) {
    char p[512];
    char m[160];
    while (fb_ebuf[0] == ' ') memmove(fb_ebuf, fb_ebuf + 1, strlen(fb_ebuf));
    int l = (int)strlen(fb_ebuf);
    while (l > 0 && fb_ebuf[l - 1] == ' ') fb_ebuf[--l] = '\0';
    if (!fb_ebuf[0]) { fb_edit_end(); return; }
    fb_normalize(fb_ebuf, p, sizeof(p));
    static vfs_dirent_t probe[4];
    int n = vfs_readdir(p, probe, 4);
    if (n < 0) {
        snprintf(m, sizeof(m), vfs_exists(p) ? "Not a folder: %s" : "No such folder: %s", p);
        desktop_toast(m);
        serial_printf("[files] path bar: bad path '%s' (%s)\n", fb_ebuf, p);
        fb_set_status("Bad path - fix it or press Esc");
        return;                         /* stay in edit mode */
    }
    serial_printf("[files] path bar: go %s\n", p);
    kstrncpy(fb_path, p, sizeof(fb_path));
    fb_edit = false;
    fb_reload();
}

static void fb_edit_key(int32_t k) {
    int l = (int)strlen(fb_ebuf);
    if (k == SDLK_RETURN || k == '\n') { fb_edit_commit(); return; }
    if (k == SDLK_ESCAPE) { fb_edit_end(); return; }
    if (k == SDLK_LEFT)  { if (fb_ecur > 0) fb_ecur--; return; }
    if (k == SDLK_RIGHT) { if (fb_ecur < l) fb_ecur++; return; }
    if (k == SDLK_HOME)  { fb_ecur = 0; return; }
    if (k == SDLK_END)   { fb_ecur = l; return; }
    if (k == SDLK_BACKSPACE) {
        if (fb_ecur > 0) { memmove(fb_ebuf + fb_ecur - 1, fb_ebuf + fb_ecur, (size_t)(l - fb_ecur + 1)); fb_ecur--; }
        return;
    }
    if (k == 127) {                     /* Delete */
        if (fb_ecur < l) memmove(fb_ebuf + fb_ecur, fb_ebuf + fb_ecur + 1, (size_t)(l - fb_ecur));
        return;
    }
    if (k == '\t') return;
    if (k >= 32 && k < 127 && l < (int)sizeof(fb_ebuf) - 1) {
        char c = (char)k;
        if (fb_shift) c = fb_shifted(c);
        memmove(fb_ebuf + fb_ecur + 1, fb_ebuf + fb_ecur, (size_t)(l - fb_ecur + 1));
        fb_ebuf[fb_ecur++] = c;
    }
}

void files_on_key(int32_t keycode, bool pressed) {
    if (keycode == SDLK_LSHIFT || keycode == SDLK_RSHIFT) { fb_shift = pressed; return; }
    if (!files_is_open() || !pressed) return;
    if (fb_edit) { fb_edit_key(keycode); fb_request(); return; }
    if (keycode == SDLK_UP) {
        if (fb_sel > 0) fb_sel--;
        fb_reveal_sel();
        fb_request();
        return;
    }
    if (keycode == SDLK_DOWN) {
        if (fb_sel + 1 < fb_count) fb_sel++;
        fb_reveal_sel();
        fb_request();
        return;
    }
    if (keycode == SDLK_RETURN || keycode == '\n') {
        fb_enter_sel();
        fb_request();
        return;
    }
    if (keycode == SDLK_BACKSPACE || keycode == SDLK_ESCAPE) {
        fb_parent();
        fb_reload();
        fb_request();
        return;
    }
}

/* ---- move (drag-and-drop) ------------------------------------------------ */

static void fb_path_join(char *out, int cap, const char *dir, const char *name) {
    int l = (int)strlen(dir);
    snprintf(out, (size_t)cap, "%s%s%s", dir, (l > 0 && dir[l - 1] == '/') ? "" : "/", name);
}

static int fb_ci_eq_n(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return 0;
        if (!x) return 1;
    }
    return 1;
}

/* Move @src (file or folder) into folder @dstdir.  Refuses moving a folder
 * into itself or its own subtree, and never overwrites an existing entry.
 * On return @msg holds a user-facing result.  0 on success. */
int files_move_into(const char *src, const char *dstdir, char *msg, int cap) {
    const char *base = src;
    for (const char *q = src; *q; q++) if (*q == '/' && q[1]) base = q + 1;
    char name[VFS_MAX_NAME];
    int bl = 0;
    while (base[bl] && base[bl] != '/' && bl < VFS_MAX_NAME - 1) { name[bl] = base[bl]; bl++; }
    name[bl] = '\0';
    int sl = (int)strlen(src);
    while (sl > 1 && src[sl - 1] == '/') sl--;
    int dl = (int)strlen(dstdir);
    if (dl >= sl && fb_ci_eq_n(dstdir, src, sl) && (dstdir[sl] == '\0' || dstdir[sl] == '/')) {
        snprintf(msg, (size_t)cap, "Can't move \"%s\" into itself", name);
        return -1;
    }
    /* already directly in dstdir? */
    int pl = (int)(base - src);
    while (pl > 1 && src[pl - 1] == '/') pl--;
    if (pl == dl && fb_ci_eq_n(src, dstdir, pl)) {
        snprintf(msg, (size_t)cap, "\"%s\" is already there", name);
        return -1;
    }
    char dst[600];
    fb_path_join(dst, sizeof(dst), dstdir, name);
    if (vfs_exists(dst)) {
        snprintf(msg, (size_t)cap, "Move failed: \"%s\" already exists there", name);
        return -1;
    }
    int rc = vfs_rename(src, dst);
    serial_printf("[files] move %s -> %s rc=%d\n", src, dst, rc);
    if (rc != 0) {
        snprintf(msg, (size_t)cap, "Move failed (%d): %s", rc, name);
        return -1;
    }
    extern int fat32_sync(void);
    fat32_sync();
    const char *dn = dstdir;
    for (const char *q = dstdir; *q; q++) if (*q == '/' && q[1]) dn = q + 1;
    snprintf(msg, (size_t)cap, "Moved \"%s\" to %s", name, dn);
    return 0;
}

static void fb_do_drop(int from, int to) {
    if (from < 0 || from >= fb_count || to < 0 || to >= fb_count || from == to) return;
    if (fb_ents[to].type != VFS_DIRECTORY) return;
    char src[600], dst[600], m[200];
    fb_path_join(src, sizeof(src), fb_path, fb_ents[from].name);
    fb_path_join(dst, sizeof(dst), fb_path, fb_ents[to].name);
    int rc = files_move_into(src, dst, m, sizeof(m));
    desktop_toast(m);
    fb_set_status(m);
    if (rc == 0) {
        char keep[VFS_MAX_NAME];
        kstrncpy(keep, fb_ents[to].name, sizeof(keep));
        int scroll = fb_scroll;
        char st[80];
        kstrncpy(st, fb_status, sizeof(st));
        fb_reload();
        for (int i = 0; i < fb_count; i++)
            if (strcmp(fb_ents[i].name, keep) == 0) { fb_sel = i; break; }
        fb_scroll = scroll;
        fb_reveal_sel();
        fb_set_status(st);
        desktop_icons_changed();
    }
}

static int fb_row_at(int ry) {
    if (!fb_win || ry < FILES_TOP || ry >= fb_win->content_h - 24) return -1;
    int row = (ry - FILES_TOP) / FILES_ROW;
    if (row >= fb_rows_visible()) return -1;
    int idx = fb_scroll + row;
    return (idx >= 0 && idx < fb_count) ? idx : -1;
}

void files_on_pointer(const desktop_ptr_event_t *ev) {
    if (!files_is_open() || !fb_win || !ev) return;
    int rx = ev->x, ry = ev->y;
    if (ev->type == DESKTOP_PTR_WHEEL) { files_on_wheel(ev->wheel); return; }
    if (ev->type == DESKTOP_PTR_MOVE) {
        fb_ptr_x = rx; fb_ptr_y = ry;
        if (fb_press_idx < 0) return;
        int dx = rx - fb_press_x, dy = ry - fb_press_y;
        if (!fb_dragging && (dx > 4 || dx < -4 || dy > 4 || dy < -4)) {
            fb_dragging = true;
            serial_printf("[files] drag start %s\n", fb_ents[fb_press_idx].name);
        }
        if (fb_dragging) {
            int t = fb_row_at(ry);
            if (t == fb_press_idx || (t >= 0 && fb_ents[t].type != VFS_DIRECTORY)) t = -1;
            fb_drop_idx = t;
            fb_request();
        }
        return;
    }
    if (ev->button != 1) return;
    if (ev->type == DESKTOP_PTR_DOWN) {
        fb_press_idx = -1; fb_dragging = false; fb_drop_idx = -1;
        fb_ptr_x = rx; fb_ptr_y = ry;
        if (ry >= 4 && ry < 30 && rx >= 8 && rx < 64) {
            if (fb_edit) fb_edit_end();
            fb_parent();
            fb_reload();
            fb_request();
            return;
        }
        if (ry >= 4 && ry < 30 && rx >= FB_BAR_X) {
            if (!fb_edit) fb_edit_begin(rx);
            else {
                int l = (int)strlen(fb_ebuf);
                int c = fb_eoff + (rx - FB_TXT_X + 4) / 8;
                fb_ecur = c < 0 ? 0 : (c > l ? l : c);
            }
            fb_request();
            return;
        }
        if (fb_edit) { fb_edit_end(); fb_request(); }   /* click elsewhere = revert */
        int idx = fb_row_at(ry);
        if (idx < 0) return;
        fb_sel = idx;
        fb_press_idx = idx;
        fb_press_x = rx; fb_press_y = ry;
        fb_request();
        return;
    }
    if (ev->type == DESKTOP_PTR_UP) {
        int from = fb_press_idx;
        bool drag = fb_dragging;
        int to = fb_drop_idx;
        fb_press_idx = -1; fb_dragging = false; fb_drop_idx = -1;
        if (from < 0) return;
        if (drag) {
            if (to >= 0) fb_do_drop(from, to);
            else serial_printf("[files] drag cancelled\n");
        } else if (fb_row_at(ry) == from) {
            fb_sel = from;
            fb_enter_sel();             /* single click opens, as before */
        }
        fb_request();
    }
}

/* Legacy entry point: left press only. */
void files_on_mouse(int rx, int ry, bool down) {
    desktop_ptr_event_t ev = { down ? DESKTOP_PTR_DOWN : DESKTOP_PTR_UP, rx, ry, 1, 1, 0, 0 };
    files_on_pointer(&ev);
}

/* Wheel: positive = up; 3 rows per step. */
void files_on_wheel(int steps) {
    if (!files_is_open() || !steps) return;
    int vis = fb_rows_visible();
    int max = fb_count - vis;
    if (max < 0) max = 0;
    fb_scroll -= steps * 3;
    if (fb_scroll < 0) fb_scroll = 0;
    if (fb_scroll > max) fb_scroll = max;
    fb_request();
}

/* Re-read the folder once a second; reload (keeping the selected name) if
 * something created/renamed/deleted entries behind our back. */
static void fb_autorefresh(void) {
    uint64_t now = ktime_now_ms();
    if (now - fb_last_check < 1000 || fb_press_idx >= 0) return;
    fb_last_check = now;
    static vfs_dirent_t *tmp;
    if (!tmp) tmp = (vfs_dirent_t *)kalloc(sizeof(vfs_dirent_t) * FILES_MAX);
    if (!tmp || !fb_ents) return;
    int n = vfs_readdir(fb_path, tmp, FILES_MAX);
    if (n < 0 || fb_signature(tmp, n) == fb_sig) return;
    char keep[VFS_MAX_NAME] = "";
    if (fb_sel >= 0 && fb_sel < fb_count) kstrncpy(keep, fb_ents[fb_sel].name, sizeof(keep));
    int scroll = fb_scroll;
    fb_reload();
    for (int i = 0; i < fb_count && keep[0]; i++)
        if (strcmp(fb_ents[i].name, keep) == 0) { fb_sel = i; break; }
    fb_scroll = scroll;
    fb_reveal_sel();
    serial_printf("[files] %s changed, reloaded (%d entries)\n", fb_path, fb_count);
    fb_need = true;
}

bool files_tick(void) {
    if (files_is_open()) fb_autorefresh();
    if (!files_is_open() || !fb_need) return false;
    spinlock_lock(&fb_win->content_lock);
    fb_redraw();
    spinlock_unlock(&fb_win->content_lock);
    return true;
}
