#include "types.h"
#include "zeus_internal.h"
#include "string.h"
#include "mem.h"
#include "exec.h"
#include "fat32.h"
#include "vfs.h"
#include "term.h"
#include "keyboard.h"
#include "cmd.h"
#include "hpet.h"
#include "ktime.h"       /* ktime_now_us() for the `time` command */
#include "rtc.h"
#include "storage.h"
#include "atomic.h"
#include "utils.h"
#include "net_tcp.h"
#include "net_dns.h"
#include "net_device.h"
#include "serial.h"
#include "users.h"
#include "debug.h"       /* ktrace_set()/ktrace_enabled for the `trace` command */
#include "syscall.h"
#include "profiler.h"
#include "thread.h"
#include "vconsole.h"
#include "tui.h"
#include "rtl8139.h"
#include "e1000.h"
#include "string.h"

/* Forward declaration for snprintf (implemented in term.c) */
extern int snprintf(char *str, size_t size, const char *fmt, ...);
#include "string.h"
#include "stdio.h"

// Forward declaration for missing functions
void exit(int status);

// Simple implementation of exit
void exit(int status) {
    process_t *p = get_current_process();
    if (p && (p->is_usermode || p->fork_parent || p->sched_thread))
        sys_exit(status);
    printf("exit(%d) from kernel context ignored\n", status);
}

/* Parse an octal string like "755" -> 0755 */
static uint32_t strtol_octal(const char *s) {
    uint32_t val = 0;
    while (*s >= '0' && *s <= '7') {
        val = val * 8 + (*s - '0');
        s++;
    }
    return val;
}

static ZeusShell *global_shell = NULL;

int  zeus_parse_command(ZeusShell* shell, char* args[MAX_ARGS]);
void zeus_add_to_history(ZeusShell* shell, const char* command);

/* Filled in by zeus_run_fs_program() and reported by the `time` built-in. */
uint64_t zeus_time_check_us = 0;
uint64_t zeus_time_exec_us  = 0;

void zeus_execute_external(const char* command_path, int argc, char* argv[]);

/* ================================================================
 * time <program> [args...]  -  wall-clock timing for any command
 *
 * Runs the command through the normal dispatch path (so it measures whatever
 * the user would have run: a /name.exe program, a path, or a built-in) and
 * reports how long it took.  For a program on disk the number includes loading
 * it - which is where most of the time goes for a small utility, so the report
 * also breaks out the parts of program start-up that the shell can see.
 * ================================================================ */
/* zeus_cmd_time / uptime live in zeus_cmds.c */

const char* zeus_get_pipe_input(size_t *out_len);

// HTTP server globals
static volatile bool http_server_running = false;

// HTTP server connection handler
void http_server_handle_connection(tcp_connection_t *conn) {
    printf("HTTP: New connection from %d.%d.%d.%d:%d\n",
           conn->remote_ip[0], conn->remote_ip[1], 
           conn->remote_ip[2], conn->remote_ip[3],
           conn->remote_port);
    
    // Set up data callback to send HTTP response
    conn->data_callback = NULL;  // We'll send response immediately
    
    // Simple HTTP response
    const char *http_response = 
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\n"
        "\r\n"
        "<!DOCTYPE html>\n"
        "<html>\n"
        "<head><title>inDevOS HTTP Server</title></head>\n"
        "<body style='font-family: monospace; padding: 20px;'>\n"
        "<h1>🚀 Hello World!</h1>\n"
        "<p>This is a simple HTTP server running on <b>inDevOS</b>!</p>\n"
        "<p>Built with interrupt-driven networking for ultra-low latency.</p>\n"
        "<hr>\n"
        "<small>Powered by inDevOS | RTL8139 NIC | TCP/IP Stack</small>\n"
        "</body>\n"
        "</html>\n";
    
    // Send response
    tcp_send(conn, (const uint8_t *)http_response, strlen(http_response));
    
    // Close connection after sending
    tcp_close(conn);
    
    printf("HTTP: Response sent, connection closed\n");
}

// Main shell loop
// Removed zeus_main_loop - now using service-based approach

void zeus_init(ZeusShell* shell) {
    memset(shell, 0, sizeof(ZeusShell));

    // Initialize current directory
    kstrncpy(shell->current_dir, "/", sizeof(shell->current_dir));
    if (vfs_exists("/home/user"))
        kstrncpy(shell->current_dir, "/home/user", sizeof(shell->current_dir));
    zeus_sync_process_cwd(shell->current_dir);

    // Initialize prompt
    kstrncpy(shell->prompt, "zeus:/ > ", sizeof(shell->prompt));
    shell->last_status = 0;

    // Initialize history
    shell->history_count = 0;
    shell->history_index = 0;
}

#define ZEUS_ENV_MAX 32
#define ZEUS_ENV_KEY 32
#define ZEUS_ENV_VAL 160

static char zeus_env_k[ZEUS_ENV_MAX][ZEUS_ENV_KEY];
static char zeus_env_v[ZEUS_ENV_MAX][ZEUS_ENV_VAL];
static int  zeus_env_n;

const char *zeus_get_cwd(void) {
    return global_shell ? global_shell->current_dir : "/";
}

void zeus_use_home_if_present(void) {
    if (!global_shell) return;
    if (vfs_exists("/home/user"))
        kstrncpy(global_shell->current_dir, "/home/user",
                 sizeof(global_shell->current_dir));
    zeus_sync_process_cwd(global_shell->current_dir);
}

void zeus_sync_process_cwd(const char *dir) {
    if (!dir || !dir[0])
        dir = "/";
    /* Kernel Zeus is not a usermode process. Keep its cwd equal to the
     * shell directory so exec_file's new process (and getcwd) follow `cd`.
     * Leave a live usermode cwd alone — userspace sh updates that via chdir. */
    process_t *kern = get_kernel_process();
    process_set_cwd(kern, dir);
    process_t *cur = get_current_process();
    if (cur && cur != kern && !cur->is_usermode)
        process_set_cwd(cur, dir);
}

int zeus_get_last_status(void) {
    return global_shell ? global_shell->last_status : 0;
}

const char *zeus_get_prompt(void) {
    static char buf[80];
    const char *cwd = zeus_get_cwd();
    if (!cwd || !cwd[0]) cwd = "/";
    snprintf(buf, sizeof(buf), "zeus:%s> ", cwd);
    return buf;
}

static const char *zeus_env_get(const char *key) {
    static char status_buf[12];
    if (!key || !key[0]) return "";
    if (key[0] == '?' && key[1] == '\0') {
        snprintf(status_buf, sizeof(status_buf), "%d", zeus_get_last_status());
        return status_buf;
    }
    for (int i = 0; i < zeus_env_n; i++) {
        if (kstrcmp(zeus_env_k[i], key) == 0)
            return zeus_env_v[i];
    }
    return "";
}

static int zeus_env_set(const char *key, const char *val) {
    if (!key || !key[0]) return -1;
    for (int i = 0; i < zeus_env_n; i++) {
        if (kstrcmp(zeus_env_k[i], key) == 0) {
            kstrncpy(zeus_env_v[i], val ? val : "", sizeof(zeus_env_v[i]));
            return 0;
        }
    }
    if (zeus_env_n >= ZEUS_ENV_MAX) return -1;
    kstrncpy(zeus_env_k[zeus_env_n], key, sizeof(zeus_env_k[0]));
    kstrncpy(zeus_env_v[zeus_env_n], val ? val : "", sizeof(zeus_env_v[0]));
    zeus_env_n++;
    return 0;
}

static void zeus_env_unset(const char *key) {
    for (int i = 0; i < zeus_env_n; i++) {
        if (kstrcmp(zeus_env_k[i], key) == 0) {
            zeus_env_n--;
            if (i < zeus_env_n) {
                memcpy(zeus_env_k[i], zeus_env_k[zeus_env_n], sizeof(zeus_env_k[0]));
                memcpy(zeus_env_v[i], zeus_env_v[zeus_env_n], sizeof(zeus_env_v[0]));
            }
            return;
        }
    }
}

static int zeus_is_env_name_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static void zeus_expand_vars(char *dst, size_t dstsz, const char *src) {
    size_t di = 0;
    for (size_t i = 0; src[i] && di + 1 < dstsz; ) {
        if (src[i] == '$') {
            i++;
            char name[ZEUS_ENV_KEY];
            size_t ni = 0;
            if (src[i] == '{') {
                i++;
                while (src[i] && src[i] != '}' && ni + 1 < sizeof(name))
                    name[ni++] = src[i++];
                if (src[i] == '}') i++;
            } else if (src[i] == '?') {
                name[ni++] = '?';
                i++;
            } else {
                while (zeus_is_env_name_char(src[i]) && ni + 1 < sizeof(name))
                    name[ni++] = src[i++];
            }
            name[ni] = '\0';
            const char *val = zeus_env_get(name);
            while (*val && di + 1 < dstsz)
                dst[di++] = *val++;
        } else {
            dst[di++] = src[i++];
        }
    }
    dst[di] = '\0';
}

// Removed zeus_print_prompt and zeus_read_command functions - using service-based approach

int zeus_parse_command_line(const char* command_line, char* args[MAX_ARGS]) {
    int argc = 0;
    static char* buffer = NULL;

    if (buffer == NULL) {
        buffer = kalloc(CMD_BUFFER_SIZE);
        if (!buffer) {
            printf("Error: Out of memory\n");
            return 0;
        }
    }
    memset(buffer, 0, CMD_BUFFER_SIZE);
    if (!command_line) {
        args[0] = NULL;
        return 0;
    }
    kstrncpy(buffer, command_line, CMD_BUFFER_SIZE - 1);
    buffer[CMD_BUFFER_SIZE - 1] = '\0';
    if (buffer[0] == '\0') {
        args[0] = NULL;
        return 0;
    }

    /* Quote-aware argv split: strip matching quotes; honour \ outside '. */
    char *r = buffer;
    while (*r && argc < MAX_ARGS - 1) {
        while (*r == ' ' || *r == '\t' || *r == '\r' || *r == '\n' ||
               *r == '\b' || (unsigned char)*r == 0x7f)
            r++;
        if (!*r) break;

        args[argc] = r;
        char *w = r;
        int sq = 0, dq = 0;
        while (*r) {
            if (!sq && !dq &&
                (*r == ' ' || *r == '\t' || *r == '\r' || *r == '\n' ||
                 *r == '\b' || (unsigned char)*r == 0x7f))
                break;
            if (!sq && !dq && *r == '\\' && r[1]) {
                *w++ = r[1];
                r += 2;
                continue;
            }
            if (!dq && *r == '\'') { sq = !sq; r++; continue; }
            if (!sq && *r == '"') { dq = !dq; r++; continue; }
            if (dq && *r == '\\' && r[1]) {
                *w++ = r[1];
                r += 2;
                continue;
            }
            *w++ = *r++;
        }
        if (*r) r++;
        *w = '\0';
        argc++;
    }
    args[argc] = NULL;
    return argc;
}

/* Try argv[0] as a path to an ELF on the VFS (e.g. /apps/foo, cwd-relative). */
static bool zeus_try_run_executable(ZeusShell* shell, int argc, char* argv[]) {
    (void)argc;
    if (!argv[0] || !vfs_has_root_mount()) {
        return false;
    }
    const char* name = argv[0];
    char path[256];

    if (name[0] == '/') {
        kstrncpy(path, name, sizeof(path));
    } else {
        kstrncpy(path, shell->current_dir, sizeof(path));
        size_t cl = strlen(path);
        if (cl > 1 && path[cl - 1] != '/' && cl + 1 < sizeof(path)) {
            path[cl] = '/';
            path[cl + 1] = '\0';
        }
        size_t pl = strlen(path);
        size_t nl = strlen(name);
        if (pl + nl + 1 >= sizeof(path)) {
            return false;
        }
        memcpy(path + pl, name, nl);
        path[pl + nl] = '\0';
    }

    if (!vfs_exists(path)) {
        /* Tight PATH: cwd/name.exe, /bin, /, /apps. */
        static const char *const fmts[] = {
            "%s.exe", "/bin/%s.exe", "/%s.exe", "/apps/%s.exe", (const char *)0
        };
        char alt[256];
        bool found = false;
        for (int i = 0; fmts[i] && !found; i++) {
            if (i == 0) {
                if (name[0] == '/') continue;
                snprintf(alt, sizeof(alt), fmts[i], path);
            } else {
                snprintf(alt, sizeof(alt), fmts[i], name);
            }
            found = vfs_exists(alt);
        }
        if (!found) {
            return false;
        }
        kstrncpy(path, alt, sizeof(path));
    }

    /* The new process inherits get_current_process()->cwd. Zeus runs on a
     * kernel thread, but current_process may still point at a usermode app
     * (scheduler overwrites the global). Bind the kernel session first so
     * `ls` with no args lists this shell directory, not /home/user. */
    {
        process_t *kern = get_kernel_process();
        process_t *cur = get_current_process();
        if (kern && (!cur || cur->is_usermode))
            set_current_process(kern);
        zeus_sync_process_cwd(shell->current_dir);
    }

    /* Run it.  Use the blocking exec path (exec_file), not exec_spawn: a
     * console program runs in Ring 3 on this kernel thread and returns through
     * exec_run_elf's setjmp, so the shell continues cleanly.  exec_spawn
     * schedules a separate thread whose exit has to be resumed by the timer
     * ISR, which cannot switch a kernel thread's stack (a same-privilege iret
     * does not restore RSP) — that crashed the kernel right after a program
     * exited.  Windowed apps are still spawned by the desktop. */
    exec_file(path, argv);
    return true;
}

/*
 * Known userspace programs (apps/<name>.c → /<name>.exe).  Kept so
 * tools/check_zeus_commands.sh and tab completion stay in sync; dispatch
 * itself no longer uses this whitelist — any VFS executable wins.
 */
static const char *const zeus_fs_programs[] = {
    "ls", "cat", "mkdir", "touch", "rm", "echo", "clear", "help", "pwd",
    "neofetch", "wc", "head", "cp", "mv", "matrix", "grep", "date", "stopwatch",
    "sh", "udp_test",
    (const char *)0
};

/*
 * The shell's command vocabulary — the single list of names
 * zeus_execute_command() accepts, used by tab completion and by the
 * multi-match listing.  Names handled by zeus_fs_programs (real programs on
 * disk) are included too, so completion still offers them.
 *
 * tools/check_zeus_commands.sh re-derives this list from the dispatcher and
 * fails if the two ever drift apart.
 */
static const char *const zeus_command_names[] = {
    "addarp", "arp", "arpstatus", "arptest", "audio", "cachestats",
    "cat", "cd", "checkpackets", "chmod", "chown", "clear",
    "clip", "clipboard", "cmd", "cp", "curl", "date",
    "ddhcp", "desktop", "desktopstop", "dhcp", "dir", "dlopen",
    "dns", "doom", "driver", "dynlink", "echo", "enabletx", "env", "export",
    "exec", "exit", "font1x", "font2x", "forcepack", "format",
    "fputest", "fsinfo", "gpu", "grep", "groupadd", "groups",
    "head", "help", "history", "hpet", "img", "inputinfo",
    "insmod", "install", "inttest", "ipv6", "kblayout", "keyboard",
    "kill", "login", "logout", "ls", "lsmod", "luade",
    "mac", "matrix", "memtest", "menu", "mkdir", "mount",
    "mv", "neofetch", "net", "netdebug", "netset", "netverbose",
    "open", "passwd", "perms", "ping", "pkg", "poweroff",
    "prof", "ps", "pwd", "qemunet", "reboot", "res",
    "rm", "rmmod", "rtldebug",     "saveperm", "saveusers", "scheddebug",
    "sendtest", "set", "setip", "settings", "sh", "shell", "shutdown", "smp",
    "spawn", "srv", "stopdhcp", "stopwatch", "storage", "su",
    "tapconfig", "task", "tcpclean", "tcpinfo", "tdesk", "terminal",
    "terminals", "testkey", "threadtest", "time", "timeraccuracy",
    "timertest", "touch", "tui", "tuidesktop", "udpverbose",     "unmount", "unset", "udp_test",
    "usb", "usbenum", "usbkeyboard", "usbpoll", "useradd", "userdel",
    "users", "version", "wc", "whoami", "xhci",
    "uptime", "execsym", "trace",
    (const char *)0
};

bool zeus_has_command(const char *name) {
    if (!name) return false;
    for (int i = 0; zeus_command_names[i]; i++) {
        if (kstrcmp(name, zeus_command_names[i]) == 0) return true;
    }
    return false;
}

static bool zeus_name_has_prefix(const char *name, const char *prefix,
                                 uint32_t plen) {
    for (uint32_t i = 0; i < plen; i++) {
        if (name[i] == '\0' || name[i] != prefix[i]) return false;
    }
    return true;
}

/* Tab completion: unique match -> append suffix (returns bytes written),
 * several matches -> returns -count, none -> 0. */
int zeus_tab_complete(const char *partial, uint32_t partial_len,
                      char *completion, uint32_t completion_max) {
    if (!partial || !completion || partial_len == 0 || completion_max < 2)
        return 0;

    const char *match = NULL;
    int match_count = 0;

    for (int i = 0; zeus_command_names[i]; i++) {
        if (zeus_name_has_prefix(zeus_command_names[i], partial, partial_len)) {
            if (match_count == 0) match = zeus_command_names[i];
            match_count++;
        }
    }

    if (match_count == 1 && match) {
        uint32_t mlen = (uint32_t)strlen(match);
        if (mlen <= partial_len) {
            completion[0] = ' ';
            completion[1] = '\0';
            return 1;
        }
        uint32_t suffix = mlen - partial_len;
        if (suffix + 2 > completion_max) return 0;
        uint32_t j = 0;
        for (uint32_t i = partial_len; i < mlen && j < completion_max - 2; i++, j++)
            completion[j] = match[i];
        completion[j++] = ' ';
        completion[j]   = '\0';
        return (int)j;
    }

    if (match_count > 1) {
        completion[0] = '\0';
        return -match_count;
    }
    return 0;
}

void zeus_print_command_matches(const char *partial, uint32_t partial_len) {
    if (!partial) return;
    printf("\n");
    for (int i = 0; zeus_command_names[i]; i++) {
        if (zeus_name_has_prefix(zeus_command_names[i], partial, partial_len))
            printf("  %s", zeus_command_names[i]);
    }
    printf("\n");
}

/* Special builtins that change Zeus state.  These always run in-kernel,
 * even if a file of the same name exists on the VFS. */
static bool zeus_is_special_builtin(const char *cmd) {
    return kstrcmp(cmd, "cd") == 0 || kstrcmp(cmd, "export") == 0 ||
           kstrcmp(cmd, "set") == 0 || kstrcmp(cmd, "unset") == 0 ||
           kstrcmp(cmd, "exit") == 0;
}

static bool zeus_is_fs_program(const char *cmd) {
    for (int i = 0; zeus_fs_programs[i]; i++) {
        if (kstrcmp(cmd, zeus_fs_programs[i]) == 0) return true;
    }
    return false;
}

static bool zeus_name_has_slash(const char *cmd) {
    if (!cmd) return false;
    for (; *cmd; cmd++)
        if (*cmd == '/') return true;
    return false;
}

static bool zeus_launch_userspace_sh(const char *explicit_path) {
    static const char *const defaults[] = {
        "/bin/sh.exe", "/bin/sh", "/sh.exe", "/sh",
        "/apps/sh.exe", "/apps/sh",
        "/os/apps/sh.exe", "/os/apps/sh", (const char *)0
    };
    if (!vfs_has_root_mount()) {
        printf("No VFS mount. Mount a filesystem first.\n");
        return false;
    }
    if (explicit_path && explicit_path[0]) {
        if (vfs_exists(explicit_path)) {
            printf("Starting userspace shell: %s\n", explicit_path);
            exec_file(explicit_path, NULL);
            return true;
        }
        printf("Shell binary '%s' not found.\n", explicit_path);
        return false;
    }
    for (int i = 0; defaults[i]; i++) {
        if (!vfs_exists(defaults[i])) continue;
        printf("Starting userspace shell: %s\n", defaults[i]);
        exec_file(defaults[i], NULL);
        return true;
    }
    printf("Userspace sh not installed (tried /sh.exe, /apps/sh.exe).\n");
    printf("Build it with: bash apps/build_apps.sh sh\n");
    return false;
}

static int zeus_no_external;
void zeus_set_no_external(int on) { zeus_no_external = on ? 1 : 0; }

void zeus_execute_command(ZeusShell* shell, int argc, char* argv[]) {
    const char* command = argv[0];
    shell->last_status = 0;

    /* Disk programs win. Skip the FAT walk for known kernel-only names
     * (netset, smp, …) — those lookups were stalling the boot log. */
    if (!zeus_no_external && !zeus_is_special_builtin(command) &&
        (zeus_is_fs_program(command) || zeus_name_has_slash(command) ||
         !zeus_has_command(command))) {
        uint64_t t_check0 = ktime_now_us();
        if (zeus_try_run_executable(shell, argc, argv)) {
            uint64_t t_done = ktime_now_us();
            zeus_time_check_us = 0;
            zeus_time_exec_us  = t_done - t_check0;
            return;
        }
    }

    // Built-in commands
    if (kstrcmp(command, "export") == 0 || kstrcmp(command, "set") == 0) {
        if (argc < 2) {
            for (int i = 0; i < zeus_env_n; i++)
                printf("%s=%s\n", zeus_env_k[i], zeus_env_v[i]);
        } else {
            char *eq = NULL;
            for (char *p = argv[1]; *p; p++) {
                if (*p == '=') { eq = p; break; }
            }
            if (eq) {
                *eq = '\0';
                if (zeus_env_set(argv[1], eq + 1) != 0)
                    shell->last_status = 1;
            } else if (argc >= 3) {
                if (zeus_env_set(argv[1], argv[2]) != 0)
                    shell->last_status = 1;
            } else {
                printf("Usage: export NAME=value\n");
                shell->last_status = 1;
            }
        }
    } else if (kstrcmp(command, "unset") == 0) {
        if (argc < 2) { printf("Usage: unset NAME\n"); shell->last_status = 1; }
        else zeus_env_unset(argv[1]);
    } else if (kstrcmp(command, "env") == 0) {
        for (int i = 0; i < zeus_env_n; i++)
            printf("%s=%s\n", zeus_env_k[i], zeus_env_v[i]);
        printf("?=%d\n", shell->last_status);
        printf("PWD=%s\n", shell->current_dir);
    } else if (kstrcmp(command, "cd") == 0) {
        zeus_cmd_cd(shell, argc, argv);
    } else if (kstrcmp(command, "pwd") == 0) {
        zeus_cmd_pwd(shell, argc, argv);
    } else if (kstrcmp(command, "ls") == 0) {
        zeus_cmd_ls(shell, argc, argv);
    } else if (kstrcmp(command, "cat") == 0) {
        zeus_cmd_cat(shell, argc, argv);
    } else if (kstrcmp(command, "echo") == 0) {
        zeus_cmd_echo(shell, argc, argv);
    } else if (kstrcmp(command, "touch") == 0) {
        zeus_cmd_touch(shell, argc, argv);
    } else if (kstrcmp(command, "mkdir") == 0) {
        zeus_cmd_mkdir(shell, argc, argv);
    } else if (kstrcmp(command, "cmd") == 0) {
        zeus_cmd_system(shell, argc, argv);
    } else if (kstrcmp(command, "clear") == 0) {
        zeus_cmd_clear(shell, argc, argv);
    } else if (kstrcmp(command, "help") == 0) {
        zeus_cmd_help(shell, argc, argv);
    } else if (kstrcmp(command, "history") == 0) {
        zeus_cmd_history(shell, argc, argv);
    } else if (kstrcmp(command, "version") == 0) {
        zeus_cmd_version(shell, argc, argv);
    } else if (kstrcmp(command, "time") == 0) {
        zeus_cmd_time(shell, argc, argv);
    } else if (kstrcmp(command, "date") == 0) {
        rtc_print_time();
    } else if (kstrcmp(command, "uptime") == 0) {
        zeus_cmd_uptime();
    } else if (kstrcmp(command, "cachestats") == 0) {
        storage_cache_stats();
    } else if (kstrcmp(command, "grep") == 0) {
        zeus_cmd_grep(shell, argc, argv);
    } else if (kstrcmp(command, "wc") == 0) {
        zeus_cmd_wc(shell, argc, argv);
    } else if (kstrcmp(command, "head") == 0) {
        zeus_cmd_head(shell, argc, argv);
    } else if (kstrcmp(command, "curl") == 0) {
        zeus_cmd_curl(shell, argc, argv);
    } else if (kstrcmp(command, "exit") == 0) {
        printf("Exiting Zeus Shell...\n");
        exit(0);
    } else if (kstrcmp(command, "shutdown") == 0 || kstrcmp(command, "poweroff") == 0) {
        extern void acpi_shutdown(void);
        printf("System is going down for poweroff NOW!\n");
        acpi_shutdown();
    } else if (kstrcmp(command, "reboot") == 0) {
        extern void acpi_reboot(void);
        printf("System is going down for reboot NOW!\n");
        acpi_reboot();
    } else if (kstrcmp(command, "neofetch") == 0) {
        extern void print_info(void);
        print_info();
    } else if (kstrcmp(command, "storage") == 0) {
        extern void show_storage_devices(void);
        show_storage_devices();
    } else if (kstrcmp(command, "mount") == 0) {
        extern void mount_filesystem_device(uint8_t device_id);
        extern void mount_filesystem_ext(uint8_t, uint32_t, const char*, const char*);
        if (argc < 2) {
            printf("Usage: mount <device_id> [partition_lba] [fat32|ext4] [path]\n");
            printf("Examples:\n");
            printf("  mount 0              - mount device 0 as FAT32 at /\n");
            printf("  mount 1 2048 ext4 /mnt - mount device 1, LBA 2048, ext4 at /mnt\n");
        } else {
            uint8_t device_id = (uint8_t)atoi(argv[1]);
            uint32_t lba = (argc >= 3) ? (uint32_t)atoi(argv[2]) : 0;
            const char *fstype = (argc >= 4) ? argv[3] : "fat32";
            const char *mpath  = (argc >= 5) ? argv[4] : "/";
            if (lba == 0 && kstrcmp(fstype, "fat32") == 0 && kstrcmp(mpath, "/") == 0) {
                mount_filesystem_device(device_id);
            } else {
                mount_filesystem_ext(device_id, lba, fstype, mpath);
            }
        }
    } else if (kstrcmp(command, "unmount") == 0) {
        extern void unmount_filesystem(void);
        extern void unmount_filesystem_path(const char *);
        if (argc >= 2) {
            unmount_filesystem_path(argv[1]);
        } else {
            unmount_filesystem();
        }
    } else if (kstrcmp(command, "format") == 0) {
        if (argc < 2) {
            printf("Usage: format <device_id> [label]\n");
        } else {
            uint8_t device_id = (uint8_t)atoi(argv[1]);
            const char *label = (argc >= 3) ? argv[2] : "INDEVOS";
            
            printf("WARNING: This will erase all data on device %d.\n", device_id);
            // In a real shell we might ask for confirmation, but here we proceed
            printf("Formatting device %d with label '%s'...\n", device_id, label);
            
            if (fat32_format(device_id, label) == 0) {
                printf("Format successful.\n");
            } else {
                printf("Format failed.\n");
            }
        }
    } else if (kstrcmp(command, "install") == 0) {
        extern void cmd_install(int argc, char **argv);
        cmd_install(argc, argv);
    } else if (kstrcmp(command, "fsinfo") == 0) {
        extern void filesystem_info(void);
        filesystem_info();
    } else if (kstrcmp(command, "rm") == 0) {
        extern void remove_file(const char *filename);
        if (argc < 2) {
            printf("Usage: rm <filename>\n");
        } else {
            remove_file(argv[1]);
        }
    } else if (kstrcmp(command, "cp") == 0) {
        extern void copy_file(const char *source, const char *dest);
        if (argc < 3) {
            printf("Usage: cp <source> <destination>\n");
        } else {
            copy_file(argv[1], argv[2]);
        }
    } else if (kstrcmp(command, "mv") == 0) {
        extern void move_file(const char *source, const char *dest);
        if (argc < 3) {
            printf("Usage: mv <source> <destination>\n");
        } else {
            move_file(argv[1], argv[2]);
        }
    } else if (kstrcmp(command, "trace") == 0) {
        /* Hot-path tracing: program loads and filesystem operations.  Off by
         * default because each message is a console write (15-40 ms here). */
        if (argc >= 2) {
            if (kstrcmp(argv[1], "on") == 0)        ktrace_set(true);
            else if (kstrcmp(argv[1], "off") == 0)  ktrace_set(false);
        }
        printf("trace: program-load and filesystem tracing is %s\n",
               ktrace_enabled ? "on" : "off");
    } else if (kstrcmp(command, "execsym") == 0) {
        /* ELF symtab loading for user-mode backtraces (off by default: it costs
         * time on every program launch). */
        if (argc >= 2 && (kstrcmp(argv[1], "on") == 0 || kstrcmp(argv[1], "off") == 0)) {
            exec_user_symbols_enabled = (kstrcmp(argv[1], "on") == 0);
        }
        printf("execsym: ELF symbol loading is %s\n",
               exec_user_symbols_enabled ? "on" : "off");
        printf("  (on = user-mode faults are symbolised; costs ~40 ms per launch)\n");
    } else if (kstrcmp(command, "exec") == 0) {
        if (zeus_no_external) {
            printf("exec: use a full-screen tty (Ctrl+F2), not this window\n");
            shell->last_status = 126;
        } else if (argc < 2) {
            printf("Usage: exec <filename>\n");
        } else {
            exec_file(argv[1], &argv[1]);
        }
    } else if (kstrcmp(command, "shell") == 0 || kstrcmp(command, "sh") == 0) {
        if (zeus_no_external) {
            printf("This window is already a shell. Ctrl+F2 opens a tty.\n");
            shell->last_status = 126;
        } else if (!zeus_launch_userspace_sh((argc >= 2) ? argv[1] : NULL))
            shell->last_status = 127;
    } else if (kstrcmp(command, "net") == 0) {
        extern void show_network_status(void);
        show_network_status();
    } else if (kstrcmp(command, "ping") == 0) {
        extern int icmp_ping(const uint8_t *dst_ip, const uint8_t *data, uint16_t data_len);
        extern int icmp_check_echo_reply(void);
        extern int net_is_initialized(void);
        extern int net_receive_packet(uint8_t *buffer, uint32_t max_length);
        extern void ethernet_process_frame(const uint8_t *frame, uint32_t length);
        extern uint64_t hpet_get_ticks(void);
        extern int parse_ip_address(const char *ip_str, uint8_t *ip_out);
        extern int dns_resolve_a(const char *hostname, uint8_t out_ip[4]);
        extern void net_wait_idle(void);

        const char *target = NULL;
        int count = 4;  /* default 4 pings */
        for (int ai = 1; ai < argc; ai++) {
            if (kstrcmp(argv[ai], "-c") == 0 && ai + 1 < argc)
                count = atoi(argv[++ai]);
            else if (!target)
                target = argv[ai];
        }

        if (!target) {
            printf("Usage: ping [-c count] <host>\n");
            printf("  host can be an IP (192.168.1.1) or hostname (google.com)\n");
        } else if (!net_is_initialized()) {
            printf("Network not initialized\n");
        } else {
            uint8_t dst_ip[4];
            if (count < 1) count = 1;
            if (count > 100) count = 100;

            /* Try parsing as IP first, then DNS resolve */
            if (parse_ip_address(target, dst_ip) != 0) {
                printf("PING %s: resolving hostname...\n", target);
                if (dns_resolve_a(target, dst_ip) != 0) {
                    printf("ping: cannot resolve '%s'\n", target);
                    goto ping_done;
                }
            }

            printf("PING %d.%d.%d.%d (%s): 20 data bytes\n",
                   dst_ip[0], dst_ip[1], dst_ip[2], dst_ip[3], target);

            int sent = 0, received = 0;
            uint64_t min_rtt = UINT64_MAX, max_rtt = 0, total_rtt = 0;

            for (int i = 0; i < count; i++) {
                const char *ping_data = "inDevOS ping probe!";
                uint64_t start = hpet_get_ticks();

                if (icmp_ping(dst_ip, (const uint8_t *)ping_data, 19) != 0) {
                    printf("ping: send failed\n");
                    continue;
                }
                sent++;

                /* Wait up to 3 seconds for reply, polling network */
                int got = 0;
                while (hpet_get_ticks() - start < 3000) {
                    /* Poll NIC for incoming packets */
                    uint8_t pkt_buf[2048];
                    int len = net_receive_packet(pkt_buf, sizeof(pkt_buf));
                    if (len > 0) ethernet_process_frame(pkt_buf, len);

                    if (icmp_check_echo_reply()) { got = 1; break; }
                    if (len <= 0) net_wait_idle();
                }

                uint64_t rtt = hpet_get_ticks() - start;

                if (got) {
                    received++;
                    if (rtt < min_rtt) min_rtt = rtt;
                    if (rtt > max_rtt) max_rtt = rtt;
                    total_rtt += rtt;
                    printf("Reply from %d.%d.%d.%d: seq=%d time=%llums\n",
                           dst_ip[0], dst_ip[1], dst_ip[2], dst_ip[3],
                           i + 1, (unsigned long long)rtt);
                } else {
                    printf("Request timeout for seq %d\n", i + 1);
                }

                /* Wait ~1 second between pings (unless last) */
                if (i < count - 1) {
                    uint64_t wait_start = hpet_get_ticks();
                    while (hpet_get_ticks() - wait_start < 1000) {
                        uint8_t pkt_buf[2048];
                        int len = net_receive_packet(pkt_buf, sizeof(pkt_buf));
                        if (len > 0) ethernet_process_frame(pkt_buf, len);
                        else net_wait_idle();
                    }
                }
            }

            /* Summary */
            printf("\n--- %s ping statistics ---\n", target);
            printf("%d packets transmitted, %d received, %d%% loss\n",
                   sent, received,
                   sent > 0 ? ((sent - received) * 100 / sent) : 100);
            if (received > 0) {
                printf("rtt min/avg/max = %llu/%llu/%llu ms\n",
                       (unsigned long long)min_rtt,
                       (unsigned long long)(total_rtt / received),
                       (unsigned long long)max_rtt);
            }
        }
        ping_done: (void)0;
    } else if (kstrcmp(command, "mac") == 0) {
        extern void show_mac_address(void);
        show_mac_address();
    } else if (kstrcmp(command, "usb") == 0) {
        extern void show_usb_status(void);
        show_usb_status();
    } else if (kstrcmp(command, "netset") == 0) {
        extern int cmd_netset(int argc, char **argv);
        if (cmd_netset(argc, argv) != 0) {
            printf("Usage: netset ip a.b.c.d [mask m.m.m.m] [gw g.g.g.g] [dns d.d.d.d]\n");
            printf("Example: netset ip 192.168.1.100 mask 255.255.255.0 gw 192.168.1.1\n");
        }
    } else if (kstrcmp(command, "ps") == 0) {
        extern void process_list_all(void);
        process_list_all();
    } else if (kstrcmp(command, "kill") == 0) {
        extern int process_terminate(uint32_t pid, int exit_code);
        if (argc < 2) {
            printf("Usage: kill <pid>\n");
        } else {
            uint32_t pid = (uint32_t)atoi(argv[1]);
            if (process_terminate(pid, 0) == 0) {
                printf("Process %d terminated\n", pid);
            } else {
                printf("Failed to terminate process %d\n", pid);
            }
        }
    } else if (kstrcmp(command, "dhcp") == 0) {
        extern void dhcp_start(void);
        printf("Starting DHCP client...\n");
        dhcp_start();
    } else if (kstrcmp(command, "ddhcp") == 0) {
        // QEMU default user networking (10.0.2.x)
        uint8_t ip[4]  = {10, 0, 2, 15};
        uint8_t gw[4]  = {10, 0, 2, 2};
        uint8_t mask[4] = {255, 255, 255, 0};
        uint8_t dns[4] = {10, 0, 2, 3};
        net_set_ip_address(ip); net_set_gateway(gw);
        net_set_subnet_mask(mask); net_set_dns_server(dns);
        printf("Manual net config set (QEMU User Net defaults):\n");
        printf("  IP: %d.%d.%d.%d  GW: %d.%d.%d.%d\n", ip[0],ip[1],ip[2],ip[3], gw[0],gw[1],gw[2],gw[3]);
        printf("  Mask: %d.%d.%d.%d  DNS: %d.%d.%d.%d\n", mask[0],mask[1],mask[2],mask[3], dns[0],dns[1],dns[2],dns[3]);
    } else if (kstrcmp(command, "tapconfig") == 0) {
        // TAP networking (host=192.168.56.1)
        uint8_t ip[4]  = {192, 168, 56, 2};
        uint8_t gw[4]  = {192, 168, 56, 1};
        uint8_t mask[4] = {255, 255, 255, 0};
        uint8_t dns[4] = {8, 8, 8, 8};
        net_set_ip_address(ip); net_set_gateway(gw);
        net_set_subnet_mask(mask); net_set_dns_server(dns);
        printf("Manual net config set (TAP Defaults):\n");
        printf("  IP: %d.%d.%d.%d  GW: %d.%d.%d.%d\n", ip[0],ip[1],ip[2],ip[3], gw[0],gw[1],gw[2],gw[3]);
        printf("  Mask: %d.%d.%d.%d  DNS: %d.%d.%d.%d\n", mask[0],mask[1],mask[2],mask[3], dns[0],dns[1],dns[2],dns[3]);
    } else if (kstrcmp(command, "qemunet") == 0) {
        // Alias for ddhcp
        uint8_t ip[4]  = {10, 0, 2, 15};
        uint8_t gw[4]  = {10, 0, 2, 2};
        uint8_t mask[4] = {255, 255, 255, 0};
        uint8_t dns[4] = {10, 0, 2, 3};
        net_set_ip_address(ip); net_set_gateway(gw);
        net_set_subnet_mask(mask); net_set_dns_server(dns);
        printf("QEMU user networking defaults applied:\n");
        printf("  IP: %d.%d.%d.%d  GW: %d.%d.%d.%d  DNS: %d.%d.%d.%d\n",
               ip[0],ip[1],ip[2],ip[3], gw[0],gw[1],gw[2],gw[3], dns[0],dns[1],dns[2],dns[3]);
    } else if (kstrcmp(command, "desktop") == 0) {
        extern void desktop_start(void);
        extern bool desktop_is_running(void);
        if (desktop_is_running()) {
            printf("Desktop is already running.\n");
        } else {
            printf("Starting desktop environment...\n");
            desktop_start();
        }
    } else if (kstrcmp(command, "spawn") == 0) {
        if (argc < 2) {
            printf("Usage: spawn <program>\n");
        } else {
            int pid = exec_spawn(argv[1], &argv[1]);
            if (pid > 0)
                printf("Spawned '%s' (PID %d)\n", argv[1], pid);
            else
                printf("spawn: failed to launch '%s'\n", argv[1]);
        }
    } else if (kstrcmp(command, "desktopstop") == 0) {
        extern void desktop_stop(void);
        extern bool desktop_is_running(void);
        if (desktop_is_running()) {
            desktop_stop();
            printf("Desktop stopped.\n");
        } else {
            printf("Desktop is not running.\n");
        }
    } else if (kstrcmp(command, "img") == 0) {
        extern void cmd_img(const char *filename);
        if (argc < 2) {
            printf("Usage: img <filename.png>\n");
        } else {
            cmd_img(argv[1]);
        }
    } else if (kstrcmp(command, "res") == 0) {
        if (argc < 3) {
            printf("Video: %ux%u  max %ux%u\n",
                   fb_get_width(), fb_get_height(),
                   fb_max_width(), fb_max_height());
            printf("Usage: res <width> <height>\n");
        } else {
            uint32_t rw = (uint32_t)atoi(argv[1]);
            uint32_t rh = (uint32_t)atoi(argv[2]);
            if (fb_set_video_resolution(rw, rh) == 0)
                printf("Resolution set to %ux%u\n", rw, rh);
            else
                printf("res: failed (out of range or GPU). Max %ux%u\n",
                       fb_max_width(), fb_max_height());
        }
    } else if (kstrcmp(command, "threadtest") == 0) {
        zeus_cmd_threadtest(shell, argc, argv);
    } else if (kstrcmp(command, "memtest") == 0) {
        zeus_cmd_memtest(shell, argc, argv);
    } else if (kstrcmp(command, "tcpinfo") == 0) {
        extern int tcp_get_connection_count(void);
        printf("TCP Connections: %d active\n", tcp_get_connection_count());
    } else if (kstrcmp(command, "tcpclean") == 0) {
        extern void tcp_cleanup_stale_connections(void);
        printf("Cleaning up stale TCP connections...\n");
        tcp_cleanup_stale_connections();
        printf("Done.\n");
    } else if (kstrcmp(command, "timertest") == 0) {
        extern void timer_test(void);
        timer_test();
    } else if (kstrcmp(command, "luade") == 0) {
#ifdef ENABLE_LUA
        extern bool luade_init(void);
        extern void luade_main_loop(void);
        extern void luade_shutdown(void);
        printf("Starting LuaDE - Lua Desktop Environment\n");
        printf("Initializing...\n");
        if (luade_init()) {
            luade_main_loop();
            luade_shutdown();
        } else {
            printf("Failed to initialize LuaDE\n");
        }
#else
        printf("Lua support not compiled in (ENABLE_LUA not set)\n");
#endif
    } else if (kstrcmp(command, "matrix") == 0) {
        /* /matrix.exe is preferred (see zeus_fs_programs); if it is not
         * installed there is nothing in the kernel to fall back to, since the
         * effect now lives entirely in the program. */
        printf("matrix: not installed (expected /matrix.exe)\n");
    } else if (kstrcmp(command, "gpu") == 0) {
        extern void gpu_test(void);
        gpu_test();
    } else if (kstrcmp(command, "task") == 0) {
        extern void network_thread_status(void);
        network_thread_status();
    } else if (kstrcmp(command, "driver") == 0) {
        // Driver commands - call cmd_driver directly
        extern void cmd_driver(const char *args);
        // Rebuild the args string from argv[1..argc-1]
        char args_buf[CMD_BUFFER_SIZE] = {0};
        int pos = 0;
        for (int i = 1; i < argc && pos < CMD_BUFFER_SIZE - 1; i++) {
            if (i > 1) args_buf[pos++] = ' ';
            int len = strlen(argv[i]);
            if (pos + len < CMD_BUFFER_SIZE - 1) {
                kstrncpy(args_buf + pos, argv[i], len);
                pos += len;
            }
        }
        cmd_driver(pos > 0 ? args_buf : NULL);
    } else if (kstrcmp(command, "audio") == 0) {
        /* Audio commands (AC97 + mixer) */
        extern bool ac97_is_available(void);
        extern void ac97_set_volume(uint8_t volume);
        extern void ac97_test_tone(uint32_t frequency, uint32_t duration_ms);
        extern void ac97_stop(void);
        extern void ac97_get_status(char *buf, size_t buflen);
        extern bool mixer_is_available(void);
        extern void mixer_get_status(char *buf, size_t buflen);
        extern int  mixer_play_tone(uint32_t frequency, uint32_t duration_ms, uint8_t volume);
        extern void mixer_stop_channel(int channel);
        extern void mixer_stop_all(void);
        extern void mixer_set_master_volume(uint8_t volume);
        extern void mixer_set_channel_volume(int channel, uint8_t volume);
        extern void mixer_set_channel_pan(int channel, int8_t pan);
        if (argc < 2) {
            char buf[128];
            ac97_get_status(buf, sizeof(buf));
            printf("%s\n", buf);
            if (mixer_is_available()) {
                mixer_get_status(buf, sizeof(buf));
                printf("%s\n", buf);
            }
            printf("\nUsage: audio test [freq] [ms]    - Play test tone (AC97 direct)\n");
            printf("       audio mix [freq] [ms]     - Play tone via mixer channel\n");
            printf("       audio chord                - Play a chord (3 tones mixed)\n");
            printf("       audio volume <0-100>      - Set AC97 volume\n");
            printf("       audio master <0-255>      - Set mixer master volume\n");
            printf("       audio chvol <ch> <0-255>  - Set mixer channel volume\n");
            printf("       audio pan <ch> <-128..127> - Set channel panning\n");
            printf("       audio stop [ch]           - Stop playback (or specific channel)\n");
        } else if (kstrcmp(argv[1], "test") == 0) {
            uint32_t freq = (argc >= 3) ? (uint32_t)atoi(argv[2]) : 440;
            uint32_t ms   = (argc >= 4) ? (uint32_t)atoi(argv[3]) : 1000;
            ac97_test_tone(freq, ms);
        } else if (kstrcmp(argv[1], "mix") == 0) {
            uint32_t freq = (argc >= 3) ? (uint32_t)atoi(argv[2]) : 440;
            uint32_t ms   = (argc >= 4) ? (uint32_t)atoi(argv[3]) : 1000;
            int ch = mixer_play_tone(freq, ms, 200);
            if (ch >= 0) printf("Playing %dHz on mixer channel %d\n", freq, ch);
            else printf("Failed to play tone\n");
        } else if (kstrcmp(argv[1], "chord") == 0) {
            /* Play C major chord: C4=262Hz, E4=330Hz, G4=392Hz */
            int c1 = mixer_play_tone(262, 2000, 180);
            int c2 = mixer_play_tone(330, 2000, 180);
            int c3 = mixer_play_tone(392, 2000, 180);
            printf("Playing C major chord on channels %d, %d, %d\n", c1, c2, c3);
        } else if (kstrcmp(argv[1], "volume") == 0) {
            if (argc >= 3) {
                ac97_set_volume((uint8_t)atoi(argv[2]));
            } else {
                printf("Usage: audio volume <0-100>\n");
            }
        } else if (kstrcmp(argv[1], "master") == 0) {
            if (argc >= 3) {
                mixer_set_master_volume((uint8_t)atoi(argv[2]));
                printf("Mixer master volume set to %d\n", atoi(argv[2]));
            } else {
                printf("Usage: audio master <0-255>\n");
            }
        } else if (kstrcmp(argv[1], "chvol") == 0) {
            if (argc >= 4) {
                mixer_set_channel_volume(atoi(argv[2]), (uint8_t)atoi(argv[3]));
            } else {
                printf("Usage: audio chvol <channel> <0-255>\n");
            }
        } else if (kstrcmp(argv[1], "pan") == 0) {
            if (argc >= 4) {
                mixer_set_channel_pan(atoi(argv[2]), (int8_t)atoi(argv[3]));
            } else {
                printf("Usage: audio pan <channel> <-128..127>\n");
            }
        } else if (kstrcmp(argv[1], "stop") == 0) {
            if (argc >= 3) {
                mixer_stop_channel(atoi(argv[2]));
                printf("Channel %d stopped\n", atoi(argv[2]));
            } else {
                mixer_stop_all();
                ac97_stop();
                printf("All audio stopped\n");
            }
        }
    } else if (kstrcmp(command, "settings") == 0) {
        extern int settings_init(void);
        extern void settings_print_all(void);
        extern int settings_set(const char *key, const char *value);
        extern void settings_apply(void);
        extern int settings_save(void);
        extern int settings_load(void);
        if (argc < 2) {
            settings_print_all();
        } else if (kstrcmp(argv[1], "set") == 0) {
            if (argc >= 4) {
                if (settings_set(argv[2], argv[3]) == 0) {
                    printf("Set %s = %s\n", argv[2], argv[3]);
                    settings_apply();
                } else {
                    printf("Unknown setting: %s\n", argv[2]);
                }
            } else {
                printf("Usage: settings set <key> <value>\n");
                printf("Keys: display.font_scale, display.fg_color, audio.volume,\n");
                printf("      network.ip, network.dhcp, input.layout, system.hostname\n");
            }
        } else if (kstrcmp(argv[1], "save") == 0) {
            if (settings_save() == 0) printf("Settings saved\n");
            else printf("Failed to save settings\n");
        } else if (kstrcmp(argv[1], "load") == 0) {
            if (settings_load() == 0) { settings_apply(); printf("Settings loaded\n"); }
            else printf("Failed to load settings\n");
        } else if (kstrcmp(argv[1], "reset") == 0) {
            settings_init();
            settings_apply();
            printf("Settings reset to defaults\n");
        }
    } else if (kstrcmp(command, "clip") == 0 || kstrcmp(command, "clipboard") == 0) {
        extern int clipboard_set_text(const char *text);
        extern int clipboard_get_text(char *buf, size_t buflen);
        extern void clipboard_clear(void);
        extern bool clipboard_has_data(void);
        extern size_t clipboard_get_size(void);
        if (argc < 2) {
            if (clipboard_has_data()) {
                char buf[256];
                int n = clipboard_get_text(buf, sizeof(buf));
                if (n >= 0) {
                    printf("Clipboard (%d bytes): %s\n", (int)clipboard_get_size(), buf);
                } else {
                    printf("Clipboard contains binary data (%d bytes)\n", (int)clipboard_get_size());
                }
            } else {
                printf("Clipboard is empty\n");
            }
            printf("Usage: clip copy <text>  - Copy text to clipboard\n");
            printf("       clip paste        - Paste clipboard contents\n");
            printf("       clip clear        - Clear clipboard\n");
        } else if (kstrcmp(argv[1], "copy") == 0) {
            if (argc >= 3) {
                /* Join all remaining args */
                char text[512] = {0};
                int pos = 0;
                for (int i = 2; i < argc && pos < 510; i++) {
                    if (i > 2) text[pos++] = ' ';
                    int len = strlen(argv[i]);
                    if (pos + len > 510) len = 510 - pos;
                    memcpy(text + pos, argv[i], len);
                    pos += len;
                }
                clipboard_set_text(text);
                printf("Copied to clipboard\n");
            } else {
                printf("Usage: clip copy <text>\n");
            }
        } else if (kstrcmp(argv[1], "paste") == 0) {
            char buf[512];
            int n = clipboard_get_text(buf, sizeof(buf));
            if (n >= 0) {
                printf("%s\n", buf);
            } else {
                printf("Clipboard is empty or contains non-text data\n");
            }
        } else if (kstrcmp(argv[1], "clear") == 0) {
            clipboard_clear();
            printf("Clipboard cleared\n");
        }
    } else if (kstrcmp(command, "whoami") == 0) {
        extern const char *user_current_name(void);
        extern uint32_t user_current_uid(void);
        extern uint32_t user_current_gid(void);
        printf("%s (uid=%u gid=%u)\n", user_current_name(), user_current_uid(), user_current_gid());
    } else if (kstrcmp(command, "login") == 0) {
        extern int user_login(const char *username, const char *password);
        if (argc >= 3) {
            user_login(argv[1], argv[2]);
        } else {
            printf("Usage: login <username> <password>\n");
        }
    } else if (kstrcmp(command, "logout") == 0) {
        extern void user_logout(void);
        user_logout();
    } else if (kstrcmp(command, "su") == 0) {
        extern int user_login(const char *username, const char *password);
        extern bool user_is_root(void);
        if (argc < 2) {
            printf("Usage: su <username> [password]\n");
        } else if (user_is_root()) {
            /* Root can switch without password */
            extern user_t *user_find_by_name(const char *name);
            /* Just set current uid/gid directly if root */
            extern uint32_t user_current_uid(void);
            /* Use a direct extern to the internal state */
            printf("Switched to %s\n", argv[1]);
            user_login(argv[1], "");  /* root override not implemented, require password */
        } else if (argc >= 3) {
            user_login(argv[1], argv[2]);
        } else {
            printf("Password required for non-root user\n");
        }
    } else if (kstrcmp(command, "useradd") == 0) {
        extern int user_add(const char *, const char *, uint32_t, uint32_t, const char *, const char *);
        extern bool user_is_root(void);
        if (!user_is_root()) {
            printf("useradd: permission denied (must be root)\n");
        } else if (argc >= 3) {
            /* useradd <username> <password> [uid] [gid] */
            uint32_t uid = 0, gid = 1;
            if (argc >= 4) uid = (uint32_t)atoi(argv[3]);
            else {
                /* Auto-assign uid */
                extern user_t *user_find_by_uid(uint32_t);
                for (uid = 1000; uid < 65000; uid++) {
                    if (!user_find_by_uid(uid)) break;
                }
            }
            if (argc >= 5) gid = (uint32_t)atoi(argv[4]);
            char home[128];
            snprintf(home, sizeof(home), "/HOME/%s", argv[1]);
            user_add(argv[1], argv[2], uid, gid, home, "zeus");
            printf("User '%s' created (uid=%u gid=%u)\n", argv[1], uid, gid);
        } else {
            printf("Usage: useradd <username> <password> [uid] [gid]\n");
        }
    } else if (kstrcmp(command, "userdel") == 0) {
        extern int user_remove(const char *);
        extern bool user_is_root(void);
        if (!user_is_root()) {
            printf("userdel: permission denied\n");
        } else if (argc >= 2) {
            user_remove(argv[1]);
        } else {
            printf("Usage: userdel <username>\n");
        }
    } else if (kstrcmp(command, "passwd") == 0) {
        extern int user_set_password(const char *, const char *);
        extern bool user_is_root(void);
        extern const char *user_current_name(void);
        if (argc >= 3 && user_is_root()) {
            user_set_password(argv[1], argv[2]);
            printf("Password updated for %s\n", argv[1]);
        } else if (argc >= 2) {
            /* Change own password */
            user_set_password(user_current_name(), argv[1]);
            printf("Password updated\n");
        } else {
            printf("Usage: passwd [username] <new_password>\n");
        }
    } else if (kstrcmp(command, "users") == 0) {
        extern int user_list(void);
        user_list();
    } else if (kstrcmp(command, "groups") == 0) {
        extern int group_list(void);
        group_list();
    } else if (kstrcmp(command, "groupadd") == 0) {
        extern int group_add(const char *, uint32_t);
        extern bool user_is_root(void);
        if (!user_is_root()) {
            printf("groupadd: permission denied\n");
        } else if (argc >= 2) {
            uint32_t gid = 0;
            if (argc >= 3) gid = (uint32_t)atoi(argv[2]);
            else {
                extern group_t *group_find_by_gid(uint32_t);
                for (gid = 100; gid < 65000; gid++) {
                    if (!group_find_by_gid(gid)) break;
                }
            }
            group_add(argv[1], gid);
            printf("Group '%s' created (gid=%u)\n", argv[1], gid);
        } else {
            printf("Usage: groupadd <name> [gid]\n");
        }
    } else if (kstrcmp(command, "chmod") == 0) {
        extern int perm_chmod(const char *, uint16_t);
        if (argc >= 3) {
            uint16_t mode = (uint16_t)strtol_octal(argv[1]);
            perm_chmod(argv[2], mode);
            printf("Mode of '%s' set to %03o\n", argv[2], mode);
        } else {
            printf("Usage: chmod <mode> <path>  (mode is octal, e.g. 755)\n");
        }
    } else if (kstrcmp(command, "chown") == 0) {
        extern int perm_chown(const char *, uint32_t, uint32_t);
        extern user_t *user_find_by_name(const char *);
        if (argc >= 3) {
            /* chown <user[:group]> <path> */
            uint32_t uid = 0, gid = 0;
            /* Parse user:group */
            char uname[32] = {0};
            char gname[32] = {0};
            const char *colon = NULL;
            for (const char *p = argv[1]; *p; p++) {
                if (*p == ':') { colon = p; break; }
            }
            if (colon) {
                size_t ulen = colon - argv[1];
                if (ulen >= sizeof(uname)) ulen = sizeof(uname) - 1;
                memcpy(uname, argv[1], ulen);
                strncpy(gname, colon + 1, sizeof(gname) - 1);
            } else {
                strncpy(uname, argv[1], sizeof(uname) - 1);
            }
            extern user_t *user_find_by_name(const char *);
            extern group_t *group_find_by_name(const char *);
            user_t *u = user_find_by_name(uname);
            if (u) uid = u->uid;
            else uid = (uint32_t)atoi(uname);
            if (gname[0]) {
                group_t *g = group_find_by_name(gname);
                if (g) gid = g->gid;
                else gid = (uint32_t)atoi(gname);
            }
            perm_chown(argv[2], uid, gid);
        } else {
            printf("Usage: chown <user[:group]> <path>\n");
        }
    } else if (kstrcmp(command, "perms") == 0) {
        extern void perm_list(const char *);
        perm_list(argc >= 2 ? argv[1] : "/");
    } else if (kstrcmp(command, "saveperm") == 0 || kstrcmp(command, "saveusers") == 0) {
        extern int users_save(void);
        extern int perms_save(void);
        users_save();
        perms_save();
        printf("Users and permissions saved\n");
    } else if (kstrcmp(command, "pkg") == 0) {
        extern void pkg_list(void);
        extern void pkg_info(const char *);
        extern int  pkg_install(const char *);
        extern int  pkg_uninstall(const char *);
        extern void pkg_search(const char *);
        extern int  pkg_create(const char *, const char *, const char *, const char *, const char *);
        if (argc < 2) {
            printf("inDevOS Package Manager\n");
            printf("Usage:\n");
            printf("  pkg list              - List installed packages\n");
            printf("  pkg install <file>    - Install from .ipkg file\n");
            printf("  pkg uninstall <name>  - Uninstall a package\n");
            printf("  pkg info <name>       - Show package details\n");
            printf("  pkg search <keyword>  - Search packages\n");
            printf("  pkg create <name> <ver> <desc> <dir> <out> - Create .ipkg\n");
        } else if (kstrcmp(argv[1], "list") == 0 || kstrcmp(argv[1], "ls") == 0) {
            pkg_list();
        } else if (kstrcmp(argv[1], "install") == 0 || kstrcmp(argv[1], "i") == 0) {
            if (argc >= 3) pkg_install(argv[2]);
            else printf("Usage: pkg install <file.ipkg>\n");
        } else if (kstrcmp(argv[1], "uninstall") == 0 || kstrcmp(argv[1], "remove") == 0) {
            if (argc >= 3) pkg_uninstall(argv[2]);
            else printf("Usage: pkg uninstall <name>\n");
        } else if (kstrcmp(argv[1], "info") == 0) {
            if (argc >= 3) pkg_info(argv[2]);
            else printf("Usage: pkg info <name>\n");
        } else if (kstrcmp(argv[1], "search") == 0 || kstrcmp(argv[1], "find") == 0) {
            if (argc >= 3) pkg_search(argv[2]);
            else printf("Usage: pkg search <keyword>\n");
        } else if (kstrcmp(argv[1], "create") == 0) {
            if (argc >= 7) {
                pkg_create(argv[2], argv[3], argv[4], argv[5], argv[6]);
            } else {
                printf("Usage: pkg create <name> <version> <description> <source_dir> <output.ipkg>\n");
            }
        } else {
            printf("Unknown subcommand: %s\n", argv[1]);
        }
    } else if (kstrcmp(command, "dlopen") == 0 || kstrcmp(command, "dynlink") == 0) {
        extern void dynlink_list(void);
        extern int  dynlink_load(const char *path);
        extern int  dynlink_unload(int handle);
        extern uint64_t dynlink_resolve(const char *name);
        if (argc < 2) {
            dynlink_list();
        } else if (kstrcmp(argv[1], "load") == 0 && argc >= 3) {
            int h = dynlink_load(argv[2]);
            if (h >= 0) printf("Loaded '%s' as handle %d\n", argv[2], h);
        } else if (kstrcmp(argv[1], "unload") == 0 && argc >= 3) {
            dynlink_unload(atoi(argv[2]));
        } else if (kstrcmp(argv[1], "resolve") == 0 && argc >= 3) {
            uint64_t addr = dynlink_resolve(argv[2]);
            if (addr) printf("%s = 0x%llx\n", argv[2], (unsigned long long)addr);
            else      printf("Symbol '%s' not found\n", argv[2]);
        } else if (kstrcmp(argv[1], "list") == 0) {
            dynlink_list();
        } else {
            printf("Usage: dynlink [list|load <path>|unload <handle>|resolve <symbol>]\n");
        }
    } else if (kstrcmp(command, "ipv6") == 0) {
        extern int ipv6_init(void);
        extern void ipv6_print_status(void);
        extern void ipv6_start_slaac(void);
        extern bool ipv6_is_enabled(void);
        extern int ipv6_ping(const void *dst);
        extern int ipv6_addr_from_string(const char *str, void *addr);
        if (argc < 2) {
            if (ipv6_is_enabled()) {
                ipv6_print_status();
            } else {
                printf("IPv6 is not enabled. Use 'ipv6 enable' to start.\n");
            }
            printf("\nUsage: ipv6 enable   - Enable IPv6 with SLAAC\n");
            printf("       ipv6 status   - Show IPv6 status\n");
            printf("       ipv6 slaac    - Send Router Solicitation\n");
            printf("       ipv6 ping <addr> - Ping an IPv6 address\n");
        } else if (kstrcmp(argv[1], "enable") == 0) {
            ipv6_init();
            ipv6_start_slaac();
            printf("IPv6 enabled, SLAAC started\n");
        } else if (kstrcmp(argv[1], "status") == 0) {
            ipv6_print_status();
        } else if (kstrcmp(argv[1], "slaac") == 0) {
            ipv6_start_slaac();
        } else if (kstrcmp(argv[1], "ping") == 0 || kstrcmp(argv[1], "ping6") == 0) {
            if (argc >= 3) {
                uint8_t addr[16];
                if (ipv6_addr_from_string(argv[2], addr) == 0) {
                    ipv6_ping((void *)addr);
                } else {
                    printf("Invalid IPv6 address: %s\n", argv[2]);
                }
            } else {
                printf("Usage: ipv6 ping <address>\n");
            }
        }
    } else if (kstrcmp(command, "tdesk") == 0 || kstrcmp(command, "tuidesktop") == 0) {
        extern void desktop_start(void);
        extern bool desktop_is_running(void);
        if (!desktop_is_running()) {
            printf("Starting desktop...\n");
            desktop_start();
        } else {
            printf("Desktop already running.\n");
        }

    } else if (kstrcmp(command, "menu") == 0 || kstrcmp(command, "tui") == 0) {
        /* TUI menu demo / system settings */
        extern tui_menu_t *tui_menu_create(const char *title, const char *subtitle);
        extern void tui_menu_destroy(tui_menu_t *menu);
        extern int  tui_menu_add(tui_menu_t *menu, const char *label, int type, int tag);
        extern int  tui_menu_add_submenu(tui_menu_t *menu, const char *label,
                                         tui_menu_t *sub, int tag);
        extern int  tui_menu_add_separator(tui_menu_t *menu);
        extern void tui_menu_set_help(tui_menu_t *menu, int idx, const char *h);
        extern void tui_menu_set_value(tui_menu_t *menu, int idx, int val);
        extern void tui_menu_set_group(tui_menu_t *menu, int idx, int grp);
        extern void tui_menu_set_theme(tui_menu_t *menu, int id);
        extern int  tui_menu_run(tui_menu_t *menu);
        extern int  tui_menu_get_selected_tag(tui_menu_t *menu);
        extern int  tui_menu_get_value(tui_menu_t *menu, int tag);
        extern void tui_message_box(const char *t, const char *m);
        extern int  tui_yes_no(const char *t, const char *q);

        /* Build a demo settings menu */
        tui_menu_t *net_sub = tui_menu_create("Network Settings",
            "Configure network options for inDevOS");
        tui_menu_add(net_sub, "Enable DHCP",       2, 20); /* checkbox */
        tui_menu_set_value(net_sub, 0, 1);
        tui_menu_set_help(net_sub, 0, "Automatically obtain IP address via DHCP");
        tui_menu_add(net_sub, "Enable IPv6",        2, 21);
        tui_menu_set_help(net_sub, 1, "Enable IPv6 protocol stack with SLAAC");
        tui_menu_add(net_sub, "Enable DNS cache",   2, 22);
        tui_menu_set_help(net_sub, 2, "Cache DNS lookups for faster resolution");

        tui_menu_t *display_sub = tui_menu_create("Display Settings",
            "Configure display and font options");
        tui_menu_add(display_sub, "Font scale: 1x",  3, 30); /* radio */
        tui_menu_set_group(display_sub, 0, 1);
        tui_menu_set_value(display_sub, 0, 1);
        tui_menu_add(display_sub, "Font scale: 2x",  3, 31);
        tui_menu_set_group(display_sub, 1, 1);
        tui_menu_add(display_sub, "Font scale: 3x",  3, 32);
        tui_menu_set_group(display_sub, 2, 1);
        tui_menu_add_separator(display_sub);
        tui_menu_add(display_sub, "Show cursor",      2, 33);
        tui_menu_set_value(display_sub, 4, 1);

        tui_menu_t *main_menu = tui_menu_create("inDevOS Configuration",
            "System configuration and settings");

        /* Set theme: argc >= 2 allows choosing theme */
        if (argc >= 2) {
            if (kstrcmp(argv[1], "dark") == 0)
                tui_menu_set_theme(main_menu, 2);
            else if (kstrcmp(argv[1], "green") == 0)
                tui_menu_set_theme(main_menu, 3);
            else if (kstrcmp(argv[1], "cyan") == 0)
                tui_menu_set_theme(main_menu, 1);
            /* Copy theme to submenus */
            if (kstrcmp(argv[1], "dark") == 0) {
                tui_menu_set_theme(net_sub, 2);
                tui_menu_set_theme(display_sub, 2);
            } else if (kstrcmp(argv[1], "green") == 0) {
                tui_menu_set_theme(net_sub, 3);
                tui_menu_set_theme(display_sub, 3);
            } else if (kstrcmp(argv[1], "cyan") == 0) {
                tui_menu_set_theme(net_sub, 1);
                tui_menu_set_theme(display_sub, 1);
            }
        }

        tui_menu_add_submenu(main_menu, "Network Settings",  net_sub,     1);
        tui_menu_set_help(main_menu, 0, "Configure network: DHCP, IPv6, DNS");
        tui_menu_add_submenu(main_menu, "Display Settings",  display_sub, 2);
        tui_menu_set_help(main_menu, 1, "Configure display: font scale, cursor");
        tui_menu_add_separator(main_menu);
        tui_menu_add(main_menu, "Enable serial logging",  2, 10);
        tui_menu_set_value(main_menu, 3, 1);
        tui_menu_set_help(main_menu, 3, "Log kernel messages to serial port");
        tui_menu_add(main_menu, "Enable audio",           2, 11);
        tui_menu_set_help(main_menu, 4, "Enable AC97 audio driver");
        tui_menu_add_separator(main_menu);
        tui_menu_add(main_menu, "System Information",      0, 100);
        tui_menu_set_help(main_menu, 6, "Show system information and status");
        tui_menu_add(main_menu, "About inDevOS",           0, 101);
        tui_menu_set_help(main_menu, 7, "Version and license information");

        int result = tui_menu_run(main_menu);
        int tag = tui_menu_get_selected_tag(main_menu);

        if (result == 0 && tag == 100) {
            printf("System info selected\n");
        } else if (result == 0 && tag == 101) {
            printf("inDevOS - a hobby operating system\n");
        }

        /* Clean up */
        tui_menu_destroy(display_sub);
        tui_menu_destroy(net_sub);
        tui_menu_destroy(main_menu);

    } else if (kstrcmp(command, "open") == 0) {
        /* File dialog - text mode */
        extern void file_dialog_init(void *dialog, int type, const char *title,
                                     const char *path, const char *filter);
        extern void file_dialog_show(void *dialog);
        extern void file_dialog_render_text(void *dialog);
        /* For now, just show the file listing */
        printf("File open dialog (text mode) - use 'ls' and 'cat' for now\n");
        printf("Graphical file dialog is unavailable in this build\n");
    } else if (kstrcmp(command, "timeraccuracy") == 0) {
        extern void timer_accuracy_test(void);
        timer_accuracy_test();
    } else if (kstrcmp(command, "stopwatch") == 0) {
        extern void stopwatch_test(void);
        stopwatch_test();
    } else if (kstrcmp(command, "dns") == 0) {
        if (argc < 2) {
            printf("Usage: dns <hostname>\n");
        } else {
            uint8_t ip[4];
            if (dns_resolve_a(argv[1], ip) == 0)
                printf("DNS: %s -> %d.%d.%d.%d\n", argv[1], ip[0], ip[1], ip[2], ip[3]);
            else
                printf("DNS: resolution failed for '%s'\n", argv[1]);
        }
    } else if (kstrcmp(command, "netverbose") == 0) {
        extern void ethernet_set_verbose(int enabled);
        extern int  ethernet_get_verbose(void);
        if (argc < 2) {
            printf("netverbose: %s\nUsage: netverbose on|off\n",
                   ethernet_get_verbose() ? "on" : "off");
        } else {
            if (kstrcmp(argv[1], "on") == 0) ethernet_set_verbose(1);
            else if (kstrcmp(argv[1], "off") == 0) ethernet_set_verbose(0);
            else printf("Usage: netverbose on|off\n");
            printf("netverbose: %s\n", ethernet_get_verbose() ? "on" : "off");
        }
    } else if (kstrcmp(command, "udpverbose") == 0) {
        extern void udp_set_verbose(int enabled);
        extern int  udp_get_verbose(void);
        if (argc < 2) {
            printf("udpverbose: %s\nUsage: udpverbose on|off\n",
                   udp_get_verbose() ? "on" : "off");
        } else {
            if (kstrcmp(argv[1], "on") == 0) udp_set_verbose(1);
            else if (kstrcmp(argv[1], "off") == 0) udp_set_verbose(0);
            else printf("Usage: udpverbose on|off\n");
            printf("udpverbose: %s\n", udp_get_verbose() ? "on" : "off");
        }
    } else if (kstrcmp(command, "stopdhcp") == 0) {
        extern void udp_unregister_handler(uint16_t port);
        printf("Stopping DHCP client...\n");
        udp_unregister_handler(68);
        printf("DHCP client stopped\n");
    } else if (kstrcmp(command, "setip") == 0) {
        extern int parse_ip_address(const char *ip_str, uint8_t *ip_out);
        if (argc < 2) {
            printf("Usage: setip <ip_address>  (e.g. setip 192.168.56.2)\n");
        } else {
            uint8_t ip[4];
            if (parse_ip_address(argv[1], ip) == 0) {
                net_set_ip_address(ip);
                printf("IP set to %d.%d.%d.%d\n", ip[0], ip[1], ip[2], ip[3]);
            } else {
                printf("Invalid IP address format\n");
            }
        }
    } else if (kstrcmp(command, "arp") == 0) {
        extern int   parse_ip_address(const char *ip_str, uint8_t *ip_out);
        extern bool  arp_resolve(const uint8_t *ip, uint8_t *mac);
        extern bool  arp_is_pending(const uint8_t *ip);
        extern int   net_has_pending_receive(void);
        extern int   net_receive_packet(uint8_t *buffer, uint32_t max_length);
        extern void  ethernet_process_frame(const uint8_t *frame, uint32_t length);
        if (argc < 2) {
            printf("Usage: arp <ip_address>\n");
        } else {
            uint8_t dst_ip[4];
            if (parse_ip_address(argv[1], dst_ip) != 0) {
                printf("Invalid IP address format\n");
            } else {
                uint8_t mac[6];
                printf("Resolving ARP for %s...\n", argv[1]);
                int attempts = 0;
                while (attempts < 10) {
                    if (arp_resolve(dst_ip, mac)) {
                        printf("Resolved: %02x:%02x:%02x:%02x:%02x:%02x\n",
                               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
                        goto arp_done;
                    } else if (!arp_is_pending(dst_ip)) {
                        printf("ARP resolution failed (no reply)\n");
                        goto arp_done;
                    }
                    printf("Waiting... (%d/10)\n", attempts + 1);
                    for (int i = 0; i < 100; i++) {
                        if (net_has_pending_receive()) {
                            uint8_t pkt[2048];
                            int n = net_receive_packet(pkt, sizeof(pkt));
                            if (n > 0) ethernet_process_frame(pkt, (uint32_t)n);
                        }
                        for (int j = 0; j < 10000; j++) __asm__ volatile("nop");
                    }
                    attempts++;
                }
                printf("ARP timeout after 10 attempts\n");
                arp_done: (void)0;
            }
        }
    } else if (kstrcmp(command, "arpstatus") == 0) {
        extern void arp_get_pending_info(int *total_pending);
        int total_pending;
        arp_get_pending_info(&total_pending);
        printf("ARP pending requests: %d\n", total_pending);
    } else if (kstrcmp(command, "arptest") == 0) {
        extern void arp_send_request(const uint8_t *ip);
        if (!net_is_initialized()) {
            printf("Network not initialized\n");
        } else {
            uint8_t test_ip[4] = {192, 168, 56, 1};
            printf("Sending ARP request to 192.168.56.1...\n");
            arp_send_request(test_ip);
            printf("ARP request sent\n");
        }
    } else if (kstrcmp(command, "addarp") == 0) {
        extern void arp_add_static(const uint8_t *ip, const uint8_t *mac);
        extern int  parse_ip_address(const char *ip_str, uint8_t *ip_out);
        if (argc < 3) {
            printf("Usage: addarp <ip> <mac>  (e.g. addarp 192.168.56.1 52:54:00:12:35:02)\n");
        } else if (!net_is_initialized()) {
            printf("Network not initialized\n");
        } else {
            uint8_t ip[4];
            if (parse_ip_address(argv[1], ip) != 0) {
                printf("Invalid IP address\n");
            } else if (strlen(argv[2]) != 17 ||
                       argv[2][2]!=':' || argv[2][5]!=':' || argv[2][8]!=':' ||
                       argv[2][11]!=':' || argv[2][14]!=':') {
                printf("Invalid MAC format. Use: xx:xx:xx:xx:xx:xx\n");
            } else {
                uint8_t mac[6];
                bool valid = true;
                for (int i = 0; i < 6 && valid; i++) {
                    int off = i * 3;
                    char h[3] = {argv[2][off], argv[2][off+1], 0};
                    uint8_t byte = 0;
                    for (int j = 0; j < 2; j++) {
                        char c = h[j];
                        if      (c >= '0' && c <= '9') byte = (byte<<4)|(c-'0');
                        else if (c >= 'a' && c <= 'f') byte = (byte<<4)|(c-'a'+10);
                        else if (c >= 'A' && c <= 'F') byte = (byte<<4)|(c-'A'+10);
                        else { valid = false; break; }
                    }
                    if (valid) mac[i] = byte;
                }
                if (valid) {
                    arp_add_static(ip, mac);
                    printf("Static ARP: %s -> %02x:%02x:%02x:%02x:%02x:%02x\n",
                           argv[1], mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
                } else {
                    printf("Invalid MAC address characters\n");
                }
            }
        }
    } else if (kstrcmp(command, "sendtest") == 0) {
        extern int net_send_packet(const uint8_t *data, uint32_t length);
        if (!net_is_initialized()) {
            printf("Network not initialized\n");
        } else {
            uint8_t pkt[64];
            memset(pkt, 0, sizeof(pkt));
            /* Broadcast destination */
            pkt[0]=pkt[1]=pkt[2]=pkt[3]=pkt[4]=pkt[5]=0xFF;
            uint8_t our_mac[6]; net_get_mac_address(our_mac);
            memcpy(&pkt[6], our_mac, 6);
            pkt[12]=0x08; pkt[13]=0x06; /* ARP EtherType */
            pkt[14]=0; pkt[15]=1;       /* HW: Ethernet */
            pkt[16]=0x08; pkt[17]=0;    /* Proto: IPv4 */
            pkt[18]=6; pkt[19]=4;       /* hw/proto lengths */
            pkt[20]=0; pkt[21]=1;       /* ARP request */
            memcpy(&pkt[22], our_mac, 6);
            uint8_t our_ip[4]; net_get_ip_address(our_ip);
            memcpy(&pkt[28], our_ip, 4);
            /* Target: 192.168.56.1 */
            pkt[38]=192; pkt[39]=168; pkt[40]=56; pkt[41]=1;
            int res = net_send_packet(pkt, sizeof(pkt));
            printf("Test packet %s\n", res == 0 ? "sent" : "FAILED");
        }
    } else if (kstrcmp(command, "forcepack") == 0) {
        extern int  net_has_pending_receive(void);
        extern int  net_receive_packet(uint8_t *buffer, uint32_t max_length);
        extern void ethernet_process_frame(const uint8_t *frame, uint32_t length);
        printf("Forcing network packet processing...\n");
        for (int i = 0; i < 20; i++) {
            if (net_has_pending_receive()) {
                printf("Found pending packet, processing...\n");
                uint8_t pkt[2048];
                int n = net_receive_packet(pkt, sizeof(pkt));
                if (n > 0) { printf("Processing %d bytes\n", n); ethernet_process_frame(pkt, (uint32_t)n); }
                else { printf("No packet despite pending flag\n"); break; }
            } else { printf("No pending packets\n"); break; }
        }
        printf("Done.\n");
    } else if (kstrcmp(command, "rtldebug") == 0) {
        extern rtl8139_device_t rtl8139_dev;
        if (!net_is_initialized()) {
            printf("Network not initialized\n");
        } else if (kstrcmp(net_get_driver_name(), "RTL8139") == 0) {
            printf("=== RTL8139 Debug ===\n");
            printf("  ISR:        0x%04x\n", rtl8139_read_reg16(&rtl8139_dev, 0x3E));
            printf("  IMR:        0x%04x\n", rtl8139_read_reg16(&rtl8139_dev, 0x3C));
            printf("  CBR:        0x%04x\n", rtl8139_read_reg16(&rtl8139_dev, 0x3A));
            printf("  CAPR:       0x%04x\n", rtl8139_read_reg16(&rtl8139_dev, 0x38));
            printf("  CMD:        0x%02x\n",  rtl8139_read_reg8(&rtl8139_dev, 0x37));
            printf("  Packet ptr: 0x%08x\n", rtl8139_dev.current_packet_ptr);
            printf("  RX pending: %d\n",      rtl8139_dev.rx_pending_flag);
            printf("Forcing interrupt check...\n");
            rtl8139_handle_interrupt();
        } else {
            printf("Not using RTL8139 (current: %s)\n", net_get_driver_name());
        }
    } else if (kstrcmp(command, "netdebug") == 0) {
        extern void hpet_print_status(void);
        extern void network_thread_status(void);
        extern int  net_has_pending_receive(void);
        extern int  net_receive_packet(uint8_t *buffer, uint32_t max_length);
        extern void ethernet_process_frame(const uint8_t *frame, uint32_t length);
        extern void rtl8139_handle_interrupt(void);
        printf("=== Network Debug Info ===\n");
        hpet_print_status();
        {   extern void show_network_status(void); show_network_status(); /* from cmd.c */ }
        network_thread_status();
        if (net_is_initialized()) {
            printf("\nPacket status:\n");
            printf("  Pending receive: %s\n", net_has_pending_receive() ? "yes" : "no");
            printf("  Driver: %s\n", net_get_driver_name());
            uint8_t pkt[2048];
            int n = net_receive_packet(pkt, sizeof(pkt));
            printf("  Manual receive: %d bytes\n", n);
            if (n > 0) {
                printf("  Processing packet...\n");
                ethernet_process_frame(pkt, (uint32_t)n);
            } else if (kstrcmp(net_get_driver_name(), "RTL8139") == 0) {
                printf("  Forcing RTL8139 interrupt check...\n");
                rtl8139_handle_interrupt();
            }
        }
    } else if (kstrcmp(command, "inttest") == 0) {
        extern e1000_device_t e1000_dev;
        if (!net_is_initialized()) {
            printf("Network not initialized\n");
        } else if (kstrcmp(net_get_driver_name(), "E1000") == 0) {
            printf("E1000 interrupt test:\n");
            printf("  IMS: 0x%08x\n", e1000_read_reg32(&e1000_dev, E1000_IMS));
            printf("  ICR: 0x%08x\n", e1000_read_reg32(&e1000_dev, E1000_ICR));
            uint32_t ims = E1000_ICR_LSC | E1000_ICR_RXT0 | E1000_ICR_RXDMT0;
            e1000_write_reg32(&e1000_dev, E1000_IMS, ims);
            printf("  IMS after enable: 0x%08x\n", e1000_read_reg32(&e1000_dev, E1000_IMS));
            printf("  CTRL: 0x%08x  STATUS: 0x%08x\n",
                   e1000_read_reg32(&e1000_dev, E1000_CTRL),
                   e1000_read_reg32(&e1000_dev, E1000_STATUS));
        } else {
            printf("inttest: E1000 only (current: %s)\n", net_get_driver_name());
        }
    } else if (kstrcmp(command, "checkpackets") == 0) {
        extern int net_receive_packet(uint8_t *buffer, uint32_t max_length);
        if (!net_is_initialized()) {
            printf("Network not initialized\n");
        } else {
            printf("Manually checking for packets...\n");
            for (int i = 0; i < 10; i++) {
                uint8_t pkt[2048];
                int n = net_receive_packet(pkt, sizeof(pkt));
                printf("  Attempt %2d: %d bytes\n", i + 1, n);
                for (int j = 0; j < 100000; j++) __asm__ volatile("nop");
            }
        }
    } else if (kstrcmp(command, "enabletx") == 0) {
        extern e1000_device_t e1000_dev;
        if (!net_is_initialized()) {
            printf("Network not initialized\n");
        } else if (kstrcmp(net_get_driver_name(), "E1000") == 0) {
            printf("Enabling E1000 transmit...\n");
            uint32_t tctl = e1000_read_reg32(&e1000_dev, E1000_TCTL);
            tctl |= E1000_TCTL_EN | E1000_TCTL_PSP;
            tctl |= (15 << 4) | (64 << 12);
            e1000_write_reg32(&e1000_dev, E1000_TCTL, tctl);
            tctl = e1000_read_reg32(&e1000_dev, E1000_TCTL);
            printf("TCTL: 0x%08x  TX %s\n", tctl,
                   (tctl & E1000_TCTL_EN) ? "enabled" : "FAILED");
        } else {
            printf("enabletx: E1000 only (current: %s)\n", net_get_driver_name());
        }
    } else if (kstrcmp(command, "keyboard") == 0) {
        printf("Keyboard Info:\n");
        printf("  PS/2: interrupt-driven, vector 33\n");
        printf("  USB:  polling-based, every 5ms\n");
        printf("  Both systems active simultaneously\n");
        printf("  Use 'kblayout' to change layout, 'testkey' to read PS/2 data\n");
    } else if (kstrcmp(command, "testkey") == 0) {
        extern uint8_t inb(uint16_t port);
        uint8_t status = inb(0x64);
        printf("PS/2 status: 0x%02x\n", status);
        if (status & 0x01) {
            uint8_t data = inb(0x60);
            printf("PS/2 data: 0x%02x\n", data);
        } else {
            printf("No PS/2 data available\n");
        }
    } else if (kstrcmp(command, "kblayout") == 0) {
        extern const char *keyboard_get_layout_name(void);
        extern void keyboard_set_layout(int layout);
        if (argc < 2) {
            printf("Current layout: %s\n", keyboard_get_layout_name());
            printf("Usage: kblayout <us|uk|de|fr>\n");
        } else if (kstrcmp(argv[1], "us") == 0) {
            keyboard_set_layout(0 /* KB_LAYOUT_US */);
            printf("Layout: %s\n", keyboard_get_layout_name());
        } else if (kstrcmp(argv[1], "uk") == 0) {
            keyboard_set_layout(1 /* KB_LAYOUT_UK */);
            printf("Layout: %s\n", keyboard_get_layout_name());
        } else if (kstrcmp(argv[1], "de") == 0) {
            keyboard_set_layout(2 /* KB_LAYOUT_DE */);
            printf("Layout: %s\n", keyboard_get_layout_name());
        } else if (kstrcmp(argv[1], "fr") == 0) {
            keyboard_set_layout(3 /* KB_LAYOUT_FR */);
            printf("Layout: %s\n", keyboard_get_layout_name());
        } else {
            printf("Unknown layout '%s'. Use: us, uk, de, fr\n", argv[1]);
        }
    } else if (kstrcmp(command, "usbenum") == 0) {
        extern void usb_enumerate_devices(void);
        usb_enumerate_devices();
    } else if (kstrcmp(command, "usbkeyboard") == 0) {
        extern void usb_keyboard_test(void);
        usb_keyboard_test();
    } else if (kstrcmp(command, "usbpoll") == 0) {
        extern void usb_keyboard_poll_test(void);
        usb_keyboard_poll_test();
    } else if (kstrcmp(command, "inputinfo") == 0) {
        extern void cmd_inputinfo(void);
        cmd_inputinfo();
    } else if (kstrcmp(command, "font1x") == 0) {
        fb_set_font_scale(1);
        printf("Font scale: 1x\n");
    } else if (kstrcmp(command, "font2x") == 0) {
        fb_set_font_scale(2);
        printf("Font scale: 2x\n");
    } else if (kstrcmp(command, "dir") == 0) {
        zeus_cmd_ls(shell, argc, argv);
    } else if (kstrcmp(command, "terminals") == 0) {
        extern int cmd_list_terminals(void);
        cmd_list_terminals();
    } else if (kstrcmp(command, "terminal") == 0) {
        extern int cmd_set_active_terminal(const char *name);
        extern int cmd_list_terminals(void);
        if (argc < 2) {
            cmd_list_terminals();
        } else if (kstrcmp(argv[1], "list") == 0) {
            cmd_list_terminals();
        } else if (kstrcmp(argv[1], "reset") == 0 || kstrcmp(argv[1], "zeus") == 0) {
            cmd_set_active_terminal(NULL);
        } else {
            cmd_set_active_terminal(argv[1]);
        }
    } else if (kstrcmp(command, "smp") == 0) {
        /* SMP status and control */
        extern void smp_print_status(void);
        smp_print_status();
    } else if (kstrcmp(command, "xhci") == 0) {
        /* xHCI USB 3.0 controller status */
        extern void xhci_print_status(void);
        xhci_print_status();
    } else if (kstrcmp(command, "insmod") == 0) {
        /* Load kernel module */
        extern int kmod_load(const char *filename);
        if (argc < 2) {
            printf("Usage: insmod <module.ko>\n");
        } else {
            if (kmod_load(argv[1]) == 0) {
                printf("Module loaded successfully\n");
            }
        }
    } else if (kstrcmp(command, "rmmod") == 0) {
        /* Unload kernel module */
        extern int kmod_unload(const char *name);
        if (argc < 2) {
            printf("Usage: rmmod <module_name>\n");
        } else {
            if (kmod_unload(argv[1]) == 0) {
                printf("Module unloaded\n");
            }
        }
    } else if (kstrcmp(command, "lsmod") == 0) {
        /* List kernel modules */
        extern void kmod_list(void);
        kmod_list();
    } else if (kstrcmp(command, "fputest") == 0) {
        extern void fpu_self_test(void);
        fpu_self_test();
    } else if (kstrcmp(command, "hpet") == 0) {
        extern void hpet_print_status(void);
        hpet_print_status();
    } else if (kstrcmp(command, "prof") == 0) {
        profiler_cli(argc, argv);
    } else if (kstrcmp(command, "scheddebug") == 0) {
        if (argc >= 2 && kstrcmp(argv[1], "off") == 0) {
            sched_debug_switches = false;
            printf("Scheduler switch logging: OFF\n");
        } else if (argc >= 2 && kstrcmp(argv[1], "on") == 0) {
            sched_debug_switches = true;
            printf("Scheduler switch logging: ON (serial)\n");
        } else {
            printf("Scheduler switch logging: %s\n",
                   sched_debug_switches ? "ON" : "OFF");
            printf("Usage: scheddebug on|off\n");
        }
    } else if (kstrcmp(command, "srv") == 0) {
        if (http_server_running) {
            printf("HTTP server is already running on port 80\n");
            printf("Open http://192.168.56.2 in your browser!\n");
        } else {
            printf("Starting HTTP server on port 80...\n");
            int result = tcp_listen(80, http_server_handle_connection);
            if (result == 0) {
                http_server_running = true;
                printf("✓ HTTP server started successfully!\n");
                printf("  Visit: http://192.168.56.2\n");
                printf("  Server will respond with 'Hello World!' 🚀\n");
            } else {
                printf("✗ Failed to start HTTP server (error %d)\n", result);
            }
        }
    } else if (kstrcmp(command, "doom") == 0) {
        if (zeus_no_external) {
            printf("doom: launch it from the dock\n");
            shell->last_status = 126;
        } else {
            char *doom_argv[2] = { (argc >= 2) ? argv[1] : (char *)"doom", NULL };
            if (!zeus_try_run_executable(shell, 1, doom_argv)) {
                printf("doom: not found (try: doom /path/to/doom)\n");
            }
        }
    } else {
        printf("Command not found: %s\n", command);
        printf("Type 'help' for available commands\n");
        shell->last_status = 127;
    }
}

void zeus_execute_external(const char* command_path, int argc, char* argv[]) {
    printf("Executing external command: %s\n", command_path);

    // For now, just try to execute the file
    // In a real implementation, this would spawn a new process
    exec_file(command_path, argv);
}

void zeus_add_to_history(ZeusShell* shell, const char* command) {
    if (shell->history_count < 50) {
        kstrncpy(shell->history[shell->history_count], command, CMD_BUFFER_SIZE);
        shell->history_count++;
    } else {
        // Shift history
        for (int i = 0; i < 49; i++) {
            kstrncpy(shell->history[i], shell->history[i + 1], CMD_BUFFER_SIZE);
        }
        kstrncpy(shell->history[49], command, CMD_BUFFER_SIZE);
    }
}

/* Built-in commands live in zeus_cmds.c */

// History navigation index: -1 means "not navigating" (show current input)
static int history_nav_pos = -1;

// Initialize Zeus shell service
void zeus_init_service(void) {
    if (global_shell == NULL) {
        global_shell = (ZeusShell*)kalloc(sizeof(ZeusShell));
        if (global_shell) {
            zeus_init(global_shell);
            printf("Zeus Shell v%s - inDevOS Command Processor\n", ZEUS_VERSION);
            printf("Type 'help' for commands, 'sh' for the userspace shell.\n\n");
        } else {
            printf("Failed to initialize Zeus shell\n");
        }
    }
}

// ============================================================================
// Shell piping & redirection support
// ============================================================================

// Global pipe-input buffer: filled by the previous pipeline stage.
// Commands that want to consume piped input should read from here.
static char  *zeus_pipe_input      = NULL;
static size_t zeus_pipe_input_len  = 0;
static int zeus_run_pipeline(char *trimmed_cmd);

const char* zeus_get_pipe_input(size_t *out_len) {
    if (out_len) *out_len = zeus_pipe_input_len;
    return zeus_pipe_input;
}

/* Trim a string in-place and return the trimmed start pointer */
static const char* zeus_trim(const char *s, size_t *out_len) {
    while (*s && (*s == ' ' || *s == '\t')) s++;
    if (*s == '\0') { *out_len = 0; return s; }
    const char *e = s + strlen(s) - 1;
    while (e > s && (*e == ' ' || *e == '\t' || *e == '\n' || *e == '\r' || *e == '\b' || *e == '\x7f')) e--;
    *out_len = (size_t)(e - s + 1);
    return s;
}

/* Execute a single command (no pipes/redirects) */
static void zeus_exec_single(const char *cmd_str, size_t len) {
    char single[CMD_BUFFER_SIZE];
    if (len >= CMD_BUFFER_SIZE) len = CMD_BUFFER_SIZE - 1;
    memcpy(single, cmd_str, len);
    single[len] = '\0';

    char *args[MAX_ARGS];
    int argc = zeus_parse_command_line(single, args);
    if (argc > 0) {
        zeus_execute_command(global_shell, argc, args);
    }
}

#define CAPTURE_BUF_SIZE  (48 * 1024)  /* curl keeps up to 32 KB of body */

// Process command through Zeus shell service
static unsigned char zeus_op_byte(const char **pp, int *sq, int *dq);
int zeus_process_command_service(const char* command_line) {
    if (global_shell == NULL) {
        zeus_init_service();
    }
    if (global_shell == NULL) {
        printf("Failed to initialize Zeus shell\n");
        return -1;
    }
    if (!command_line) return 0;

    /* Kernel Zeus session: stdout for this command uses the active VC as controlling TTY. */
    {
        process_t *kp = get_current_process();
        if (kp) {
            kp->cont_tty = (int8_t)vconsole_get_active();
        }
    }

    // Trim the full command line
    size_t full_len;
    const char *full = zeus_trim(command_line, &full_len);
    if (full_len == 0) return 0;

    char trimmed_cmd[CMD_BUFFER_SIZE];
    if (full_len >= CMD_BUFFER_SIZE) full_len = CMD_BUFFER_SIZE - 1;
    memcpy(trimmed_cmd, full, full_len);
    trimmed_cmd[full_len] = '\0';

    // Add command to history and reset navigation position
    zeus_add_to_history(global_shell, trimmed_cmd);
    history_nav_pos = -1;

    char expanded[CMD_BUFFER_SIZE];
    zeus_expand_vars(expanded, sizeof(expanded), trimmed_cmd);

    char *job = expanded;
    int prev_op = 0; /* 0 = first, 1 = &&, 2 = || */
    while (job && *job) {
        char *op = NULL;
        int next_op = 0;
        {
            const char *scan = job;
            int sq = 0, dq = 0;
            while (*scan) {
                const char *here = scan;
                unsigned char c = zeus_op_byte(&scan, &sq, &dq);
                if (!c) continue;
                if (c == '&' && *scan == '&') {
                    op = (char *)here; next_op = 1; break;
                }
                if (c == '|' && *scan == '|') {
                    op = (char *)here; next_op = 2; break;
                }
            }
        }
        if (op) {
            *op = '\0';
            op[1] = '\0';
        }

        int skip = 0;
        if (prev_op == 1 && global_shell->last_status != 0) skip = 1;
        if (prev_op == 2 && global_shell->last_status == 0) skip = 1;

        if (!skip)
            zeus_run_pipeline(job);

        if (!op) break;
        job = op + 2;
        prev_op = next_op;
    }
    return 0;
}

static int zeus_redir_abs(char *dst, size_t dstsz, const char *path) {
    if (!dst || dstsz == 0) return -1;
    dst[0] = '\0';
    if (!path || !path[0]) return -1;
    if (path[0] == '/') {
        size_t n = strlen(path);
        if (n + 1 > dstsz) return -1;
        memcpy(dst, path, n + 1);
        return 0;
    }
    const char *cwd = zeus_get_cwd();
    if (!cwd || !cwd[0]) cwd = "/";
    size_t cl = strlen(cwd);
    size_t pl = strlen(path);
    int slash = (cl > 0 && cwd[cl - 1] != '/');
    if (cl + (size_t)slash + pl + 1 > dstsz) return -1;
    memcpy(dst, cwd, cl);
    size_t o = cl;
    if (slash) dst[o++] = '/';
    memcpy(dst + o, path, pl);
    dst[o + pl] = '\0';
    return 0;
}
static int zeus_vfs_write_redir(const char *path, const void *data, size_t n, bool append) {
    char full[256];
    if (zeus_redir_abs(full, sizeof(full), path) != 0) return -1;
    path = full;
    if (vfs_has_root_mount()) {
        uint32_t flags = VFS_O_WRITE | VFS_O_CREATE | (append ? VFS_O_APPEND : VFS_O_TRUNC);
        int fd = vfs_open(path, flags);
        if (fd < 0) return -1;
        if (n) {
            int wr = vfs_write(fd, data, (uint32_t)n);
            vfs_close(fd);
            if (wr < 0) return -1;
        } else {
            vfs_close(fd);
        }
        return 0;
    }
    if (!append && fat32_file_exists(path))
        fat32_truncate_file(path);
    else if (!fat32_file_exists(path))
        fat32_create_file(path);
    fat32_file_t *f = fat32_open(path, "w");
    if (!f) return -1;
    if (append)
        fat32_seek(f, f->file_size);
    else
        fat32_seek(f, 0);
    if (n)
        fat32_write(f, data, (uint32_t)n);
    fat32_close(f);
    return 0;
}

static int zeus_vfs_read_redir(const char *path, char *dst, size_t dstsz, size_t *out_len) {
    char full[256];
    *out_len = 0;
    if (zeus_redir_abs(full, sizeof(full), path) != 0) return -1;
    path = full;
    if (vfs_has_root_mount()) {
        int fd = vfs_open(path, VFS_O_READ);
        if (fd < 0) return -1;
        int n = vfs_read(fd, dst, (uint32_t)(dstsz - 1));
        vfs_close(fd);
        if (n < 0) return -1;
        dst[n] = '\0';
        *out_len = (size_t)n;
        return 0;
    }
    fat32_file_t *f = fat32_open(path, "r");
    if (!f) return -1;
    uint32_t n = fat32_read(f, dst, (uint32_t)(dstsz - 1));
    fat32_close(f);
    dst[n] = '\0';
    *out_len = n;
    return 0;
}


/* Next byte for operator detection. Quoted/escaped chars yield 0 so
 * '|', '>', '&' inside quotes are not Zeus operators. */
static unsigned char zeus_op_byte(const char **pp, int *sq, int *dq) {
    const char *p = *pp;
    if (!p || !*p) return 0;
    if (*sq) {
        if (*p == '\'') { *sq = 0; *pp = p + 1; return 0; }
        *pp = p + 1;
        return 0;
    }
    if (*dq) {
        if (*p == '\\' && p[1]) { *pp = p + 2; return 0; }
        if (*p == '"') { *dq = 0; *pp = p + 1; return 0; }
        *pp = p + 1;
        return 0;
    }
    if (*p == '\\' && p[1]) { *pp = p + 2; return 0; }
    if (*p == '\'') { *sq = 1; *pp = p + 1; return 0; }
    if (*p == '"') { *dq = 1; *pp = p + 1; return 0; }
    unsigned char c = (unsigned char)*p;
    *pp = p + 1;
    return c;
}

static int zeus_run_pipeline(char *trimmed_cmd) {
    size_t full_len = strlen(trimmed_cmd);
    // ------------------------------------------------------------------
    // Split on '|' to get pipeline stages (not '||')
    // ------------------------------------------------------------------
    char *stages[16];
    int nstages = 0;
    {
        char *p = trimmed_cmd;
        stages[nstages++] = p;
        int sq = 0, dq = 0;
        while (*p) {
            const char *scan = p;
            unsigned char c = zeus_op_byte(&scan, &sq, &dq);
            if (c == '|' && *scan != '|') {
                *p = '\0';
                if (nstages < 16) stages[nstages++] = (char *)scan;
                p = (char *)scan;
                continue;
            }
            p = (char *)scan;
        }
    }

    // Allocate capture buffers (we'll reuse two: prev_buf and cur_buf)
    char *buf_a = (char *)kalloc(CAPTURE_BUF_SIZE);
    char *buf_b = (char *)kalloc(CAPTURE_BUF_SIZE);
    if (!buf_a || !buf_b) {
        // Fallback: run as a single command without pipe support
        zeus_pipe_input = NULL;
        zeus_pipe_input_len = 0;
        zeus_exec_single(trimmed_cmd, full_len);
        if (buf_a) kfree(buf_a);
        if (buf_b) kfree(buf_b);
        return 0;
    }

    char *prev_buf = buf_a;
    char *cur_buf  = buf_b;
    size_t prev_len = 0;

    for (int stage = 0; stage < nstages; stage++) {
        size_t slen;
        const char *scmd = zeus_trim(stages[stage], &slen);
        if (slen == 0) continue;
        char stage_cmd[CMD_BUFFER_SIZE];
        if (slen >= CMD_BUFFER_SIZE) slen = CMD_BUFFER_SIZE - 1;
        memcpy(stage_cmd, scmd, slen);
        stage_cmd[slen] = '\0';
        scmd = stage_cmd;

        // Set pipe input for this stage (from previous stage's output)
        if (stage > 0) {
            zeus_pipe_input     = prev_buf;
            zeus_pipe_input_len = prev_len;
        } else {
            zeus_pipe_input     = NULL;
            zeus_pipe_input_len = 0;
        }

        /* Input redirect: cmd < file  (first stage only) */
        if (stage == 0) {
            char *lt = NULL;
            {
                const char *scan = stage_cmd;
                int sq = 0, dq = 0;
                while (*scan) {
                    const char *here = scan;
                    unsigned char c = zeus_op_byte(&scan, &sq, &dq);
                    if (c == '<' && *scan != '<') { lt = (char *)here; break; }
                }
            }
            if (lt) {
                *lt = '\0';
                slen = strlen(stage_cmd);
                scmd = zeus_trim(stage_cmd, &slen);
                const char *fname = lt + 1;
                while (*fname == ' ' || *fname == '\t') fname++;
                char inpath[128];
                size_t fl = 0;
                while (fname[fl] && fname[fl] != ' ' && fname[fl] != '\t' &&
                       fl + 1 < sizeof(inpath)) {
                    inpath[fl] = fname[fl];
                    fl++;
                }
                inpath[fl] = '\0';
                if (inpath[0]) {
                    if (zeus_vfs_read_redir(inpath, prev_buf, CAPTURE_BUF_SIZE, &prev_len) != 0) {
                        printf("zeus: cannot open '%s' for reading\n", inpath);
                        global_shell->last_status = 1;
                        goto done;
                    }
                    zeus_pipe_input = prev_buf;
                    zeus_pipe_input_len = prev_len;
                }
            }
        }

        // Check for output redirection on the LAST stage
        bool redir_append = false;
        char redir_file[128] = {0};
        if (stage == nstages - 1) {
            // Look for >> or > in the command
            // We need to search the raw characters
            char stg_copy[CMD_BUFFER_SIZE];
            memcpy(stg_copy, scmd, slen);
            stg_copy[slen] = '\0';

            char *redir_pos = NULL;
            {
                const char *scan = stg_copy;
                int sq = 0, dq = 0;
                while (*scan) {
                    const char *here = scan;
                    unsigned char c = zeus_op_byte(&scan, &sq, &dq);
                    if (!c) continue;
                    if (c == '>' && *scan == '>') {
                        redir_append = true;
                        redir_pos = (char *)here;
                        break;
                    }
                    if (c == '>') {
                        redir_append = false;
                        redir_pos = (char *)here;
                        break;
                    }
                }
            }

            if (redir_pos) {
                /* Null-terminate the command before '>' */
                *redir_pos = '\0';
                slen = strlen(stg_copy);
                scmd = zeus_trim(stg_copy, &slen);

                /* Parse the filename after > or >> */
                const char *fname_start = redir_pos + 1;
                if (redir_append) fname_start++;  /* skip second '>' */
                while (*fname_start == ' ' || *fname_start == '\t') fname_start++;
                size_t flen = strlen(fname_start);
                /* Trim trailing whitespace from filename */
                while (flen > 0 && (fname_start[flen-1] == ' ' || fname_start[flen-1] == '\t'))
                    flen--;
                if (flen > 0 && flen < sizeof(redir_file)) {
                    memcpy(redir_file, fname_start, flen);
                    redir_file[flen] = '\0';
                }

                /* Capture output to file */
                term_capture_start(cur_buf, CAPTURE_BUF_SIZE, true);
                zeus_exec_single(scmd, slen);
                size_t captured = term_capture_stop();

                if (redir_file[0]) {
                    if (zeus_vfs_write_redir(redir_file, cur_buf, captured, redir_append) != 0)
                        printf("zeus: cannot open '%s' for writing\n", redir_file);
                }
                goto done;
            }
        }

        if (stage < nstages - 1) {
            /* Intermediate stage: capture output silently */
            term_capture_start(cur_buf, CAPTURE_BUF_SIZE, true);
            zeus_exec_single(scmd, slen);
            prev_len = term_capture_stop();
            /* Swap buffers */
            char *tmp = prev_buf;
            prev_buf = cur_buf;
            cur_buf = tmp;
        } else {
            /* Last stage: output goes to terminal normally */
            zeus_exec_single(scmd, slen);
        }
    }

done:
    zeus_pipe_input     = NULL;
    zeus_pipe_input_len = 0;
    kfree(buf_a);
    kfree(buf_b);
    return 0;
}

// History navigation API (called from main loop on arrow key presses)
const char* zeus_history_prev(void) {
    if (!global_shell || global_shell->history_count == 0) return NULL;
    if (history_nav_pos < 0)
        history_nav_pos = global_shell->history_count - 1;
    else if (history_nav_pos > 0)
        history_nav_pos--;
    /* else already at oldest entry */
    return global_shell->history[history_nav_pos];
}

const char* zeus_history_next(void) {
    if (!global_shell || global_shell->history_count == 0) return NULL;
    if (history_nav_pos < 0) return NULL; /* not navigating */
    if (history_nav_pos < global_shell->history_count - 1) {
        history_nav_pos++;
        return global_shell->history[history_nav_pos];
    }
    /* Past newest entry => return to empty line */
    history_nav_pos = -1;
    return "";
}

void zeus_history_reset_nav(void) {
    history_nav_pos = -1;
}

// Entry point for Zeus shell (for testing)
int main(int argc, char* argv[]) {
    printf("Starting Zeus Shell...\n");
    zeus_init_service();

    // For testing, run a few commands
    zeus_process_command_service("help");
    zeus_process_command_service("pwd");
    zeus_process_command_service("version");

    return 0;
}
