#include "files.h"
#include "render.h"
#include "vfs.h"
#include "string.h"
#include "serial.h"
#include "utils.h"
extern int snprintf(char *str, size_t size, const char *fmt, ...);
#include "../include/SDL.h"

#define FILES_W 560
#define FILES_H 420
#define FILES_ROW 22
#define FILES_TOP 36
#define FILES_MAX 40

static desktop_window_t *fb_win;
static char fb_path[512];
static vfs_dirent_t fb_ents[FILES_MAX];
static int fb_count;
static int fb_sel;
static int fb_scroll;
static char fb_status[80];
static bool fb_need;

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

static void fb_reload(void) {
    int n;
    fb_count = 0;
    fb_sel = 0;
    fb_scroll = 0;
    n = vfs_readdir(fb_path, fb_ents, FILES_MAX);
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
    fb_set_status(fb_count ? "Click a folder to open it" : "Empty folder");
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
        fb_set_status("Not a folder");
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
    r_text(&t, 70, 8, fb_path, 0xFFF4F4F5);

    int vis = fb_rows_visible();
    for (int row = 0; row < vis; row++) {
        int idx = fb_scroll + row;
        int y = FILES_TOP + row * FILES_ROW;
        if (idx >= fb_count) break;
        if (idx == fb_sel)
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

bool files_is_open(void) { return fb_win && fb_win->id != 0; }
bool files_is_window(const desktop_window_t *win) {
    return win && fb_win && win == fb_win && fb_win->id != 0;
}
void files_on_window_destroyed(desktop_window_t *win) {
    if (win == fb_win) fb_win = NULL;
}

void files_on_key(int32_t keycode, bool pressed) {
    if (!files_is_open() || !pressed) return;
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

void files_on_mouse(int rx, int ry, bool down) {
    if (!files_is_open() || !down || !fb_win) return;
    if (ry >= 4 && ry < 30 && rx >= 8 && rx < 64) {
        fb_parent();
        fb_reload();
        fb_request();
        return;
    }
    if (ry < FILES_TOP || ry >= fb_win->content_h - 24) return;
    int row = (ry - FILES_TOP) / FILES_ROW;
    int idx = fb_scroll + row;
    if (idx < 0 || idx >= fb_count) return;
    fb_sel = idx;
    fb_enter_sel();
    fb_request();
}

bool files_tick(void) {
    if (!files_is_open() || !fb_need) return false;
    spinlock_lock(&fb_win->content_lock);
    fb_redraw();
    spinlock_unlock(&fb_win->content_lock);
    return true;
}
