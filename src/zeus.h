#pragma once

#include "types.h"

// Zeus Shell - Main command processor for inDevOS

// Version information
#define ZEUS_VERSION "1.0.0"
#define ZEUS_VERSION_MAJOR 1
#define ZEUS_VERSION_MINOR 0
#define ZEUS_VERSION_PATCH 0

// Function declarations
void zeus_main_loop(void);
int zeus_execute_external(const char* command_path, int argc, char* argv[]);

// Shell integration functions
void zeus_init_shell(void);
void zeus_start_shell(void);
void zeus_stop_shell(void);

// Command processing
int zeus_process_command(const char* command_line);
int zeus_parse_command_line(const char* command_line, char* argv[], int max_args);

// Built-in commands
void zeus_builtin_help(void);
void zeus_builtin_version(void);
void zeus_builtin_exit(void);

// External command execution
int zeus_launch_command(const char* command_name, char* argv[]);

// Kernel integration
void kernel_start_zeus(void);

// Service functions
void zeus_init_service(void);
/** Point the kernel shell at /home/user when that directory exists. */
void zeus_use_home_if_present(void);
int zeus_process_command_service(const char* command_line);
/** When set, the desktop terminal must not exec_file (that blocks the compositor). */
void zeus_set_no_external(int on);

// History navigation (for arrow-key support)
const char* zeus_history_prev(void);
const char* zeus_history_next(void);
void zeus_history_reset_nav(void);

// Pipe input access (for pipe-aware commands)
const char* zeus_get_pipe_input(size_t *out_len);

// Command vocabulary + tab completion.  zeus.c owns the shell's command list,
// so callers (e.g. the console) must not keep a second copy of it.
int zeus_tab_complete(const char *partial, uint32_t partial_len,
                      char *completion, uint32_t completion_max);
void zeus_print_command_matches(const char *partial, uint32_t partial_len);
bool zeus_has_command(const char *name);

/** Interactive prompt including cwd, e.g. "zeus:/os> " */
const char *zeus_get_prompt(void);
const char *zeus_get_cwd(void);
int zeus_get_last_status(void);
