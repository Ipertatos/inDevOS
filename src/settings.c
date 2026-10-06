/**
 * @file settings.c
 * @brief System settings manager implementation
 */

#include "settings.h"
#include "serial.h"
#include "term.h"
#include "vfs.h"
#include "mem.h"
#include "string.h"

extern int snprintf(char *str, size_t size, const char *fmt, ...);
extern int atoi(const char *s);

/* Global settings state */
static system_settings_t_full settings = {0};

/* Settings file path on FAT32 */
#define SETTINGS_FILE "/SETTINGS.CFG"

/* ============================================================================
 * Defaults
 * ============================================================================ */

static void settings_set_defaults(void) {
    /* Display */
    settings.display.font_scale = 1;
    settings.display.fg_color = 0xFFFFFF;
    settings.display.bg_color = 0x000000;
    settings.display.cursor_blink = true;
    settings.display.scrollback_lines = 200;
    settings.display.theme = THEME_DARK;
    settings.display.accent = 0x3DDC97;

    /* Audio */
    settings.audio.master_volume = 75;
    settings.audio.mixer_volume = 200;
    settings.audio.muted = false;
    settings.audio.sample_rate = 44100;

    /* Network */
    settings.network.ip_address[0] = 10;
    settings.network.ip_address[1] = 0;
    settings.network.ip_address[2] = 2;
    settings.network.ip_address[3] = 15;
    settings.network.subnet_mask[0] = 255;
    settings.network.subnet_mask[1] = 255;
    settings.network.subnet_mask[2] = 255;
    settings.network.subnet_mask[3] = 0;
    settings.network.gateway[0] = 10;
    settings.network.gateway[1] = 0;
    settings.network.gateway[2] = 2;
    settings.network.gateway[3] = 2;
    settings.network.dns_server[0] = 10;
    settings.network.dns_server[1] = 0;
    settings.network.dns_server[2] = 2;
    settings.network.dns_server[3] = 3;
    settings.network.use_dhcp = true;
    strncpy(settings.network.hostname, "indevos", sizeof(settings.network.hostname));

    /* Input */
    settings.input.keyboard_layout = 0;  /* US layout */
    settings.input.key_repeat_delay = 500;
    settings.input.key_repeat_rate = 30;

    /* System */
    strncpy(settings.system.hostname, "indevos", sizeof(settings.system.hostname));
    settings.system.serial_debug = true;
    settings.system.timezone_offset = 0;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

int settings_init(void) {
    settings_set_defaults();
    settings.initialized = true;

    /* Try to load from disk */
    if (settings_load() == 0) {
        serial_puts("settings: loaded from " SETTINGS_FILE "\n");
    } else {
        serial_puts("settings: using defaults\n");
    }

    return 0;
}

const display_settings_t* settings_get_display(void) {
    return &settings.display;
}

const audio_settings_t* settings_get_audio(void) {
    return &settings.audio;
}

const network_settings_t* settings_get_network(void) {
    return &settings.network;
}

const input_settings_t* settings_get_input(void) {
    return &settings.input;
}

const system_settings_t* settings_get_system(void) {
    return &settings.system;
}

/* Helper: parse an IP address string "a.b.c.d" into 4 bytes */
static int parse_ip(const char *str, uint8_t ip[4]) {
    int parts = 0;
    uint32_t val = 0;
    const char *p = str;
    while (*p && parts < 4) {
        if (*p >= '0' && *p <= '9') {
            val = val * 10 + (*p - '0');
            if (val > 255) return -1;
        } else if (*p == '.') {
            ip[parts++] = (uint8_t)val;
            val = 0;
        } else {
            return -1;
        }
        p++;
    }
    if (parts == 3) { ip[3] = (uint8_t)val; return 0; }
    return -1;
}

/* Helper: parse a hex color "#RRGGBB" or "RRGGBB" */
static uint32_t parse_color(const char *str) {
    if (*str == '#') str++;
    uint32_t color = 0;
    for (int i = 0; i < 6 && *str; i++, str++) {
        color <<= 4;
        if (*str >= '0' && *str <= '9') color |= (*str - '0');
        else if (*str >= 'a' && *str <= 'f') color |= (*str - 'a' + 10);
        else if (*str >= 'A' && *str <= 'F') color |= (*str - 'A' + 10);
    }
    return color;
}

int settings_set(const char *key, const char *value) {
    if (!settings.initialized || !key || !value) return -1;

    /* Display settings */
    if (strcmp(key, "display.font_scale") == 0) {
        int v = atoi(value);
        if (v >= 1 && v <= 4) settings.display.font_scale = (uint32_t)v;
    } else if (strcmp(key, "display.fg_color") == 0) {
        settings.display.fg_color = parse_color(value);
    } else if (strcmp(key, "display.bg_color") == 0) {
        settings.display.bg_color = parse_color(value);
    } else if (strcmp(key, "display.cursor_blink") == 0) {
        settings.display.cursor_blink = (strcmp(value, "true") == 0 || atoi(value) != 0);
    } else if (strcmp(key, "display.theme") == 0) {
        if (strcmp(value, "light") == 0 || atoi(value) == THEME_LIGHT)
            settings.display.theme = THEME_LIGHT;
        else
            settings.display.theme = THEME_DARK;
    } else if (strcmp(key, "display.accent") == 0) {
        settings.display.accent = parse_color(value);
    }
    /* Audio settings */
    else if (strcmp(key, "audio.volume") == 0) {
        int v = atoi(value);
        if (v >= 0 && v <= 100) settings.audio.master_volume = (uint8_t)v;
    } else if (strcmp(key, "audio.mixer_volume") == 0) {
        int v = atoi(value);
        if (v >= 0 && v <= 255) settings.audio.mixer_volume = (uint8_t)v;
    } else if (strcmp(key, "audio.muted") == 0) {
        settings.audio.muted = (strcmp(value, "true") == 0 || atoi(value) != 0);
    }
    /* Network settings */
    else if (strcmp(key, "network.ip") == 0) {
        parse_ip(value, settings.network.ip_address);
    } else if (strcmp(key, "network.mask") == 0) {
        parse_ip(value, settings.network.subnet_mask);
    } else if (strcmp(key, "network.gateway") == 0) {
        parse_ip(value, settings.network.gateway);
    } else if (strcmp(key, "network.dns") == 0) {
        parse_ip(value, settings.network.dns_server);
    } else if (strcmp(key, "network.dhcp") == 0) {
        settings.network.use_dhcp = (strcmp(value, "true") == 0 || atoi(value) != 0);
    } else if (strcmp(key, "network.hostname") == 0) {
        strncpy(settings.network.hostname, value, sizeof(settings.network.hostname) - 1);
    }
    /* Input settings */
    else if (strcmp(key, "input.layout") == 0) {
        settings.input.keyboard_layout = atoi(value);
    }
    /* System settings */
    else if (strcmp(key, "system.hostname") == 0) {
        strncpy(settings.system.hostname, value, sizeof(settings.system.hostname) - 1);
    } else if (strcmp(key, "system.serial_debug") == 0) {
        settings.system.serial_debug = (strcmp(value, "true") == 0 || atoi(value) != 0);
    } else if (strcmp(key, "system.timezone") == 0) {
        settings.system.timezone_offset = (uint32_t)atoi(value);
    } else {
        return -1;  /* Unknown key */
    }

    return 0;
}

int settings_get(const char *key, char *buf, size_t buflen) {
    if (!settings.initialized || !key || !buf || buflen == 0) return -1;

    if (strcmp(key, "display.font_scale") == 0) {
        snprintf(buf, buflen, "%d", settings.display.font_scale);
    } else if (strcmp(key, "display.fg_color") == 0) {
        snprintf(buf, buflen, "#%06x", settings.display.fg_color);
    } else if (strcmp(key, "display.bg_color") == 0) {
        snprintf(buf, buflen, "#%06x", settings.display.bg_color);
    } else if (strcmp(key, "display.theme") == 0) {
        snprintf(buf, buflen, "%s", settings.display.theme == THEME_LIGHT ? "light" : "dark");
    } else if (strcmp(key, "display.accent") == 0) {
        snprintf(buf, buflen, "#%06x", settings.display.accent);
    } else if (strcmp(key, "audio.volume") == 0) {
        snprintf(buf, buflen, "%d", settings.audio.master_volume);
    } else if (strcmp(key, "audio.muted") == 0) {
        snprintf(buf, buflen, "%s", settings.audio.muted ? "true" : "false");
    } else if (strcmp(key, "network.ip") == 0) {
        snprintf(buf, buflen, "%d.%d.%d.%d",
                 settings.network.ip_address[0], settings.network.ip_address[1],
                 settings.network.ip_address[2], settings.network.ip_address[3]);
    } else if (strcmp(key, "network.dhcp") == 0) {
        snprintf(buf, buflen, "%s", settings.network.use_dhcp ? "true" : "false");
    } else if (strcmp(key, "system.hostname") == 0) {
        strncpy(buf, settings.system.hostname, buflen - 1);
    } else {
        return -1;
    }
    return 0;
}

void settings_print_all(void) {
    printf("=== System Settings ===\n\n");

    printf("[Display]\n");
    printf("  font_scale     = %d\n", settings.display.font_scale);
    printf("  fg_color       = #%06x\n", settings.display.fg_color);
    printf("  bg_color       = #%06x\n", settings.display.bg_color);
    printf("  cursor_blink   = %s\n", settings.display.cursor_blink ? "true" : "false");
    printf("  scrollback     = %d lines\n", settings.display.scrollback_lines);
    printf("  theme          = %s\n", settings.display.theme == THEME_LIGHT ? "light" : "dark");
    printf("  accent         = #%06x\n", settings.display.accent);

    printf("\n[Audio]\n");
    printf("  volume         = %d%%\n", settings.audio.master_volume);
    printf("  mixer_volume   = %d\n", settings.audio.mixer_volume);
    printf("  muted          = %s\n", settings.audio.muted ? "true" : "false");
    printf("  sample_rate    = %d Hz\n", settings.audio.sample_rate);

    printf("\n[Network]\n");
    printf("  ip             = %d.%d.%d.%d\n",
           settings.network.ip_address[0], settings.network.ip_address[1],
           settings.network.ip_address[2], settings.network.ip_address[3]);
    printf("  mask           = %d.%d.%d.%d\n",
           settings.network.subnet_mask[0], settings.network.subnet_mask[1],
           settings.network.subnet_mask[2], settings.network.subnet_mask[3]);
    printf("  gateway        = %d.%d.%d.%d\n",
           settings.network.gateway[0], settings.network.gateway[1],
           settings.network.gateway[2], settings.network.gateway[3]);
    printf("  dns            = %d.%d.%d.%d\n",
           settings.network.dns_server[0], settings.network.dns_server[1],
           settings.network.dns_server[2], settings.network.dns_server[3]);
    printf("  dhcp           = %s\n", settings.network.use_dhcp ? "true" : "false");
    printf("  hostname       = %s\n", settings.network.hostname);

    printf("\n[Input]\n");
    printf("  layout         = %d\n", settings.input.keyboard_layout);
    printf("  repeat_delay   = %d ms\n", settings.input.key_repeat_delay);
    printf("  repeat_rate    = %d ms\n", settings.input.key_repeat_rate);

    printf("\n[System]\n");
    printf("  hostname       = %s\n", settings.system.hostname);
    printf("  serial_debug   = %s\n", settings.system.serial_debug ? "true" : "false");
    printf("  timezone       = UTC%+d min\n", (int)settings.system.timezone_offset);
}

void settings_apply(void) {
    if (!settings.initialized) return;

    /* Apply display settings */
    extern void fb_set_font_scale(uint32_t scale);
    extern void console_set_fg(uint32_t color);
    fb_set_font_scale(settings.display.font_scale);
    console_set_fg(settings.display.fg_color);
    extern void desktop_theme_reload(void);
    desktop_theme_reload();

    /* Apply audio settings */
    extern void ac97_set_volume(uint8_t volume);
    extern void mixer_set_master_volume(uint8_t volume);
    if (!settings.audio.muted) {
        ac97_set_volume(settings.audio.master_volume);
        mixer_set_master_volume(settings.audio.mixer_volume);
    } else {
        ac97_set_volume(0);
        mixer_set_master_volume(0);
    }

    /* Apply keyboard layout */
    extern void keyboard_set_layout(int layout_id);
    keyboard_set_layout(settings.input.keyboard_layout);

    serial_puts("settings: applied to subsystems\n");
}

int settings_save(void) {
    if (!settings.initialized) return -1;
    if (!vfs_has_root_mount()) return -1;

    /* Create a simple key=value text file */
    char *buf = (char *)kalloc(2048);
    if (!buf) return -1;

    int pos = 0;
    pos += snprintf(buf + pos, 2048 - pos, "# inDevOS Settings\n");
    pos += snprintf(buf + pos, 2048 - pos, "display.font_scale=%d\n", settings.display.font_scale);
    pos += snprintf(buf + pos, 2048 - pos, "display.fg_color=#%06x\n", settings.display.fg_color);
    pos += snprintf(buf + pos, 2048 - pos, "display.bg_color=#%06x\n", settings.display.bg_color);
    pos += snprintf(buf + pos, 2048 - pos, "display.cursor_blink=%s\n", settings.display.cursor_blink ? "true" : "false");
    pos += snprintf(buf + pos, 2048 - pos, "display.theme=%s\n",
                    settings.display.theme == THEME_LIGHT ? "light" : "dark");
    pos += snprintf(buf + pos, 2048 - pos, "display.accent=#%06x\n", settings.display.accent);
    pos += snprintf(buf + pos, 2048 - pos, "audio.volume=%d\n", settings.audio.master_volume);
    pos += snprintf(buf + pos, 2048 - pos, "audio.mixer_volume=%d\n", settings.audio.mixer_volume);
    pos += snprintf(buf + pos, 2048 - pos, "audio.muted=%s\n", settings.audio.muted ? "true" : "false");
    pos += snprintf(buf + pos, 2048 - pos, "network.dhcp=%s\n", settings.network.use_dhcp ? "true" : "false");
    pos += snprintf(buf + pos, 2048 - pos, "network.ip=%d.%d.%d.%d\n",
                    settings.network.ip_address[0], settings.network.ip_address[1],
                    settings.network.ip_address[2], settings.network.ip_address[3]);
    pos += snprintf(buf + pos, 2048 - pos, "network.hostname=%s\n", settings.network.hostname);
    pos += snprintf(buf + pos, 2048 - pos, "input.layout=%d\n", settings.input.keyboard_layout);
    pos += snprintf(buf + pos, 2048 - pos, "system.hostname=%s\n", settings.system.hostname);
    pos += snprintf(buf + pos, 2048 - pos, "system.serial_debug=%s\n", settings.system.serial_debug ? "true" : "false");

    /* Write to file via VFS */
    int rc = vfs_write_file(SETTINGS_FILE, buf, (uint32_t)pos);
    kfree(buf);
    if (rc != 0) return -1;

    serial_puts("settings: saved to " SETTINGS_FILE "\n");
    return 0;
}

int settings_load(void) {
    if (!vfs_has_root_mount()) return -1;

    void *vbuf = NULL;
    int rd = vfs_read_file_alloc(SETTINGS_FILE, &vbuf, 2047);
    if (rd <= 0 || !vbuf) { if (vbuf) kfree(vbuf); return -1; }
    char *buf = (char *)vbuf;  /* NUL-terminated by vfs_read_file_alloc */

    /* Parse key=value lines */
    char *line = buf;
    while (*line) {
        /* Skip comments and empty lines */
        if (*line == '#' || *line == '\n' || *line == '\r') {
            while (*line && *line != '\n') line++;
            if (*line == '\n') line++;
            continue;
        }

        /* Find '=' */
        char *eq = line;
        while (*eq && *eq != '=' && *eq != '\n') eq++;
        if (*eq != '=') {
            while (*line && *line != '\n') line++;
            if (*line == '\n') line++;
            continue;
        }

        *eq = '\0';
        char *key = line;
        char *value = eq + 1;

        /* Find end of value */
        char *eol = value;
        while (*eol && *eol != '\n' && *eol != '\r') eol++;
        char saved = *eol;
        *eol = '\0';

        /* Apply the setting */
        settings_set(key, value);

        *eol = saved;
        line = eol;
        if (*line == '\r') line++;
        if (*line == '\n') line++;
    }

    kfree(buf);
    return 0;
}
