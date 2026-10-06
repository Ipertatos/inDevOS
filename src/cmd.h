#pragma once

#include "types.h"

#define CMD_MAX_LEN 256

/* ================================================================
 * Terminal multiplexer API
 *
 * cmd.c acts as a dispatch layer that routes raw input to the active
 * terminal backend. By default all input is forwarded to Zeus.
 * Additional backends (debug consoles, serial shells, etc.) can be
 * registered here and made active at runtime.
 * ================================================================ */

/** Handler function type for a terminal backend. */
typedef int (*cmd_terminal_fn)(const char *line, size_t len);

/**
 * Register a named terminal backend.
 * If a terminal with the same name already exists its handler is updated.
 * Returns the terminal index on success, -1 on failure.
 */
int cmd_register_terminal(const char *name, cmd_terminal_fn handler);

/**
 * Set the active terminal by name.
 * Pass NULL to revert to the default backend (zeus).
 * Returns 0 on success, -1 if the name is not found.
 */
int cmd_set_active_terminal(const char *name);

/** Returns the name of the currently active terminal (never NULL). */
const char *cmd_get_active_terminal(void);

/** Print all registered terminals to the console. */
int cmd_list_terminals(void);

/* ================================================================
 * Core dispatch function (called from the keyboard/input layer)
 * ================================================================ */
void cmd(char *input, uint64_t len);

/* ================================================================
 * Shared utility functions
 * (legacy helpers – prefer using Zeus built-ins where possible)
 * ================================================================ */
void clear(void);
void mount_filesystem(void);
void mount_filesystem_device(uint8_t device_id);
void mount_filesystem_ext(uint8_t device_id, uint32_t partition_lba,
                          const char *fstype, const char *mount_path);
void unmount_filesystem(void);
void unmount_filesystem_path(const char *path);
void list_files(void);
void filesystem_info(void);
void show_storage_devices(void);
void show_help(void);
int  atoi(const char *str);

/* File I/O helpers */
void cat_file(const char *filename);
void touch_file(const char *filename);
void make_directory(const char *dirname);
void remove_file(const char *filename);
void copy_file(const char *source, const char *dest);
void move_file(const char *source, const char *dest);

/* Network helpers */
void show_network_status(void);
void show_mac_address(void);
int  parse_ip_address(const char *ip_str, uint8_t *ip_out);
int  cmd_netset(int argc, char **argv);

/* USB / input helpers */
void show_usb_status(void);
void usb_enumerate_devices(void);
void usb_keyboard_test(void);
void usb_keyboard_poll_test(void);
void cmd_inputinfo(void);

/* Image helpers */
void cmd_img(const char *filename);

/* Timer helpers */
void timer_test(void);
void timer_accuracy_test(void);
void stopwatch_test(void);
void gpu_test(void);

/* System information (`neofetch`) – reads kernel state, so it stays in the
 * kernel; formerly src/usoft/info.c. */
void print_info(void);
void format_uptime(uint64_t ticks, char *buffer, size_t buffer_size);
void format_memory_size(uint64_t bytes, char *buffer, size_t buffer_size);

/* Tab completion lives in zeus.h (zeus_tab_complete) – the shell owns the
 * command vocabulary, cmd.c no longer keeps a second copy of it. */

