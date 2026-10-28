#include "zeus_internal.h"
#include "types.h"
#include "string.h"
#include "mem.h"
#include "fat32.h"
#include "vfs.h"
#include "term.h"
#include "hpet.h"
#include "ktime.h"
#include "rtc.h"
#include "storage.h"
#include "atomic.h"
#include "utils.h"
#include "net_tcp.h"
#include "net_dns.h"
#include "net_device.h"
#include "serial.h"
#include "cmd.h"
#include "syscall.h"
#include "thread.h"
#include "stdio.h"

extern int snprintf(char *str, size_t size, const char *fmt, ...);

void zeus_cmd_time(ZeusShell* shell, int argc, char* argv[]) {
    if (argc < 2) {
        printf("usage: time <program> [args...]\n");
        printf("  e.g. time ls /   time wc /doom1.wad   time grep WAD /doom1.wad\n");
        return;
    }
    if (kstrcmp(argv[1], "time") == 0) {
        printf("time: refusing to time itself\n");
        return;
    }

    uint64_t start_us = ktime_now_us();
    zeus_time_check_us = 0;
    zeus_time_exec_us  = 0;
    zeus_execute_command(shell, argc - 1, &argv[1]);
    uint64_t end_us = ktime_now_us();

    uint64_t total_us = end_us - start_us;
    uint32_t ms = (uint32_t)(total_us / 1000u);

    printf("time: %s took %u.%03u s (%u ms)\n", argv[1],
           ms / 1000u, ms % 1000u, ms);
    if (zeus_time_exec_us) {
        printf("time:   shell lookup %u ms, program load+run %u ms\n",
               (uint32_t)(zeus_time_check_us / 1000u),
               (uint32_t)(zeus_time_exec_us / 1000u));
    }

    /* Show what loading the program alone costs: re-read the same file (now
     * warm in the block cache) and time that.  It separates "this program is
     * slow" from "loading anything is slow", which is the usual answer for a
     * small utility. */
    const char *name = argv[1];
    char probe[256];
    if (name[0] != '/')
        snprintf(probe, sizeof(probe), "/%s.exe", name);
    else
        snprintf(probe, sizeof(probe), "%s", name);

    if (vfs_has_root_mount() && vfs_exists(probe)) {
        int fd = vfs_open(probe, VFS_O_READ);
        if (fd >= 0) {
            int size = vfs_seek(fd, 0, VFS_SEEK_END);
            vfs_seek(fd, 0, VFS_SEEK_SET);
            if (size > 0 && size <= (int)(512 * 1024)) {
                void *buf = kalloc((uint32_t)size);
                if (buf) {
                    uint64_t t0 = ktime_now_us();
                    int got = vfs_read(fd, buf, (uint32_t)size);
                    uint64_t t1 = ktime_now_us();
                    if (got > 0) {
                        printf("time:   (%d byte program file read in %u us)\n",
                               got, (uint32_t)(t1 - t0));
                    }
                    kfree(buf);
                }
            }
            vfs_close(fd);
        }
    }
}

void zeus_cmd_uptime(void) {
    char buf[64];
    /* hpet_get_ticks() is already milliseconds (see hpet.c). */
    format_uptime(hpet_get_ticks(), buf, sizeof(buf));
    printf("up %s\n", buf);
}

// Forward declarations for pipe support

// Built-in command implementations

void zeus_cmd_cd(ZeusShell* shell, int argc, char* argv[]) {
    if (argc < 2) {
        printf("Usage: cd <directory>\n");
        return;
    }

    // For now, just update the current directory string
    // In a real implementation, this would change the actual working directory
    if (kstrcmp(argv[1], "..") == 0) {
        // Go up one directory (simplified)
        // Find the last slash manually
        int len = strlen(shell->current_dir);
        int last_slash_pos = -1;
        for (int i = len - 1; i >= 0; i--) {
            if (shell->current_dir[i] == '/') {
                last_slash_pos = i;
                break;
            }
        }

        if (last_slash_pos > 0) {
            shell->current_dir[last_slash_pos] = '\0';
        } else {
            kstrncpy(shell->current_dir, "/", sizeof(shell->current_dir));
        }
    } else {
        // Change to specified directory
        if (argv[1][0] == '/') {
            // Absolute path
            kstrncpy(shell->current_dir, argv[1], sizeof(shell->current_dir));
        } else {
            // Relative path
            int len = strlen(shell->current_dir);
            if (len > 1 && (size_t)len < sizeof(shell->current_dir) - 1) {
                shell->current_dir[len] = '/';
                shell->current_dir[len + 1] = '\0';
            }
            // Simple concatenation
            int remaining_space = sizeof(shell->current_dir) - strlen(shell->current_dir) - 1;
            if (remaining_space > 0) {
                kstrncpy(shell->current_dir + strlen(shell->current_dir),
                        argv[1], remaining_space);
            }
        }
    }

    printf("Changed directory to: %s\n", shell->current_dir);
    zeus_sync_process_cwd(shell->current_dir);
}

void zeus_cmd_pwd(ZeusShell* shell, int argc, char* argv[]) {
    printf("%s\n", shell->current_dir);
}

void zeus_cmd_ls(ZeusShell* shell, int argc, char* argv[]) {
    if (!vfs_has_root_mount() && !fat32_is_mounted()) {
        printf("No filesystem mounted. Use 'mount' command first.\n");
        return;
    }

    printf("Directory listing:\n");
    printf("Type   Size       Name\n");
    printf("-----  ---------- --------\n");

    if (vfs_has_root_mount()) {
        vfs_dirent_t vfs_entries[32];
        int count = vfs_readdir(shell->current_dir, vfs_entries, 32);
        if (count < 0) { printf("Error reading directory\n"); return; }
        if (count == 0) { printf("(empty directory)\n"); return; }
        for (int i = 0; i < count; i++) {
            printf("%s   %10u   %s\n",
                   (vfs_entries[i].type == VFS_DIRECTORY) ? "DIR " : "FILE",
                   vfs_entries[i].size, vfs_entries[i].name);
        }
        printf("\nTotal: %d items\n", count);
    } else {
        static fat32_dir_list_t entries[32];
        int count = fat32_list_directory(shell->current_dir, entries, 32);
        if (count < 0) { printf("Error reading directory\n"); return; }
        if (count == 0) { printf("(empty directory)\n"); return; }
        for (int i = 0; i < count; i++) {
            printf("%s   %10d   %s\n",
                   entries[i].is_directory ? "DIR " : "FILE",
                   entries[i].size, entries[i].name);
        }
        printf("\nTotal: %d items\n", count);
    }
}

void zeus_cmd_help(ZeusShell* shell, int argc, char* argv[]) {
    printf("Zeus Shell v%s  -  inDevOS Command Reference\n\n", ZEUS_VERSION);

    printf("Shell:\n");
    printf("  help       - Show this help\n");
    printf("  sh / shell - Enter the userspace (Ring-3) shell\n");
    printf("  clear      - Clear the screen\n");
    printf("  version    - Show shell version\n");
    printf("  history    - Show command history\n");
    printf("  time <cmd> - Run <cmd> and report how long it took\n");
    printf("  date       - Current date/time     uptime - Time since boot\n");
    printf("  exit       - Exit the shell\n");
    printf("  shutdown / poweroff / reboot\n");
    printf("  Dispatch:  disk programs win; cd/export/set/unset/exit stay built-in\n");

    printf("\nFilesystem:\n");
    printf("  cd <dir>   pwd   ls / dir   cat <file>   echo [msg]\n");
    printf("  touch <f>  mkdir <d>   rm <f>   cp <s> <d>   mv <s> <d>\n");
    printf("  storage    mount <id>  unmount  format <id> [label]  fsinfo\n");
    printf("  exec <file>   install [id] [yes]\n");

    printf("\nNetwork:\n");
    printf("  net             - Show network status\n");
    printf("  dhcp            - Start DHCP client\n");
    printf("  ddhcp           - Apply QEMU user-net defaults (10.0.2.15)\n");
    printf("  tapconfig       - Apply TAP network defaults (192.168.56.2)\n");
    printf("  qemunet         - Alias for ddhcp\n");
    printf("  netset ip <ip> [mask <m>] [gw <g>] [dns <d>]\n");
    printf("  setip <ip>      - Set IP directly\n");
    printf("  ping <host> [-c N]  dns <host>  mac\n");
    printf("  arp <ip>  arpstatus  arptest  addarp <ip> <mac>\n");
    printf("  netverbose on|off   udpverbose on|off\n");
    printf("  stopdhcp  forcepack  sendtest  netdebug\n");
    printf("  rtldebug  inttest   checkpackets  enabletx\n");
    printf("  srv             - Start HTTP server on port 80\n");
    printf("  curl [-v] [-I] <url>  ipv6 [enable|status|ping <addr>]\n");

    printf("\nUSB / Input:\n");
    printf("  lsusb [-v]  xhci  usb  inputinfo\n");
    printf("  keyboard  testkey  kblayout <us|uk|de|fr>\n");

    printf("\nAudio:\n");
    printf("  audio              - Status\n");
    printf("  audio test [f][ms] - Play tone\n");
    printf("  audio volume <N>   audio stop\n");

    printf("\nDisplay / Desktop:\n");
    printf("  font1x  font2x  res [W H]  img <file>  matrix  gpu\n");
    printf("  menu / tui  open\n");
    printf("  desktop            - Start graphical desktop environment\n");
    printf("  spawn <program>    - Launch a program in the background\n");

    printf("\nProcesses & Modules:\n");
    printf("  ps  kill <pid>  task  smp\n");
    printf("  lsmod  insmod <file.ko>  rmmod <name>\n");
    printf("  driver [subcommand]  -  use 'driver help'\n");
    printf("  dynlink / dlopen [list|load|unload|resolve]\n");

    printf("\nUsers & Permissions:\n");
    printf("  whoami  login <u> <p>  logout  su <u> [p]\n");
    printf("  useradd <u> <p>  userdel <u>  passwd [u] <p>  users\n");
    printf("  groupadd <g>  groups  chmod <mode> <path>  chown <u> <path>\n");
    printf("  perms [path]  saveperm\n");

    printf("\nPackage Manager:\n");
    printf("  pkg list | install <f> | uninstall <n> | info <n> | search <kw>\n");

    printf("\nSettings & Clipboard:\n");
    printf("  settings [set <key> <val>] [save] [load] [reset]\n");
    printf("  clip / clipboard [copy <txt>] [paste] [clear]\n");

    printf("\nTimers & Debug:\n");
    printf("  timertest  timeraccuracy  stopwatch  hpet  fputest\n");
    printf("  prof start [div]|stop|reset|stats|dump|overlay on|off|sample <n>\n");
    printf("  threadtest [n] [iter]  memtest [n] [size]\n");
    printf("  tcpinfo  tcpclean  cachestats\n");

    printf("\nScripting:\n");
    printf("  Pipe:     cmd1 | cmd2 | cmd3\n");
    printf("  Redirect: cmd > file  or  cmd >> file\n");
    printf("  grep <pattern> [file]  wc [file]  head [-n N] [file]\n");

    printf("\nTerminal Backends:\n");
    printf("  terminals        - List registered terminal backends\n");
    printf("  terminal <name>  - Switch to a terminal backend\n");
    printf("  terminal reset   - Revert to zeus (default)\n");
}

void zeus_cmd_clear(ZeusShell* shell, int argc, char* argv[]) {
    clear();
}

void zeus_cmd_history(ZeusShell* shell, int argc, char* argv[]) {
    printf("Command History:\n");
    for (int i = 0; i < shell->history_count; i++) {
        printf("  %d: %s\n", i + 1, shell->history[i]);
    }
}

void zeus_cmd_version(ZeusShell* shell, int argc, char* argv[]) {
    printf("Zeus Shell v%s\n", ZEUS_VERSION);
    printf("inDevOS Command Processor\n");
    printf("Built with modern shell features\n");
}

// Additional built-in command implementations

void zeus_cmd_cat(ZeusShell* shell, int argc, char* argv[]) {
    if (argc < 2) {
        printf("Usage: cat <filename>\n");
        return;
    }

    const char* filename = argv[1];
    char abs_path[256];
    const char* path = filename;
    if (filename[0] != '/') {
        abs_path[0] = '/';
        int i = 1;
        for (int j = 0; filename[j] && i < 255; j++)
            abs_path[i++] = filename[j];
        abs_path[i] = '\0';
        path = abs_path;
    }

    if (!vfs_has_root_mount() && !fat32_is_mounted()) {
        printf("No filesystem mounted. Use 'mount' command first.\n");
        return;
    }

    if (vfs_has_root_mount()) {
        int vfd = vfs_open(path, VFS_O_READ);
        if (vfd < 0) { printf("Error: Could not open file '%s'\n", filename); return; }
        static char buffer[1024];
        int bytes_read;
        while ((bytes_read = vfs_read(vfd, buffer, sizeof(buffer) - 1)) > 0) {
            buffer[bytes_read] = '\0';
            printf("%s", buffer);
        }
        printf("\n");
        vfs_close(vfd);
    } else {
        fat32_file_t* file = fat32_open(path, "r");
        if (!file) { printf("Error: Could not open file '%s'\n", filename); return; }
        static char buffer[1024];
        int bytes_read;
        while ((bytes_read = fat32_read(file, buffer, sizeof(buffer) - 1)) > 0) {
            buffer[bytes_read] = '\0';
            printf("%s", buffer);
        }
        printf("\n");
        fat32_close(file);
    }
}

void zeus_cmd_echo(ZeusShell* shell, int argc, char* argv[]) {
    // Skip the command name (argv[0])
    for (int i = 1; i < argc; i++) {
        if (i > 1) {
            printf(" ");
        }
        printf("%s", argv[i]);
    }
    printf("\n");
}

void zeus_cmd_touch(ZeusShell* shell, int argc, char* argv[]) {
    if (argc < 2) {
        printf("Usage: touch <filename>\n");
        return;
    }

    const char* filename = argv[1];

    if (!vfs_has_root_mount() && !fat32_is_mounted()) {
        printf("No filesystem mounted. Use 'mount' command first.\n");
        return;
    }

    printf("Creating file: %s\n", filename);
    int rc = vfs_has_root_mount() ? vfs_create(filename) : fat32_create_file(filename);
    if (rc == 0)
        printf("File '%s' created successfully\n", filename);
    else
        printf("Failed to create file '%s'\n", filename);
}

void zeus_cmd_mkdir(ZeusShell* shell, int argc, char* argv[]) {
    if (argc < 2) {
        printf("Usage: mkdir <dirname>\n");
        return;
    }

    const char* dirname = argv[1];

    if (!vfs_has_root_mount() && !fat32_is_mounted()) {
        printf("No filesystem mounted. Use 'mount' command first.\n");
        return;
    }

    printf("Creating directory: %s\n", dirname);
    int rc = vfs_has_root_mount() ? vfs_mkdir(dirname) : fat32_create_directory(dirname);
    if (rc == 0)
        printf("Directory '%s' created successfully\n", dirname);
    else
        printf("Failed to create directory '%s'\n", dirname);
}

void zeus_cmd_system(ZeusShell* shell, int argc, char* argv[]) {
    /* The 'cmd' command is deprecated - all commands now live directly in Zeus.
     * This wrapper forwards to zeus_process_command_service so that any command
     * typed as "cmd foo bar" is equivalent to typing "foo bar" directly. */
    if (argc < 2) {
        printf("NOTE: The 'cmd' prefix is deprecated. All commands are available directly.\n");
        printf("Example: instead of 'cmd help', just type 'help'.\n");
        return;
    }

    char cmd_buffer[CMD_BUFFER_SIZE] = {0};
    int pos = 0;
    for (int i = 1; i < argc && pos < CMD_BUFFER_SIZE - 1; i++) {
        if (i > 1) cmd_buffer[pos++] = ' ';
        int len = strlen(argv[i]);
        if (pos + len < CMD_BUFFER_SIZE - 1) {
            kstrncpy(cmd_buffer + pos, argv[i], len);
            pos += len;
        }
    }
    cmd_buffer[pos] = '\0';
    zeus_process_command_service(cmd_buffer);
}

//=============================================================================
// Testing Commands
//=============================================================================

// Cooperative multitasking test: round-robin worker stepping from the
// command loop.  (Ring-0 kernel threads are not preemptively scheduled by
// this kernel — see preemptive_schedule() in idt.c — so the test drives
// the workers cooperatively, as the old task manager did.)

// Shared state for the task test
static volatile int task_test_counter = 0;
static volatile int task_test_completed[8] = {0};
static int task_test_iterations = 10;
static int task_test_progress[8] = {0};
static spinlock_t task_test_lock = {0};

// One worker iteration: increments the shared counter under the lock.
static void task_test_step(int idx) {
    if (task_test_progress[idx] >= task_test_iterations) return;

    klock(&task_test_lock);
    task_test_counter++;
    kunlock(&task_test_lock);

    int i = ++task_test_progress[idx];
    if (i % 5 == 0 || i == task_test_iterations) {
        printf("[Task %d] Iteration %d/%d, counter=%d\n",
               idx + 1, i, task_test_iterations, task_test_counter);
    }
    if (i >= task_test_iterations) {
        task_test_completed[idx] = 1;
        printf("[Task %d] Completed!\n", idx + 1);
    }
}

// Reset shared state before a test run
static void reset_task_workers(void) {
    task_test_counter = 0;
    for (int i = 0; i < 8; i++) {
        task_test_completed[i] = 0;
        task_test_progress[i] = 0;
    }
}

// ============================================================================
// grep / wc / head - pipe-aware commands
// ============================================================================

/* Simple substring search (case-sensitive) */
static bool zeus_strcontains(const char *haystack, size_t hlen, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0 || nlen > hlen) return false;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        bool match = true;
        for (size_t j = 0; j < nlen; j++) {
            if (haystack[i+j] != needle[j]) { match = false; break; }
        }
        if (match) return true;
    }
    return false;
}

void zeus_cmd_grep(ZeusShell* shell, int argc, char* argv[]) {
    (void)shell;
    if (argc < 2) {
        printf("Usage: grep <pattern> [file]\n");
        printf("  Reads from piped input or a file, prints lines containing <pattern>\n");
        return;
    }
    const char *pattern = argv[1];

    const char *input = NULL;
    size_t input_len = 0;
    char *file_buf = NULL;

    if (argc >= 3) {
        /* Read from file */
        fat32_file_t *f = fat32_open(argv[2], "r");
        if (!f) { printf("grep: cannot open '%s'\n", argv[2]); return; }
        file_buf = (char *)kalloc(f->file_size + 1);
        if (!file_buf) { fat32_close(f); printf("grep: out of memory\n"); return; }
        int rd = fat32_read(f, file_buf, f->file_size);
        fat32_close(f);
        if (rd < 0) { kfree(file_buf); return; }
        file_buf[rd] = '\0';
        input = file_buf;
        input_len = (size_t)rd;
    } else {
        /* Read from piped input */
        input = zeus_get_pipe_input(&input_len);
        if (!input || input_len == 0) {
            printf("grep: no input (pipe or file required)\n");
            return;
        }
    }

    /* Process line by line */
    const char *p = input;
    const char *end = input + input_len;
    while (p < end) {
        const char *nl = p;
        while (nl < end && *nl != '\n') nl++;
        size_t line_len = (size_t)(nl - p);
        if (zeus_strcontains(p, line_len, pattern)) {
            /* Print matching line */
            for (size_t i = 0; i < line_len; i++) printf("%c", p[i]);
            printf("\n");
        }
        p = nl + 1;
    }

    if (file_buf) kfree(file_buf);
}

void zeus_cmd_wc(ZeusShell* shell, int argc, char* argv[]) {
    (void)shell;
    const char *input = NULL;
    size_t input_len = 0;
    char *file_buf = NULL;
    const char *name = "<stdin>";

    if (argc >= 2) {
        fat32_file_t *f = fat32_open(argv[1], "r");
        if (!f) { printf("wc: cannot open '%s'\n", argv[1]); return; }
        file_buf = (char *)kalloc(f->file_size + 1);
        if (!file_buf) { fat32_close(f); printf("wc: out of memory\n"); return; }
        int rd = fat32_read(f, file_buf, f->file_size);
        fat32_close(f);
        if (rd < 0) { kfree(file_buf); return; }
        file_buf[rd] = '\0';
        input = file_buf;
        input_len = (size_t)rd;
        name = argv[1];
    } else {
        input = zeus_get_pipe_input(&input_len);
        if (!input) { printf("wc: no input\n"); return; }
    }

    uint32_t lines = 0, words = 0, chars = (uint32_t)input_len;
    bool in_word = false;
    for (size_t i = 0; i < input_len; i++) {
        if (input[i] == '\n') lines++;
        if (input[i] == ' ' || input[i] == '\t' || input[i] == '\n' || input[i] == '\r') {
            in_word = false;
        } else if (!in_word) {
            in_word = true;
            words++;
        }
    }
    printf("  %u  %u  %u %s\n", lines, words, chars, name);

    if (file_buf) kfree(file_buf);
}

void zeus_cmd_head(ZeusShell* shell, int argc, char* argv[]) {
    (void)shell;
    int max_lines = 10;
    const char *filename = NULL;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] == 'n' && argv[i][2] == '\0') {
            if (i + 1 < argc) { max_lines = atoi(argv[++i]); }
        } else if (argv[i][0] == '-') {
            max_lines = atoi(argv[i] + 1);
        } else {
            filename = argv[i];
        }
    }

    const char *input = NULL;
    size_t input_len = 0;
    char *file_buf = NULL;

    if (filename) {
        fat32_file_t *f = fat32_open(filename, "r");
        if (!f) { printf("head: cannot open '%s'\n", filename); return; }
        file_buf = (char *)kalloc(f->file_size + 1);
        if (!file_buf) { fat32_close(f); printf("head: out of memory\n"); return; }
        int rd = fat32_read(f, file_buf, f->file_size);
        fat32_close(f);
        if (rd < 0) { kfree(file_buf); return; }
        file_buf[rd] = '\0';
        input = file_buf;
        input_len = (size_t)rd;
    } else {
        input = zeus_get_pipe_input(&input_len);
        if (!input) { printf("head: no input\n"); return; }
    }

    int lines_printed = 0;
    const char *p = input;
    const char *end = input + input_len;
    while (p < end && lines_printed < max_lines) {
        const char *nl = p;
        while (nl < end && *nl != '\n') nl++;
        for (const char *c = p; c < nl; c++) printf("%c", *c);
        printf("\n");
        lines_printed++;
        p = nl + 1;
    }

    if (file_buf) kfree(file_buf);
}

// ============================================================================
// curl - HTTP GET client
// ============================================================================

/* Simple URL parser: extracts host, port, and path from http://host[:port][/path] */
static int curl_parse_url(const char *url, char *host, size_t host_sz,
                          uint16_t *port, char *path, size_t path_sz) {
    *port = 80;
    path[0] = '/'; path[1] = '\0';

    /* Skip scheme */
    const char *p = url;
    if (strncmp(p, "http://", 7) == 0) p += 7;
    else if (strncmp(p, "https://", 8) == 0) {
        printf("curl: HTTPS not supported\n");
        return -1;
    }

    /* Extract host (up to ':', '/', or end) */
    size_t hi = 0;
    while (*p && *p != ':' && *p != '/' && hi + 1 < host_sz)
        host[hi++] = *p++;
    host[hi] = '\0';
    if (hi == 0) return -1;

    /* Optional port */
    if (*p == ':') {
        p++;
        uint16_t pt = 0;
        while (*p >= '0' && *p <= '9') { pt = pt * 10 + (*p - '0'); p++; }
        if (pt > 0) *port = pt;
    }

    /* Path */
    if (*p == '/') {
        size_t pi = 0;
        while (*p && pi + 1 < path_sz) path[pi++] = *p++;
        path[pi] = '\0';
    }

    return 0;
}

/* Try to parse an IPv4 dotted-decimal string into 4 bytes. Returns 0 on success. */
static int curl_parse_ip(const char *str, uint8_t ip[4]) {
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
            return -1;  /* not a valid IP character */
        }
        p++;
    }
    if (parts == 3) { ip[3] = (uint8_t)val; return 0; }
    return -1;
}

void zeus_cmd_curl(ZeusShell* shell, int argc, char* argv[]) {
    (void)shell;

    bool verbose  = false;
    bool head_only = false;
    const char *url = NULL;

    for (int i = 1; i < argc; i++) {
        if (kstrcmp(argv[i], "-v") == 0) verbose = true;
        else if (kstrcmp(argv[i], "-I") == 0) head_only = true;
        else if (kstrcmp(argv[i], "-h") == 0 || kstrcmp(argv[i], "--help") == 0) {
            printf("Usage: curl [-v] [-I] <url>\n");
            printf("  -v   Verbose (show request/response headers)\n");
            printf("  -I   HEAD request (headers only)\n");
            printf("  url  http://host[:port][/path]\n");
            return;
        }
        else url = argv[i];
    }

    if (!url) {
        printf("Usage: curl [-v] [-I] <url>\n");
        return;
    }

    /* Parse URL */
    char host[128], path[256];
    uint16_t port;
    if (curl_parse_url(url, host, sizeof(host), &port, path, sizeof(path)) < 0) {
        printf("curl: invalid URL '%s'\n", url);
        return;
    }

    if (verbose)
        printf("* Connecting to %s port %u ...\n", host, port);

    /* Resolve hostname to IP */
    uint8_t ip[4];
    if (curl_parse_ip(host, ip) != 0) {
        /* Not a dotted-decimal IP -- try DNS */
        if (dns_resolve_a(host, ip) != 0) {
            printf("curl: could not resolve host '%s'\n", host);
            return;
        }
    }

    if (verbose)
        printf("* Resolved to %d.%d.%d.%d\n", ip[0], ip[1], ip[2], ip[3]);

    /* Open TCP connection */
    tcp_connection_t *conn = tcp_connect(ip, port, NULL);
    if (!conn) {
        printf("curl: failed to initiate TCP connection\n");
        return;
    }

    /* Wait for handshake to complete (poll up to 5 seconds).
     * We must drive the NIC receive loop ourselves — PCNet's interrupt handler
     * only clears interrupt flags without calling ethernet_process_frame, so
     * the SYN-ACK would sit in the RX ring forever without explicit polling. */
    {
        extern int net_receive_packet(uint8_t *buffer, uint32_t max_length);
        extern void ethernet_process_frame(const uint8_t *frame, uint32_t length);
        extern uint64_t hpet_get_ticks(void);
        uint8_t _rx[2048];
        uint64_t _hs_start = hpet_get_ticks();
        while (hpet_get_ticks() - _hs_start < 5000) {
            int _n = net_receive_packet(_rx, sizeof(_rx));
            if (_n > 0) ethernet_process_frame(_rx, (uint32_t)_n);
            if (conn->state == TCP_STATE_ESTABLISHED) break;
            if (conn->state == TCP_STATE_CLOSED) {
                printf("curl: connection refused\n");
                return;
            }
            __asm__ volatile("pause");
        }
    }
    if (conn->state != TCP_STATE_ESTABLISHED) {
        printf("curl: connection timed out\n");
        tcp_close(conn);
        return;
    }

    if (verbose) printf("* Connected!\n");

    /* Build HTTP request */
    char req[512];
    int rlen;
    if (head_only)
        rlen = snprintf(req, sizeof(req),
            "HEAD %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: inDevOS-curl/1.0\r\nAccept: */*\r\nConnection: close\r\n\r\n",
            path, host);
    else
        rlen = snprintf(req, sizeof(req),
            "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: inDevOS-curl/1.0\r\nAccept: */*\r\nConnection: close\r\n\r\n",
            path, host);

    if (verbose) {
        printf("> %s %s HTTP/1.0\n", head_only ? "HEAD" : "GET", path);
        printf("> Host: %s\n", host);
    }

    /* Send request */
    int sent = tcp_send(conn, (const uint8_t *)req, (uint32_t)rlen);
    if (sent < 0) {
        printf("curl: failed to send request\n");
        tcp_close(conn);
        return;
    }

    /* Receive response - poll for data (up to 10 seconds) */
    char *resp_buf = (char *)kalloc(32768);  /* 32KB response buffer */
    if (!resp_buf) {
        printf("curl: out of memory\n");
        tcp_close(conn);
        return;
    }

    uint32_t total_recv = 0;

    /* Poll the NIC and drain recv_buffer until the server closes the
     * connection or we hit a 10-second idle timeout. */
    {
        extern int net_receive_packet(uint8_t *buffer, uint32_t max_length);
        extern void ethernet_process_frame(const uint8_t *frame, uint32_t length);
        extern uint64_t hpet_get_ticks(void);
        uint8_t _rx[2048];
        uint64_t _last_data_tick = hpet_get_ticks();

        while (total_recv < 32767) {
            int _n = net_receive_packet(_rx, sizeof(_rx));
            if (_n > 0) ethernet_process_frame(_rx, (uint32_t)_n);

            uint32_t to_copy = tcp_conn_read(conn, (uint8_t *)resp_buf + total_recv, 32767 - total_recv);
            if (to_copy > 0) {
                total_recv += to_copy;
                _last_data_tick = hpet_get_ticks();
            } else {
                if (conn->state == TCP_STATE_CLOSE_WAIT ||
                    conn->state == TCP_STATE_CLOSED ||
                    conn->state == TCP_STATE_TIME_WAIT) {
                    break;
                }
                if (hpet_get_ticks() - _last_data_tick > 10000)
                    break;  /* 10-second idle timeout */
            }
            __asm__ volatile("pause");
        }
    }
    resp_buf[total_recv] = '\0';

    /* Close connection */
    tcp_close(conn);

    if (total_recv == 0) {
        printf("curl: empty response\n");
        kfree(resp_buf);
        return;
    }

    /* Find the end of HTTP headers (\r\n\r\n) */
    char *body = NULL;
    for (uint32_t i = 0; i + 3 < total_recv; i++) {
        if (resp_buf[i] == '\r' && resp_buf[i+1] == '\n' &&
            resp_buf[i+2] == '\r' && resp_buf[i+3] == '\n') {
            resp_buf[i] = '\0';  /* terminate header string */
            body = resp_buf + i + 4;
            break;
        }
    }

    if (verbose || head_only) {
        /* Print response headers */
        printf("%s\n", resp_buf);
        if (head_only) { kfree(resp_buf); return; }
        printf("\n");
    }

    /* Print body */
    if (body && body < resp_buf + total_recv) {
        uint32_t body_len = total_recv - (uint32_t)(body - resp_buf);
        for (uint32_t i = 0; i < body_len; i++)
            printf("%c", body[i]);
        if (body_len > 0 && body[body_len - 1] != '\n')
            printf("\n");
    }

    kfree(resp_buf);
}

// ============================================================================

// Cooperative multitasking test command
void zeus_cmd_threadtest(ZeusShell* shell, int argc, char* argv[]) {
    (void)shell;
    
    int num_tasks = 3;
    int iterations = 10;
    
    // Parse optional arguments
    if (argc >= 2) {
        num_tasks = atoi(argv[1]);
        if (num_tasks < 1) num_tasks = 1;
        if (num_tasks > 8) num_tasks = 8;
    }
    if (argc >= 3) {
        iterations = atoi(argv[2]);
        if (iterations < 1) iterations = 1;
        if (iterations > 50) iterations = 50;
    }
    
    printf("\n==== Cooperative Multitasking Test ====\n");
    printf("Running %d workers, %d iterations each\n", num_tasks, iterations);
    printf("Workers are stepped round-robin every 100ms\n\n");
    
    // Set up test parameters
    task_test_iterations = iterations;
    reset_task_workers();
    
    printf("Waiting for completion (timeout: 30 seconds)...\n");
    printf("Press Ctrl+C to interrupt.\n\n");
    
    // Clear any previous terminate flag
    set_terminate_flag(false);
    
    // Wait for tasks to complete (hpet_get_ticks() returns milliseconds)
    uint64_t start_time = hpet_get_ticks();
    uint64_t timeout_ms = 30000;  // 30 second timeout
    uint64_t last_status = start_time;
    uint64_t last_step = 0;
    int interrupted = 0;
    
    while (1) {
        // Check for Ctrl+C interrupt
        if (get_terminate_flag()) {
            printf("\nInterrupted by Ctrl+C\n");
            interrupted = 1;
            break;
        }
        
        // Step each unfinished worker once per 100ms slot
        uint64_t step_now = hpet_get_ticks();
        if (step_now - last_step >= 100) {
            last_step = step_now;
            for (int i = 0; i < num_tasks; i++) {
                task_test_step(i);
            }
        }
        
        // Check if all tasks completed
        int completed = 0;
        for (int i = 0; i < num_tasks; i++) {
            if (task_test_completed[i]) {
                completed++;
            }
        }
        
        if (completed >= num_tasks) {
            printf("\nAll tasks completed!\n");
            break;
        }
        
        // Print status every 2 seconds
        uint64_t now = hpet_get_ticks();
        if (now - last_status > 2000) {
            printf("Progress: %d/%d tasks complete, counter=%d\n", 
                   completed, num_tasks, task_test_counter);
            last_status = now;
        }
        
        // Check timeout
        if (now - start_time > timeout_ms) {
            printf("\nTest timed out after 30 seconds.\n");
            break;
        }
        
        // Small delay to prevent spinning too fast
        for (volatile int k = 0; k < 10000; k++) {
            __asm__ volatile("pause");
        }
    }
    
    // Clear the terminate flag
    set_terminate_flag(false);
    
    // Results
    printf("\n==== Test Results ====\n");
    printf("Tasks created:    %d\n", num_tasks);
    printf("Iterations/task:  %d\n", iterations);
    printf("Expected counter: %d\n", num_tasks * iterations);
    printf("Actual counter:   %d\n", task_test_counter);
    
    if (interrupted) {
        printf("STATUS: INTERRUPTED by user\n");
        for (int i = 0; i < num_tasks; i++) {
            printf("  Task %d: %s\n", i + 1, 
                   task_test_completed[i] ? "completed" : "interrupted");
        }
    } else {
        int all_completed = 1;
        for (int i = 0; i < num_tasks; i++) {
            if (!task_test_completed[i]) {
                all_completed = 0;
                break;
            }
        }
        
        if (all_completed && task_test_counter == num_tasks * iterations) {
            printf("STATUS: PASSED - All tasks completed correctly!\n");
        } else {
            printf("STATUS: INCOMPLETE\n");
            for (int i = 0; i < num_tasks; i++) {
                printf("  Task %d: %s\n", i + 1, 
                       task_test_completed[i] ? "completed" : "not completed");
            }
        }
    }
    
    printf("Test completed.\n\n");
}

// Memory allocation test command
void zeus_cmd_memtest(ZeusShell* shell, int argc, char* argv[]) {
    (void)shell;
    
    int num_allocations = 10;
    size_t alloc_size = 4096;  // Default 4KB
    
    // Parse optional arguments
    if (argc >= 2) {
        num_allocations = atoi(argv[1]);
        if (num_allocations < 1) num_allocations = 1;
        if (num_allocations > 100) num_allocations = 100;
    }
    if (argc >= 3) {
        alloc_size = (size_t)atoi(argv[2]);
        if (alloc_size < 64) alloc_size = 64;
        if (alloc_size > 1048576) alloc_size = 1048576;  // Max 1MB
    }
    
    printf("\n==== Memory Allocation Test ====\n");
    printf("Performing %d allocations of %zu bytes each\n\n", num_allocations, alloc_size);
    
    void **allocations = kalloc(sizeof(void*) * num_allocations);
    if (!allocations) {
        printf("ERROR: Failed to allocate tracking array\n");
        return;
    }
    
    int successful_allocs = 0;
    int successful_frees = 0;
    
    // Allocate memory
    printf("Phase 1: Allocating memory...\n");
    for (int i = 0; i < num_allocations; i++) {
        allocations[i] = kalloc(alloc_size);
        if (allocations[i]) {
            successful_allocs++;
            // Write pattern to memory to verify it's usable
            memset(allocations[i], (i & 0xFF), alloc_size);
            printf("  [%d] Allocated %zu bytes at 0x%llx\n", i + 1, alloc_size, 
                   (unsigned long long)allocations[i]);
        } else {
            printf("  [%d] FAILED to allocate %zu bytes\n", i + 1, alloc_size);
        }
    }
    
    // Verify memory contents
    printf("\nPhase 2: Verifying memory contents...\n");
    int verify_passed = 0;
    for (int i = 0; i < num_allocations; i++) {
        if (allocations[i]) {
            uint8_t *ptr = (uint8_t*)allocations[i];
            bool valid = true;
            for (size_t j = 0; j < alloc_size; j++) {
                if (ptr[j] != (i & 0xFF)) {
                    valid = false;
                    break;
                }
            }
            if (valid) {
                verify_passed++;
            } else {
                printf("  [%d] VERIFICATION FAILED - memory corruption detected!\n", i + 1);
            }
        }
    }
    printf("  Verified: %d/%d allocations\n", verify_passed, successful_allocs);
    
    // Free memory
    printf("\nPhase 3: Freeing memory...\n");
    for (int i = 0; i < num_allocations; i++) {
        if (allocations[i]) {
            kfree(allocations[i]);
            successful_frees++;
            printf("  [%d] Freed allocation at 0x%llx\n", i + 1, 
                   (unsigned long long)allocations[i]);
        }
    }
    
    kfree(allocations);
    
    // Results
    printf("\n==== Test Results ====\n");
    printf("Requested allocations: %d\n", num_allocations);
    printf("Successful allocations: %d\n", successful_allocs);
    printf("Successful verifications: %d\n", verify_passed);
    printf("Successful frees: %d\n", successful_frees);
    printf("Total memory tested: %zu bytes\n", (size_t)successful_allocs * alloc_size);
    
    if (successful_allocs == num_allocations && 
        verify_passed == successful_allocs && 
        successful_frees == successful_allocs) {
        printf("STATUS: PASSED - All allocations successful!\n");
    } else {
        printf("STATUS: FAILED - Some operations failed\n");
    }
    
    printf("\nMemory test completed.\n\n");
}

