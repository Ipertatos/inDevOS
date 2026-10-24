/**
 * launch.c - desktop launch service and file associations.
 *
 * Association file /etc/indevos/open-with (LF, '#' comments), one rule per
 * line:
 *     dir -> files
 *     ext txt -> notepad
 *     ext exe -> run
 *     ext wad -> doom
 *     ext png -> toast Image viewer is not available yet
 *     ext foo -> /bin/prog.exe --flag %f        (%f = file, %d = its folder)
 * Actions: run | notepad | files | lua | doom | toast <text> | <program> args.
 * Programs start with the file's folder as working directory.  When the file
 * is missing a built-in default table (same content) is used.
 */
#include "launch.h"
#include "desktop.h"
#include "exec.h"
#include "vfs.h"
#include "mem.h"
#include "serial.h"
#include "string.h"
#include "notepad.h"
#include "files.h"
#include "lua_runtime.h"
#include "kterm.h"

extern int snprintf(char *str, size_t size, const char *fmt, ...);
extern bool process_pid_alive(uint32_t pid);

#define LAUNCH_CONF   "/etc/indevos/open-with"
#define LAUNCH_RULES  48
#define LAUNCH_ACT    96

static const char launch_default_conf[] =
    "# inDevOS file associations: ext <extension> -> <action>\n"
    "dir -> files\n"
    "ext exe -> run\n"
    "ext elf -> run\n"
    "ext txt -> notepad\n"
    "ext md -> notepad\n"
    "ext c -> notepad\n"
    "ext h -> notepad\n"
    "ext sh -> notepad\n"
    "ext cfg -> notepad\n"
    "ext conf -> notepad\n"
    "ext log -> notepad\n"
    "ext ini -> notepad\n"
    "ext lua -> lua\n"
    "ext wad -> doom\n"
    "ext png -> toast Image viewer is not available yet\n"
    "ext bmp -> toast Image viewer is not available yet\n"
    "ext jpg -> toast Image viewer is not available yet\n";

typedef struct { char ext[16]; char act[LAUNCH_ACT]; } launch_rule_t;
static launch_rule_t launch_rules[LAUNCH_RULES];
static int  launch_nrules;
static bool launch_loaded;

#define LAUNCH_TRACK 16
static int launch_pids[LAUNCH_TRACK];

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static bool streq_ci(const char *a, const char *b) {
    while (*a && *b) { if (lc(*a) != lc(*b)) return false; a++; b++; }
    return *a == *b;
}

static void launch_parse(const char *s, int n) {
    launch_nrules = 0;
    int i = 0;
    while (i < n && launch_nrules < LAUNCH_RULES) {
        char line[160];
        int l = 0;
        while (i < n && s[i] != '\n') {
            if (s[i] != '\r' && l < (int)sizeof(line) - 1) line[l++] = s[i];
            i++;
        }
        i++;
        line[l] = '\0';
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#') continue;
        char *arrow = strstr(p, "->");
        if (!arrow) continue;
        char *act = arrow + 2;
        while (*act == ' ' || *act == '\t') act++;
        char *e = arrow;
        while (e > p && (e[-1] == ' ' || e[-1] == '\t')) e--;
        *e = '\0';
        launch_rule_t *r = &launch_rules[launch_nrules];
        if (streq_ci(p, "dir")) {
            kstrncpy(r->ext, "/", sizeof(r->ext));
        } else if ((p[0] == 'e' || p[0] == 'E') && (p[1] == 'x' || p[1] == 'X') &&
                   (p[2] == 't' || p[2] == 'T') && p[3] == ' ') {
            p += 4;
            while (*p == ' ') p++;
            if (*p == '.') p++;
            kstrncpy(r->ext, p, sizeof(r->ext));
        } else {
            continue;
        }
        kstrncpy(r->act, act, sizeof(r->act));
        int al = (int)strlen(r->act);
        while (al > 0 && (r->act[al - 1] == ' ' || r->act[al - 1] == '\t'))
            r->act[--al] = '\0';
        launch_nrules++;
    }
}

static void launch_load(void) {
    if (launch_loaded) return;
    launch_loaded = true;
    void *buf = NULL;
    int n = vfs_read_file_alloc(LAUNCH_CONF, &buf, 16384);
    if (n > 0 && buf) {
        launch_parse((const char *)buf, n);
        kfree(buf);
        serial_printf("[launch] %d rules from %s\n", launch_nrules, LAUNCH_CONF);
    }
    if (launch_nrules == 0) {
        if (buf && n <= 0) kfree(buf);
        launch_parse(launch_default_conf, (int)sizeof(launch_default_conf) - 1);
        serial_printf("[launch] %d built-in rules (%s missing)\n", launch_nrules, LAUNCH_CONF);
    }
}

void launch_reload(void) { launch_loaded = false; }

static const char *launch_ext(const char *name) {
    const char *dot = NULL;
    for (const char *p = name; *p; p++) {
        if (*p == '/') dot = NULL;
        else if (*p == '.') dot = p;
    }
    return (dot && dot != name) ? dot + 1 : "";
}

static const char *launch_basename(const char *path) {
    const char *b = path;
    for (const char *p = path; *p; p++) if (*p == '/') b = p + 1;
    return b;
}

static const char *launch_find(const char *path, bool is_dir) {
    launch_load();
    const char *ext = is_dir ? "/" : launch_ext(launch_basename(path));
    if (!*ext) return NULL;
    for (int i = 0; i < launch_nrules; i++)
        if (streq_ci(launch_rules[i].ext, ext)) return launch_rules[i].act;
    return NULL;
}

int launch_icon_for(const char *name, bool is_dir) {
    if (is_dir) return LAUNCH_ICON_FOLDER;
    const char *a = launch_find(name, false);
    if (!a) return LAUNCH_ICON_GENERIC;
    if (streq_ci(a, "run")) return LAUNCH_ICON_EXE;
    if (streq_ci(a, "notepad")) return LAUNCH_ICON_TEXT;
    if (streq_ci(a, "lua")) return LAUNCH_ICON_LUA;
    if (streq_ci(a, "doom")) return LAUNCH_ICON_WAD;
    const char *ext = launch_ext(launch_basename(name));
    if (streq_ci(ext, "png") || streq_ci(ext, "bmp") || streq_ci(ext, "jpg") ||
        streq_ci(ext, "jpeg") || streq_ci(ext, "gif"))
        return LAUNCH_ICON_IMAGE;
    return LAUNCH_ICON_GENERIC;
}

static void launch_track(int pid) {
    for (int i = 0; i < LAUNCH_TRACK; i++) {
        if (launch_pids[i] > 0 && !process_pid_alive((uint32_t)launch_pids[i]))
            launch_pids[i] = 0;
    }
    for (int i = 0; i < LAUNCH_TRACK; i++)
        if (launch_pids[i] <= 0) { launch_pids[i] = pid; return; }
}

int launch_running_count(void) {
    int n = 0;
    for (int i = 0; i < LAUNCH_TRACK; i++) {
        if (launch_pids[i] > 0 && process_pid_alive((uint32_t)launch_pids[i])) n++;
        else launch_pids[i] = 0;
    }
    return n;
}

static void launch_dir_of(const char *path, char *out, int n) {
    kstrncpy(out, path, (size_t)n);
    int cut = -1;
    for (int i = 0; out[i]; i++) if (out[i] == '/') cut = i;
    if (cut <= 0) { out[0] = '/'; out[1] = '\0'; }
    else out[cut] = '\0';
}

static int launch_spawn(const char *prog, char *const argv[], const char *cwd,
                        const char *const envp[]) {
    int pid = exec_spawn_ex(prog, argv, cwd, envp);
    serial_printf("[launch] spawn %s cwd=%s -> pid %d\n", prog, cwd ? cwd : "-", pid);
    if (pid > 0) launch_track(pid);
    return pid;
}

static void launch_fail(const char *what, const char *name) {
    char msg[96];
    snprintf(msg, sizeof(msg), "%s %s", what, name);
    desktop_toast(msg);
}

/* ---- .desktop shortcuts ------------------------------------------------
 * A small subset of the freedesktop format, LF text:
 *     [Desktop Entry]
 *     Name=Programs
 *     Exec=files /bin
 *     Icon=folder
 * Exec is a program path (or bare name looked up in /bin) with arguments, a
 * file or folder to open through the associations, or one of the built-in
 * words: files [dir], notepad <file>, terminal [dir],
 * terminal-run <command line> (typed into a terminal window's shell). */
typedef struct {
    char name[64];
    char exec[192];
    char icon[32];
} launch_shortcut_t;

static bool launch_is_shortcut(const char *path) {
    const char *e = launch_ext(launch_basename(path));
    return e && streq_ci(e, "desktop");
}

static bool launch_read_shortcut(const char *path, launch_shortcut_t *s) {
    s->name[0] = s->exec[0] = s->icon[0] = '\0';
    void *buf = NULL;
    int n = vfs_read_file_alloc(path, &buf, 4096);
    if (n <= 0 || !buf) { if (buf) kfree(buf); return false; }
    const char *p = (const char *)buf, *end = p + n;
    while (p < end) {
        const char *ls = p;
        while (p < end && *p != '\n') p++;
        const char *le = p;
        if (p < end) p++;
        while (ls < le && (*ls == ' ' || *ls == '\t')) ls++;
        while (le > ls && (le[-1] == '\r' || le[-1] == ' ' || le[-1] == '\t')) le--;
        if (ls >= le || *ls == '#' || *ls == '[') continue;
        const char *eq = ls;
        while (eq < le && *eq != '=') eq++;
        if (eq >= le) continue;
        int kl = (int)(eq - ls);
        char key[16];
        if (kl <= 0 || kl >= (int)sizeof(key)) continue;
        for (int i = 0; i < kl; i++) key[i] = ls[i];
        key[kl] = '\0';
        char *dst = NULL; int cap = 0;
        if (streq_ci(key, "Name"))      { dst = s->name; cap = sizeof(s->name); }
        else if (streq_ci(key, "Exec")) { dst = s->exec; cap = sizeof(s->exec); }
        else if (streq_ci(key, "Icon")) { dst = s->icon; cap = sizeof(s->icon); }
        if (!dst) continue;
        const char *v = eq + 1;
        int vl = (int)(le - v);
        if (vl >= cap) vl = cap - 1;
        for (int i = 0; i < vl; i++) dst[i] = v[i];
        dst[vl] = '\0';
    }
    kfree(buf);
    return s->exec[0] != '\0';
}

static int launch_icon_word(const char *w) {
    if (!w || !w[0]) return LAUNCH_ICON_EXE;
    if (streq_ci(w, "folder") || streq_ci(w, "directory")) return LAUNCH_ICON_FOLDER;
    if (streq_ci(w, "text") || streq_ci(w, "notepad"))     return LAUNCH_ICON_TEXT;
    if (streq_ci(w, "exe") || streq_ci(w, "program") ||
        streq_ci(w, "application") || streq_ci(w, "terminal")) return LAUNCH_ICON_EXE;
    if (streq_ci(w, "image"))                              return LAUNCH_ICON_IMAGE;
    if (streq_ci(w, "lua"))                                return LAUNCH_ICON_LUA;
    if (streq_ci(w, "wad") || streq_ci(w, "doom"))         return LAUNCH_ICON_WAD;
    return LAUNCH_ICON_GENERIC;
}

int launch_shortcut_info(const char *path, char *label, int n, int *icon) {
    launch_shortcut_t s;
    if (!launch_is_shortcut(path) || !launch_read_shortcut(path, &s)) return -1;
    if (label && n > 0) {
        const char *src = s.name;
        char tmp[64];
        if (!src[0]) {           /* basename without ".desktop" */
            const char *b = launch_basename(path);
            int l = 0;
            while (b[l] && l < (int)sizeof(tmp) - 1) { tmp[l] = b[l]; l++; }
            if (l > 8) l -= 8;
            tmp[l] = '\0';
            src = tmp;
        }
        int l = 0;
        while (src[l] && l < n - 1) { label[l] = src[l]; l++; }
        label[l] = '\0';
    }
    if (icon) *icon = launch_icon_word(s.icon);
    return 0;
}

static bool launch_is_dir(const char *path) {
    vfs_dirent_t d[2];
    return vfs_readdir(path, d, 2) >= 0;
}

static bool launch_is_elf(const char *path) {
    uint8_t h[4];
    int fd = vfs_open(path, VFS_O_READ);
    if (fd < 0) return false;
    int n = vfs_read(fd, h, 4);
    vfs_close(fd);
    return n == 4 && h[0] == 0x7F && h[1] == 'E' && h[2] == 'L' && h[3] == 'F';
}

/* Split a command line in place (spaces; "double quotes" group). */
static int launch_split(char *q, char **argv, int max) {
    int argc = 0;
    while (*q && argc < max - 1) {
        while (*q == ' ' || *q == '\t') q++;
        if (!*q) break;
        if (*q == '"') {
            argv[argc++] = ++q;
            while (*q && *q != '"') q++;
        } else {
            argv[argc++] = q;
            while (*q && *q != ' ' && *q != '\t') q++;
        }
        if (*q) *q++ = '\0';
    }
    argv[argc] = NULL;
    return argc;
}

static int launch_shortcut_run(const char *path) {
    launch_shortcut_t s;
    const char *name = launch_basename(path);
    if (!launch_read_shortcut(path, &s)) { launch_fail("Bad shortcut", name); return -1; }
    char dir[256];
    launch_dir_of(path, dir, sizeof(dir));
    static char cmd[192];
    int l = 0;
    while (s.exec[l] && l < (int)sizeof(cmd) - 1) { cmd[l] = s.exec[l]; l++; }
    cmd[l] = '\0';
    serial_printf("[launch] shortcut %s: Name='%s' Exec='%s' Icon='%s'\n",
                  path, s.name, s.exec, s.icon);
    /* Exec=terminal-run <shell command line>: a terminal window runs it. */
    {
        static const char tr[] = "terminal-run ";
        int k = 0;
        while (tr[k] && cmd[k] == tr[k]) k++;
        if (!tr[k]) {
            while (cmd[k] == ' ') k++;
            {
                /* Live CD: /bin/links lives on /cdrom when / is the HDD. */
                static char rewritten[192];
                const char *src = cmd + k;
                if (!vfs_exists("/bin/links") && vfs_exists("/cdrom/bin/links") &&
                    (src[0] == 'H' || src[0] == '/' || (src[0] == 'S' && src[1] == 'S'))) {
                    /* Prefer a known-good live command line. */
                    snprintf(rewritten, sizeof(rewritten),
                        "HOME=/tmp SSL_CERT_FILE=/cdrom/etc/ssl/cert.pem "
                        "/cdrom/bin/links /cdrom/etc/links/start.html");
                    src = rewritten;
                } else if (!vfs_exists("/bin/links") && vfs_exists("/cdrom/bin/links")) {
                    snprintf(rewritten, sizeof(rewritten), "%s", src);
                    /* best-effort already in Exec= */
                    src = rewritten;
                }
                kterm_run_in("/tmp", src);
            }
            return 0;
        }
    }
    char *argv[12];
    int argc = launch_split(cmd, argv, 12);
    if (argc == 0) { launch_fail("Bad shortcut", name); return -1; }
    if (streq_ci(argv[0], "files")) {
        files_open_path(argc > 1 ? argv[1] : dir);
        return 0;
    }
    if (streq_ci(argv[0], "notepad") && argc > 1) {
        notepad_open_path(argv[1]);
        return 0;
    }
    if (streq_ci(argv[0], "terminal")) {
        kterm_open_in(argc > 1 ? argv[1] : dir);
        return 0;
    }
    char prog[160];
    if (argv[0][0] == '/') {
        snprintf(prog, sizeof(prog), "%s", argv[0]);
    } else {
        snprintf(prog, sizeof(prog), "/bin/%s.exe", argv[0]);
        if (!vfs_exists(prog)) snprintf(prog, sizeof(prog), "/bin/%s", argv[0]);
    }
    if (!vfs_exists(prog)) {
        char alt[160];
        if (argv[0][0] == '/')
            snprintf(alt, sizeof(alt), "/cdrom%s", argv[0]);
        else
            snprintf(alt, sizeof(alt), "/cdrom/bin/%s", argv[0]);
        if (vfs_exists(alt)) {
            snprintf(prog, sizeof(prog), "%s", alt);
        } else {
            launch_fail("Shortcut target missing:", argv[0]);
            return -1;
        }
    }
    if (argc == 1 && !launch_is_elf(prog)) {
        if (launch_is_shortcut(prog)) { launch_fail("Shortcut points to a shortcut", name); return -1; }
        return launch_open_path(prog, launch_is_dir(prog));
    }
    char pdir[256];
    launch_dir_of(prog, pdir, sizeof(pdir));
    int pid = launch_spawn(prog, argv, pdir, NULL);
    if (pid <= 0) launch_fail("Could not run", name);
    return pid;
}

int launch_open_path(const char *path, bool is_dir) {
    if (!path || !path[0]) return -1;
    static char cdrom_path[256];
    if (!vfs_exists(path)) {
        const char *base = launch_basename(path);
        snprintf(cdrom_path, sizeof(cdrom_path), "/cdrom/home/user/Desktop/%s", base);
        if (vfs_exists(cdrom_path)) path = cdrom_path;
        else {
            snprintf(cdrom_path, sizeof(cdrom_path), "/cdrom%s", path);
            if (vfs_exists(cdrom_path)) path = cdrom_path;
        }
    }
    const char *name = launch_basename(path);
    if (!is_dir && launch_is_shortcut(path)) return launch_shortcut_run(path);
    const char *act = launch_find(path, is_dir);
    char dir[256];
    launch_dir_of(path, dir, sizeof(dir));

    if (!act && !is_dir) {
        /* No rule: sniff ELF magic, else treat printable text as text. */
        uint8_t head[64];
        int fd = vfs_open(path, VFS_O_READ);
        int n = fd >= 0 ? vfs_read(fd, head, sizeof(head)) : -1;
        if (fd >= 0) vfs_close(fd);
        if (n >= 4 && head[0] == 0x7F && head[1] == 'E' && head[2] == 'L' && head[3] == 'F')
            act = "run";
        else if (n >= 0) {
            bool text = true;
            for (int i = 0; i < n; i++)
                if (head[i] < 9 || (head[i] > 13 && head[i] < 32)) { text = false; break; }
            act = text ? "notepad" : NULL;
        }
    }
    if (!act) act = is_dir ? "files" : NULL;
    serial_printf("[launch] open %s%s -> %s\n", path, is_dir ? " (dir)" : "",
                  act ? act : "(none)");
    if (!act) { launch_fail("No app to open", name); return -1; }

    if (streq_ci(act, "files")) {
        files_open_path(is_dir ? path : dir);
        return 0;
    }
    if (streq_ci(act, "notepad")) {
        notepad_open_path(path);
        return 0;
    }
    if (streq_ci(act, "run")) {
        char *argv[2] = { (char *)name, NULL };
        int pid = launch_spawn(path, argv, dir, NULL);
        if (pid <= 0) launch_fail("Could not run", name);
        return pid;
    }
    if (streq_ci(act, "lua")) {
        void *buf = NULL;
        int n = vfs_read_file_alloc(path, &buf, 1u << 20);
        if (n <= 0 || !buf) { launch_fail("Could not read", name); return -1; }
        int rc = lua_run_script_from_buffer(path, buf, (uint32_t)n);
        kfree(buf);
        char msg[96];
        snprintf(msg, sizeof(msg), "%s finished (rc %d)", name, rc);
        desktop_toast(msg);
        serial_printf("[launch] lua %s rc=%d\n", path, rc);
        return 0;
    }
    if (streq_ci(act, "doom")) {
        /* Doom finds an IWAD by name in its folder list ("." included); other
         * WADs are PWADs and go through -file. */
        static const char *const iwads[] = {
            "doom1.wad", "doom.wad", "doomu.wad", "doom2.wad", "doom2f.wad",
            "plutonia.wad", "tnt.wad", NULL };
        bool iwad = false;
        for (int i = 0; iwads[i]; i++) if (streq_ci(name, iwads[i])) iwad = true;
        static char envwad[300];
        snprintf(envwad, sizeof(envwad), "DOOMWADDIR=%s", dir);
        const char *envp[] = { envwad, NULL };
        char *argv_i[2] = { "doom", NULL };
        char *argv_p[4] = { "doom", "-file", (char *)path, NULL };
        int pid = launch_spawn("/bin/doom.exe", iwad ? argv_i : argv_p, dir, envp);
        if (pid <= 0) launch_fail("Could not start Doom for", name);
        return pid;
    }
    if (act[0] == 't' && act[1] == 'o' && act[2] == 'a' && act[3] == 's' &&
        act[4] == 't' && (act[5] == ' ' || !act[5])) {
        desktop_toast(act[5] ? act + 6 : name);
        return 0;
    }

    /* Generic command line with %f / %d substitution. */
    static char cmd[LAUNCH_ACT + 512];
    int o = 0;
    for (const char *p = act; *p && o < (int)sizeof(cmd) - 1; p++) {
        if (p[0] == '%' && (p[1] == 'f' || p[1] == 'd')) {
            const char *sub = p[1] == 'f' ? path : dir;
            while (*sub && o < (int)sizeof(cmd) - 1) cmd[o++] = *sub++;
            p++;
        } else {
            cmd[o++] = *p;
        }
    }
    cmd[o] = '\0';
    char *argv[12];
    int argc = 0;
    char *q = cmd;
    while (*q && argc < 11) {
        while (*q == ' ') *q++ = '\0';
        if (!*q) break;
        argv[argc++] = q;
        while (*q && *q != ' ') q++;
    }
    argv[argc] = NULL;
    if (argc == 0) { launch_fail("Bad association for", name); return -1; }
    int pid = launch_spawn(argv[0], argv, dir, NULL);
    if (pid <= 0) launch_fail("Could not open", name);
    return pid;
}
