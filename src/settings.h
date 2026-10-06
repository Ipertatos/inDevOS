/**
 * @file settings.h
 * @brief System settings manager
 *
 * Provides persistent system configuration including display settings,
 * audio settings, network settings, and keyboard layout.
 */

#ifndef SETTINGS_H
#define SETTINGS_H

#include "types.h"

/** Settings categories */
typedef enum {
    SETTING_CAT_DISPLAY = 0,
    SETTING_CAT_AUDIO,
    SETTING_CAT_NETWORK,
    SETTING_CAT_INPUT,
    SETTING_CAT_SYSTEM,
    SETTING_CAT_COUNT
} settings_category_t;

#define THEME_DARK   0
#define THEME_LIGHT  1

/** Display settings */
typedef struct {
    uint32_t font_scale;        /**< Console font scale (1-4) */
    uint32_t fg_color;          /**< Default foreground color */
    uint32_t bg_color;          /**< Default background color */
    bool cursor_blink;          /**< Enable cursor blinking */
    uint32_t scrollback_lines;  /**< Number of scrollback lines */
    uint8_t  theme;             /**< THEME_DARK or THEME_LIGHT */
    uint32_t accent;            /**< Desktop accent (0xRRGGBB) */
} display_settings_t;

/** Audio settings */
typedef struct {
    uint8_t master_volume;      /**< Master volume (0-100) */
    uint8_t mixer_volume;       /**< Mixer master volume (0-255) */
    bool muted;                 /**< Global mute */
    uint32_t sample_rate;       /**< Preferred sample rate */
} audio_settings_t;

/** Network settings */
typedef struct {
    uint8_t ip_address[4];      /**< Static IP address */
    uint8_t subnet_mask[4];     /**< Subnet mask */
    uint8_t gateway[4];         /**< Default gateway */
    uint8_t dns_server[4];      /**< DNS server */
    bool use_dhcp;              /**< Use DHCP for configuration */
    char hostname[64];          /**< System hostname */
} network_settings_t;

/** Input settings */
typedef struct {
    int keyboard_layout;        /**< Keyboard layout ID (KB_LAYOUT_*) */
    uint32_t key_repeat_delay;  /**< Key repeat delay (ms) */
    uint32_t key_repeat_rate;   /**< Key repeat rate (ms) */
} input_settings_t;

/** System settings */
typedef struct {
    char hostname[64];          /**< System hostname */
    bool serial_debug;          /**< Enable serial debug output */
    uint32_t timezone_offset;   /**< Timezone offset from UTC (minutes) */
} system_settings_t;

/** Complete settings state */
typedef struct {
    display_settings_t display;
    audio_settings_t audio;
    network_settings_t network;
    input_settings_t input;
    system_settings_t system;
    bool initialized;
} system_settings_t_full;

/**
 * @brief Initialize the settings system with defaults
 * @return 0 on success
 */
int settings_init(void);

/**
 * @brief Get display settings (read-only)
 * @return Pointer to display settings
 */
const display_settings_t* settings_get_display(void);

/**
 * @brief Get audio settings (read-only)
 * @return Pointer to audio settings
 */
const audio_settings_t* settings_get_audio(void);

/**
 * @brief Get network settings (read-only)
 * @return Pointer to network settings
 */
const network_settings_t* settings_get_network(void);

/**
 * @brief Get input settings (read-only)
 * @return Pointer to input settings
 */
const input_settings_t* settings_get_input(void);

/**
 * @brief Get system settings (read-only)
 * @return Pointer to system settings
 */
const system_settings_t* settings_get_system(void);

/**
 * @brief Apply a setting by key/value string
 * @param key   Setting key (e.g. "display.font_scale")
 * @param value Setting value as string
 * @return 0 on success, -1 on error
 */
int settings_set(const char *key, const char *value);

/**
 * @brief Get a setting value as string
 * @param key   Setting key
 * @param buf   Output buffer
 * @param buflen Buffer size
 * @return 0 on success, -1 if key not found
 */
int settings_get(const char *key, char *buf, size_t buflen);

/**
 * @brief Print all settings to the terminal
 */
void settings_print_all(void);

/**
 * @brief Apply settings to active subsystems
 *
 * Pushes current settings to the display, audio, network, etc.
 */
void settings_apply(void);

/**
 * @brief Save settings to persistent storage (FAT32)
 * @return 0 on success, -1 on error
 */
int settings_save(void);

/**
 * @brief Load settings from persistent storage (FAT32)
 * @return 0 on success, -1 if file not found
 */
int settings_load(void);

#endif /* SETTINGS_H */
