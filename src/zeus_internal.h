#pragma once

#include "types.h"
#include "mem.h"

#ifndef ZEUS_VERSION
#define ZEUS_VERSION "1.0.0"
#endif

#define CMD_BUFFER_SIZE 256
#define MAX_ARGS 16

typedef struct {
    char current_dir[256];
    char prompt[64];
    char command_buffer[CMD_BUFFER_SIZE];
    int buffer_pos;
    int history_count;
    char history[50][CMD_BUFFER_SIZE];
    int history_index;
    int last_status;
} ZeusShell;

void zeus_init(ZeusShell *shell);
void zeus_execute_command(ZeusShell *shell, int argc, char *argv[]);
/* Copy the shell directory into the kernel session cwd (what exec inherits). */
void zeus_sync_process_cwd(const char *dir);

void zeus_cmd_cd(ZeusShell *shell, int argc, char *argv[]);
void zeus_cmd_pwd(ZeusShell *shell, int argc, char *argv[]);
void zeus_cmd_ls(ZeusShell *shell, int argc, char *argv[]);
void zeus_cmd_help(ZeusShell *shell, int argc, char *argv[]);
void zeus_cmd_clear(ZeusShell *shell, int argc, char *argv[]);
void zeus_cmd_history(ZeusShell *shell, int argc, char *argv[]);
void zeus_cmd_version(ZeusShell *shell, int argc, char *argv[]);
void zeus_cmd_cat(ZeusShell *shell, int argc, char *argv[]);
void zeus_cmd_echo(ZeusShell *shell, int argc, char *argv[]);
void zeus_cmd_touch(ZeusShell *shell, int argc, char *argv[]);
void zeus_cmd_mkdir(ZeusShell *shell, int argc, char *argv[]);
void zeus_cmd_system(ZeusShell *shell, int argc, char *argv[]);
void zeus_cmd_threadtest(ZeusShell *shell, int argc, char *argv[]);
void zeus_cmd_memtest(ZeusShell *shell, int argc, char *argv[]);
void zeus_cmd_grep(ZeusShell *shell, int argc, char *argv[]);
void zeus_cmd_wc(ZeusShell *shell, int argc, char *argv[]);
void zeus_cmd_head(ZeusShell *shell, int argc, char *argv[]);
void zeus_cmd_curl(ZeusShell *shell, int argc, char *argv[]);
void zeus_cmd_time(ZeusShell *shell, int argc, char *argv[]);
void zeus_cmd_uptime(void);

extern uint64_t zeus_time_check_us;
extern uint64_t zeus_time_exec_us;

int zeus_process_command_service(const char *command_line);
const char *zeus_get_pipe_input(size_t *out_len);
