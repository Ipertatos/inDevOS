/*
 * sh.c - Interactive userspace shell for inDevOS
 *
 * Dispatch rule (same as Zeus): special builtins that change shell state
 * run in-process; every other name is resolved on PATH and exec'd.
 *
 * Special builtins: cd, export, unset, exit
 * Regular builtins (used only when PATH has no match): pwd, id, echo, env, help
 */
#include "../include/syscalls.h"

#define SH_MAX_LINE  256
#define SH_MAX_ARGS  32
#define SH_MAX_ENV   16
#define SH_PATH_MAX  256

static char env_k[SH_MAX_ENV][32];
static char env_v[SH_MAX_ENV][96];
static int  env_n;
static int  last_status;
static char default_path[] = "/bin:/apps:/";

static void print_str(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    write(STDOUT_FILENO, s, len);
}

static void print_num(uint32_t n) {
    char buf[12];
    int i = 0;
    if (n == 0) {
        buf[i++] = '0';
    } else {
        char tmp[12];
        int j = 0;
        while (n > 0) {
            tmp[j++] = (char)('0' + (n % 10));
            n /= 10;
        }
        while (j > 0) buf[i++] = tmp[--j];
    }
    buf[i] = '\0';
    print_str(buf);
}

static int str_eq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static int str_len(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void str_copy(char *d, const char *s, int max) {
    int i = 0;
    if (max < 1) return;
    while (s[i] && i < max - 1) {
        d[i] = s[i];
        i++;
    }
    d[i] = '\0';
}

static const char *env_get(const char *key) {
    for (int i = 0; i < env_n; i++) {
        if (str_eq(env_k[i], key)) return env_v[i];
    }
    if (str_eq(key, "PATH")) return default_path;
    return 0;
}

static void env_set(const char *key, const char *val) {
    for (int i = 0; i < env_n; i++) {
        if (str_eq(env_k[i], key)) {
            str_copy(env_v[i], val, (int)sizeof(env_v[i]));
            return;
        }
    }
    if (env_n >= SH_MAX_ENV) return;
    str_copy(env_k[env_n], key, (int)sizeof(env_k[0]));
    str_copy(env_v[env_n], val, (int)sizeof(env_v[0]));
    env_n++;
}

static void env_unset(const char *key) {
    for (int i = 0; i < env_n; i++) {
        if (!str_eq(env_k[i], key)) continue;
        for (int j = i; j < env_n - 1; j++) {
            str_copy(env_k[j], env_k[j + 1], (int)sizeof(env_k[0]));
            str_copy(env_v[j], env_v[j + 1], (int)sizeof(env_v[0]));
        }
        env_n--;
        return;
    }
}

static int read_line(char *buf, int max) {
    int pos = 0;
    while (pos < max - 1) {
        char ch;
        int n = read(STDIN_FILENO, &ch, 1);
        if (n <= 0) break;
        if (ch == '\n' || ch == '\r') {
            write(STDOUT_FILENO, "\n", 1);
            break;
        }
        if (ch == 127 || ch == '\b') {
            if (pos > 0) {
                pos--;
                write(STDOUT_FILENO, "\b \b", 3);
            }
            continue;
        }
        buf[pos++] = ch;
        write(STDOUT_FILENO, &ch, 1);
    }
    buf[pos] = '\0';
    return pos;
}

static int tokenize(char *line, char **argv, int max_args) {
    int argc = 0;
    char *p = line;

    while (*p && argc < max_args - 1) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';
    }
    argv[argc] = (char *)0;
    return argc;
}

static int path_has_slash(const char *s) {
    while (*s) {
        if (*s == '/') return 1;
        s++;
    }
    return 0;
}

/* Try execv on `dir/name` and `dir/name.exe`. Returns 0 on success (never). */
static int try_exec_in_dir(const char *dir, const char *name, char **argv) {
    char path[SH_PATH_MAX];
    int dlen = dir ? str_len(dir) : 0;
    int nlen = str_len(name);
    if (dlen + 1 + nlen + 5 >= SH_PATH_MAX) return -1;

    int i = 0;
    if (dlen == 0 || (dlen == 1 && dir[0] == '/')) {
        path[i++] = '/';
    } else {
        for (int j = 0; j < dlen; j++) path[i++] = dir[j];
        if (path[i - 1] != '/') path[i++] = '/';
    }
    for (int j = 0; j < nlen; j++) path[i++] = name[j];
    path[i] = '\0';

    execv(path, argv);

    path[i++] = '.';
    path[i++] = 'e';
    path[i++] = 'x';
    path[i++] = 'e';
    path[i] = '\0';
    execv(path, argv);
    return -1;
}

static int exec_on_path(char **argv) {
    const char *name = argv[0];
    if (path_has_slash(name)) {
        execv(name, argv);
        try_exec_in_dir("", name, argv);
        return -1;
    }

    const char *path = env_get("PATH");
    if (!path) path = default_path;

    char dir[SH_PATH_MAX];
    int di = 0;
    for (const char *p = path;; p++) {
        if (*p == ':' || *p == '\0') {
            dir[di] = '\0';
            try_exec_in_dir(di ? dir : "/", name, argv);
            di = 0;
            if (*p == '\0') break;
            continue;
        }
        if (di < SH_PATH_MAX - 1) dir[di++] = *p;
    }
    return -1;
}

static void show_prompt(void) {
    char cwd[128];
    if (getcwd(cwd, sizeof(cwd))) {
        print_str("[");
        print_str(getuid() == 0 ? "root" : "user");
        print_str("@inDevOS ");
        print_str(cwd);
        print_str("]");
        print_str(getuid() == 0 ? "# " : "$ ");
    } else {
        print_str("sh> ");
    }
}

static int parse_int(const char *s) {
    int n = 0;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') {
        n = n * 10 + (*s - '0');
        s++;
    }
    return neg ? -n : n;
}

static void cmd_help(void) {
    print_str("inDevOS userspace sh\n");
    print_str("  special builtins: cd, export, unset, exit\n");
    print_str("  builtins:         pwd, id, echo, env, help\n");
    print_str("  other names:      searched on PATH (default /:/apps)\n");
    print_str("  disk programs win over regular builtins.\n");
}

static int run_command(int argc, char **argv) {
    if (argc < 1) return 0;

    /* Special builtins — always in-process (change shell state). */
    if (str_eq(argv[0], "exit")) {
        int code = (argc > 1) ? parse_int(argv[1]) : last_status;
        print_str("Exiting shell.\n");
        exit(code);
    }
    if (str_eq(argv[0], "cd")) {
        const char *dir = (argc > 1) ? argv[1] : "/";
        if (chdir(dir) != 0) {
            print_str("cd: failed to change to ");
            print_str(dir);
            print_str("\n");
            return 1;
        }
        return 0;
    }
    if (str_eq(argv[0], "export")) {
        if (argc < 2) {
            for (int i = 0; i < env_n; i++) {
                print_str(env_k[i]);
                print_str("=");
                print_str(env_v[i]);
                print_str("\n");
            }
            return 0;
        }
        char *eq = argv[1];
        while (*eq && *eq != '=') eq++;
        if (*eq == '=') {
            *eq = '\0';
            env_set(argv[1], eq + 1);
        } else if (argc >= 3) {
            env_set(argv[1], argv[2]);
        } else {
            print_str("Usage: export NAME=value\n");
            return 1;
        }
        return 0;
    }
    if (str_eq(argv[0], "unset")) {
        if (argc < 2) {
            print_str("Usage: unset NAME\n");
            return 1;
        }
        env_unset(argv[1]);
        return 0;
    }

    /* Regular builtins — only if PATH has no executable of this name.
     * We still try PATH first so a disk `echo`/`pwd` wins. */
    pid_t child = fork();
    if (child < 0) {
        /* fork unavailable: fall through to regular builtins / fail */
    } else if (child == 0) {
        exec_on_path(argv);
        /* No PATH hit — regular builtins in the child so the parent wait()s. */
        if (str_eq(argv[0], "pwd")) {
            char cwd[128];
            if (getcwd(cwd, sizeof(cwd))) print_str(cwd);
            else print_str("(unknown)");
            print_str("\n");
            exit(0);
        }
        if (str_eq(argv[0], "id")) {
            print_str("uid=");
            print_num(getuid());
            print_str(" gid=");
            print_num(getgid());
            print_str("\n");
            exit(0);
        }
        if (str_eq(argv[0], "echo")) {
            for (int i = 1; i < argc; i++) {
                if (i > 1) print_str(" ");
                print_str(argv[i]);
            }
            print_str("\n");
            exit(0);
        }
        if (str_eq(argv[0], "env")) {
            print_str("PATH=");
            print_str(env_get("PATH"));
            print_str("\n");
            for (int i = 0; i < env_n; i++) {
                if (str_eq(env_k[i], "PATH")) continue;
                print_str(env_k[i]);
                print_str("=");
                print_str(env_v[i]);
                print_str("\n");
            }
            print_str("?=");
            print_num((uint32_t)last_status);
            print_str("\n");
            exit(0);
        }
        if (str_eq(argv[0], "help")) {
            cmd_help();
            exit(0);
        }
        print_str("sh: not found: ");
        print_str(argv[0]);
        print_str("\n");
        exit(127);
    } else {
        int status = 0;
        wait(&status);
        return status;
    }

    /* fork failed — run regular builtins in-process */
    if (str_eq(argv[0], "pwd") || str_eq(argv[0], "id") ||
        str_eq(argv[0], "echo") || str_eq(argv[0], "env") ||
        str_eq(argv[0], "help")) {
        print_str("sh: fork failed\n");
        return 1;
    }
    print_str("sh: fork failed\n");
    return 1;
}

static int run_line(char *line) {
    char *argv[SH_MAX_ARGS];
    int argc = tokenize(line, argv, SH_MAX_ARGS);
    if (argc == 0) return last_status;
    last_status = run_command(argc, argv);
    return last_status;
}

int main(int argc, char **argv) {
    env_set("PATH", default_path);

    if (argc >= 3 && str_eq(argv[1], "-c")) {
        char buf[SH_MAX_LINE];
        str_copy(buf, argv[2], SH_MAX_LINE);
        return run_line(buf);
    }

    print_str("inDevOS userspace shell (PID ");
    print_num((uint32_t)getpid());
    print_str(", UID ");
    print_num(getuid());
    print_str(")\n");
    print_str("Type 'help' or 'exit'. Disk programs on PATH win.\n\n");

    char line[SH_MAX_LINE];
    for (;;) {
        show_prompt();
        int len = read_line(line, sizeof(line));
        if (len == 0) continue;
        run_line(line);
    }
}
