#pragma once

// Driver command function declarations
void cmd_driver(const char *args);
void cmd_driver_help(void);
void cmd_driver_list(void);
void cmd_driver_load(const char *args);
void cmd_driver_unload(const char *args);
void cmd_driver_enable(const char *args);
void cmd_driver_disable(const char *args);
void cmd_driver_status(const char *args);
void cmd_driver_reset(const char *args);
void cmd_driver_probe(const char *args);
void cmd_driver_discover(const char *args);
void cmd_driver_save(const char *args); 