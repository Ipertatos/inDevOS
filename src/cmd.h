#pragma once

#include "types.h"

#define CMD_MAX_LEN 256

void cmd(char* cmd, uint64_t len);
void clear();
void timer_test();
void mount_filesystem();
void mount_filesystem_device(uint8_t device_id);
void unmount_filesystem();
void list_files();
void filesystem_info();
void show_storage_devices();
void show_help();
int atoi(const char* str);

// File I/O commands
void cat_file(const char *filename);
void touch_file(const char *filename);
void make_directory(const char *dirname);
void remove_file(const char *filename);
void copy_file(const char *source, const char *dest);
void move_file(const char *source, const char *dest);
