#include "settings_app.h"
#include "settings.h"
#include "render.h"
#include "string.h"
#include "serial.h"
#include "utils.h"
extern int snprintf(char *str, size_t size, const char *fmt, ...);
#include "keyboard.h"
#include "../include/SDL.h"

#define SET_W 520
#define SET_H 430
#define SET_ROW 36
#define SET_TOP 44

static desktop_window_t *set_win;
static bool set_need;
static char set_status[80];

static const uint32_t set_accents[] = {
    0x3DDC97, 0x60A5FA, 0xFBBF24, 0xF87171, 0xA78BFA, 0xFB7185
};
#define SET_NACCENT 6

static void set_status_msg(const char *s) {
    int i = 0;
    while (s && s[i] && i < (int)sizeof(set_status) - 1) {
        set_status[i] = s[i];
        i++;
    }
    set_status[i] = '\0';
}

static void set_request(void) { set_need = true; }

static int set_accent_index(void) {
    const display_settings_t *d = settings_get_display();
    uint32_t a = d ? (d->accent & 0xFFFFFFu) : 0x3DDC97;
    for (int i = 0; i < SET_NACCENT; i++)
        if (set_accents[i] == a) return i;
    return 0;
}

static void set_apply_key(const char *key, const char *val) {
    settings_set(key, val);
    settings_apply();
    set_status_msg("Applied");
    set_request();
}

static void set_row_action(int row) {
    char buf[32];
    const display_settings_t *d = settings_get_display();
    const audio_settings_t *a = settings_get_audio();
    const input_settings_t *in = settings_get_input();
    const network_settings_t *n = settings_get_network();
    switch (row) {
    case 0:
        set_apply_key("display.theme",
                      (d && d->theme == THEME_LIGHT) ? "dark" : "light");
        set_status_msg("Theme changed");
        break;
    case 1: {
        int i = (set_accent_index() + 1) % SET_NACCENT;
        snprintf(buf, sizeof(buf), "#%06x", set_accents[i]);
        set_apply_key("display.accent", buf);
        set_status_msg("Accent changed");
        break;
    }
    case 2: {
        int v = a ? (int)a->master_volume : 75;
        v -= 10;
        if (v < 0) v = 0;
        snprintf(buf, sizeof(buf), "%d", v);
        set_apply_key("audio.volume", buf);
        set_status_msg("Volume down");
        break;
    }
    case 3: {
        int v = a ? (int)a->master_volume : 75;
        v += 10;
        if (v > 100) v = 100;
        snprintf(buf, sizeof(buf), "%d", v);
        set_apply_key("audio.volume", buf);
        set_status_msg("Volume up");
        break;
    }
    case 4:
        set_apply_key("audio.muted", (a && a->muted) ? "false" : "true");
        set_status_msg((a && a->muted) ? "Sound on" : "Muted");
        break;
    case 5: {
        int lay = in ? in->keyboard_layout : 0;
        lay = (lay + 1) % 4;
        snprintf(buf, sizeof(buf), "%d", lay);
        set_apply_key("input.layout", buf);
        set_status_msg("Keyboard layout changed");
        break;
    }
    case 6: {
        int sc = d ? (int)d->font_scale : 1;
        sc++;
        if (sc > 4) sc = 1;
        snprintf(buf, sizeof(buf), "%d", sc);
        set_apply_key("display.font_scale", buf);
        set_status_msg("Console text size changed");
        break;
    }
    case 7:
        set_apply_key("display.cursor_blink",
                      (d && d->cursor_blink) ? "false" : "true");
        set_status_msg("Cursor blink toggled");
        break;
    case 8:
        set_apply_key("network.dhcp", (n && n->use_dhcp) ? "false" : "true");
        set_status_msg("DHCP toggled (saved on Save)");
        break;
    case 9:
        if (settings_save() == 0)
            set_status_msg("Saved to /SETTINGS.CFG");
        else
            set_status_msg("Save failed (no disk?)");
        set_request();
        break;
    default:
        break;
    }
}

static void set_redraw(void) {
    if (!set_win || !set_win->pixels) return;
    int w = set_win->content_w;
    int h = set_win->content_h;
    r_target_t t;
    r_target_init(&t, set_win->pixels, w, h, w);
    r_fill(&t, 0, 0, w, h, 0xFF12161E);
    r_fill(&t, 0, 0, w, 36, 0xFF1A2035);
    r_text(&t, 16, 10, "Settings", 0xFFF4F4F5);

    const display_settings_t *d = settings_get_display();
    const audio_settings_t *a = settings_get_audio();
    const input_settings_t *in = settings_get_input();
    const network_settings_t *n = settings_get_network();
    const char *layouts[] = { "US", "UK", "DE", "FR" };
    int lay = in ? in->keyboard_layout : 0;
    if (lay < 0 || lay > 3) lay = 0;

    char res[64], theme[64], accent[64], vol_dn[64], vol_up[64];
    char mute[64], kb[64], font[64], blink[64], dhcp[64];
    snprintf(res, sizeof(res), "Display  %ux%u  (boot framebuffer)",
             fb_get_width(), fb_get_height());
    snprintf(theme, sizeof(theme), "Theme          %s",
             (d && d->theme == THEME_LIGHT) ? "Light" : "Dark");
    snprintf(accent, sizeof(accent), "Accent         #%06x",
             d ? (d->accent & 0xFFFFFFu) : 0x3DDC97u);
    snprintf(vol_dn, sizeof(vol_dn), "Volume down    %d%%",
             a ? (int)a->master_volume : 0);
    snprintf(vol_up, sizeof(vol_up), "Volume up      %d%%",
             a ? (int)a->master_volume : 0);
    snprintf(mute, sizeof(mute), "Mute           %s",
             (a && a->muted) ? "On" : "Off");
    snprintf(kb, sizeof(kb), "Keyboard       %s", layouts[lay]);
    snprintf(font, sizeof(font), "Console text   %ux",
             d ? d->font_scale : 1);
    snprintf(blink, sizeof(blink), "Cursor blink   %s",
             (d && d->cursor_blink) ? "On" : "Off");
    snprintf(dhcp, sizeof(dhcp), "DHCP           %s",
             (n && n->use_dhcp) ? "On" : "Off");

    const char *rows[] = {
        theme, accent, vol_dn, vol_up, mute, kb, font, blink, dhcp,
        "Save settings"
    };
    int nrows = 10;
    for (int i = 0; i < nrows; i++) {
        int y = SET_TOP + i * SET_ROW;
        if (y + SET_ROW > h - 28) break;
        r_rrect(&t, 12, y, w - 24, SET_ROW - 6, 6, 0xFF1C2436);
        r_fill(&t, 12, y, 4, SET_ROW - 6, 0xFF3DDC97);
        r_text(&t, 28, y + 8, rows[i], 0xFFE5E9F0);
    }
    r_text(&t, 16, SET_TOP - 16, res, 0xFF94A3B8);
    r_fill(&t, 0, h - 24, w, 24, 0xFF0E121A);
    r_text(&t, 12, h - 18, set_status[0] ? set_status : "Click a row", 0xFF94A3B8);
    set_win->dirty = true;
    set_need = false;
}

void settings_app_open(void) {
    if (set_win && set_win->id) {
        desktop_focus_window(set_win);
        set_request();
        return;
    }
    int sw = (int)fb_get_width();
    int sh = (int)fb_get_height();
    int x = (sw - SET_W) / 2;
    int y = (sh - SET_H) / 3;
    if (x < 12) x = 12;
    if (y < 40) y = 40;
    set_win = desktop_create_window(0, x, y, SET_W, SET_H, "Settings");
    if (!set_win) return;
    set_status_msg("Click a row to change it");
    spinlock_lock(&set_win->content_lock);
    set_redraw();
    spinlock_unlock(&set_win->content_lock);
    serial_puts("[settings] opened\n");
}

bool settings_app_is_open(void) { return set_win && set_win->id != 0; }
bool settings_app_is_window(const desktop_window_t *win) {
    return win && set_win && win == set_win && set_win->id != 0;
}
void settings_app_on_window_destroyed(desktop_window_t *win) {
    if (win == set_win) set_win = NULL;
}

void settings_app_on_key(int32_t keycode, bool pressed) {
    (void)keycode; (void)pressed;
}

void settings_app_on_mouse(int rx, int ry, bool down) {
    if (!settings_app_is_open() || !down || !set_win) return;
    if (ry < SET_TOP) return;
    int row = (ry - SET_TOP) / SET_ROW;
    if (row < 0 || row > 9) return;
    int y = SET_TOP + row * SET_ROW;
    if (y + SET_ROW > set_win->content_h - 28) return;
    if (rx < 12 || rx >= set_win->content_w - 12) return;
    set_row_action(row);
}

bool settings_app_tick(void) {
    if (!settings_app_is_open() || !set_need) return false;
    spinlock_lock(&set_win->content_lock);
    set_redraw();
    spinlock_unlock(&set_win->content_lock);
    return true;
}
