/**
 * @file users.c
 * @brief User accounts, groups, and file permissions implementation
 */

#include "users.h"
#include "vfs.h"
#include "mem.h"
#include "string.h"
#include "serial.h"

extern int snprintf(char *str, size_t size, const char *fmt, ...);
extern int printf(const char *fmt, ...);
extern void *memset(void *, int, size_t);
extern void *memcpy(void *, const void *, size_t);
extern int strcmp(const char *a, const char *b);
extern size_t strlen(const char *s);
extern char *strcpy(char *dst, const char *src);
extern char *strncpy(char *dst, const char *src, size_t n);
extern int atoi(const char *s);
extern int strncmp(const char *a, const char *b, size_t n);

/* Forward declarations */
static int user_count(void);
static int group_count(void);

/* ================================================================
 *  Internal state
 * ================================================================ */

static user_t       users[MAX_USERS];
static group_t      groups[MAX_GROUPS];
static perm_entry_t perms[MAX_PERM_ENTRIES];

static uint32_t current_uid = UID_ROOT;   /* boot as root */
static uint32_t current_gid = GID_ROOT;
static bool     logged_in   = true;        /* root is implicit at boot */

/* ================================================================
 *  Utility
 * ================================================================ */

uint32_t password_hash(const char *password) {
    /* DJB2 hash – simple but sufficient for a hobby OS */
    uint32_t hash = 5381;
    while (*password) {
        hash = ((hash << 5) + hash) + (uint8_t)*password++;
    }
    return hash;
}

void perm_mode_to_string(uint16_t mode, char *buf) {
    buf[0] = (mode & PERM_UR) ? 'r' : '-';
    buf[1] = (mode & PERM_UW) ? 'w' : '-';
    buf[2] = (mode & PERM_UX) ? 'x' : '-';
    buf[3] = (mode & PERM_GR) ? 'r' : '-';
    buf[4] = (mode & PERM_GW) ? 'w' : '-';
    buf[5] = (mode & PERM_GX) ? 'x' : '-';
    buf[6] = (mode & PERM_OR) ? 'r' : '-';
    buf[7] = (mode & PERM_OW) ? 'w' : '-';
    buf[8] = (mode & PERM_OX) ? 'x' : '-';
    buf[9] = '\0';
}

static uint32_t next_uid(void) {
    uint32_t max_uid = 0;
    for (int i = 0; i < MAX_USERS; i++) {
        if (users[i].active && users[i].uid >= max_uid)
            max_uid = users[i].uid + 1;
    }
    return max_uid;
}

static uint32_t next_gid(void) {
    uint32_t max_gid = 0;
    for (int i = 0; i < MAX_GROUPS; i++) {
        if (groups[i].active && groups[i].gid >= max_gid)
            max_gid = groups[i].gid + 1;
    }
    return max_gid;
}

/* ================================================================
 *  Initialisation
 * ================================================================ */

void users_init(void) {
    memset(users, 0, sizeof(users));
    memset(groups, 0, sizeof(groups));
    memset(perms, 0, sizeof(perms));

    /* Try to load from disk */
    if (users_load() != 0) {
        /* Create default root user and root group */
        group_add("root", GID_ROOT);
        group_add("users", 1);
        user_add("root", "root", UID_ROOT, GID_ROOT, "/home/user", "zeus");

        /* Create a normal "user" account */
        user_add("user", "user", 1, 1, "/home/user", "zeus");
        group_add_member("users", 1);
    }
    perms_load();   /* load permissions overlay (ignore failure) */

    current_uid = UID_ROOT;
    current_gid = GID_ROOT;
    logged_in   = true;

    serial_printf("users: initialized (%d users, %d groups)\n",
                  user_count(), group_count());
}

static int user_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_USERS; i++)
        if (users[i].active) n++;
    return n;
}

static int group_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_GROUPS; i++)
        if (groups[i].active) n++;
    return n;
}

/* ================================================================
 *  User management
 * ================================================================ */

int user_add(const char *username, const char *password,
             uint32_t uid, uint32_t gid,
             const char *home, const char *shell) {
    if (!username || !password) return -1;
    if (user_find_by_name(username)) {
        printf("users: user '%s' already exists\n", username);
        return -1;
    }
    if (user_find_by_uid(uid)) {
        printf("users: uid %u already in use\n", uid);
        return -1;
    }

    for (int i = 0; i < MAX_USERS; i++) {
        if (!users[i].active) {
            users[i].active = true;
            users[i].uid    = uid;
            users[i].gid    = gid;
            users[i].password_hash = password_hash(password);
            strncpy(users[i].username, username, MAX_USERNAME_LEN - 1);
            strncpy(users[i].home, home ? home : "/", MAX_HOME_LEN - 1);
            strncpy(users[i].shell, shell ? shell : "zeus", MAX_SHELL_LEN - 1);
            return 0;
        }
    }
    printf("users: user table full\n");
    return -1;
}

int user_remove(const char *username) {
    if (!username) return -1;
    if (strcmp(username, "root") == 0) {
        printf("users: cannot remove root\n");
        return -1;
    }
    user_t *u = user_find_by_name(username);
    if (!u) {
        printf("users: user '%s' not found\n", username);
        return -1;
    }
    u->active = false;
    return 0;
}

user_t *user_find_by_name(const char *username) {
    if (!username) return NULL;
    for (int i = 0; i < MAX_USERS; i++) {
        if (users[i].active && strcmp(users[i].username, username) == 0)
            return &users[i];
    }
    return NULL;
}

user_t *user_find_by_uid(uint32_t uid) {
    for (int i = 0; i < MAX_USERS; i++) {
        if (users[i].active && users[i].uid == uid)
            return &users[i];
    }
    return NULL;
}

int user_set_password(const char *username, const char *password) {
    user_t *u = user_find_by_name(username);
    if (!u) return -1;
    u->password_hash = password_hash(password);
    return 0;
}

bool user_check_password(const char *username, const char *password) {
    user_t *u = user_find_by_name(username);
    if (!u) return false;
    return u->password_hash == password_hash(password);
}

int user_list(void) {
    printf("%-16s UID   GID   Home              Shell\n", "Username");
    printf("--------------------------------------------------------------\n");
    int n = 0;
    for (int i = 0; i < MAX_USERS; i++) {
        if (users[i].active) {
            printf("%-16s %-5u %-5u %-18s %s\n",
                   users[i].username,
                   users[i].uid,
                   users[i].gid,
                   users[i].home,
                   users[i].shell);
            n++;
        }
    }
    printf("Total: %d user(s)\n", n);
    return n;
}

/* ================================================================
 *  Group management
 * ================================================================ */

int group_add(const char *name, uint32_t gid) {
    if (!name) return -1;
    if (group_find_by_name(name)) {
        printf("users: group '%s' already exists\n", name);
        return -1;
    }
    for (int i = 0; i < MAX_GROUPS; i++) {
        if (!groups[i].active) {
            groups[i].active = true;
            groups[i].gid    = gid;
            groups[i].member_count = 0;
            strncpy(groups[i].name, name, MAX_GROUP_NAME_LEN - 1);
            return 0;
        }
    }
    printf("users: group table full\n");
    return -1;
}

int group_remove(const char *name) {
    if (!name) return -1;
    if (strcmp(name, "root") == 0) {
        printf("users: cannot remove root group\n");
        return -1;
    }
    group_t *g = group_find_by_name(name);
    if (!g) return -1;
    g->active = false;
    return 0;
}

group_t *group_find_by_name(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < MAX_GROUPS; i++) {
        if (groups[i].active && strcmp(groups[i].name, name) == 0)
            return &groups[i];
    }
    return NULL;
}

group_t *group_find_by_gid(uint32_t gid) {
    for (int i = 0; i < MAX_GROUPS; i++) {
        if (groups[i].active && groups[i].gid == gid)
            return &groups[i];
    }
    return NULL;
}

int group_add_member(const char *group_name, uint32_t uid) {
    group_t *g = group_find_by_name(group_name);
    if (!g) return -1;
    /* Check if already a member */
    for (uint32_t i = 0; i < g->member_count; i++) {
        if (g->members[i] == uid) return 0;
    }
    if (g->member_count >= MAX_GROUP_MEMBERS) return -1;
    g->members[g->member_count++] = uid;
    return 0;
}

int group_remove_member(const char *group_name, uint32_t uid) {
    group_t *g = group_find_by_name(group_name);
    if (!g) return -1;
    for (uint32_t i = 0; i < g->member_count; i++) {
        if (g->members[i] == uid) {
            /* Shift remaining members */
            for (uint32_t j = i; j < g->member_count - 1; j++)
                g->members[j] = g->members[j + 1];
            g->member_count--;
            return 0;
        }
    }
    return -1;
}

bool group_is_member(uint32_t gid, uint32_t uid) {
    group_t *g = group_find_by_gid(gid);
    if (!g) return false;
    for (uint32_t i = 0; i < g->member_count; i++) {
        if (g->members[i] == uid) return true;
    }
    return false;
}

int group_list(void) {
    printf("%-16s GID   Members\n", "Group");
    printf("----------------------------------------------\n");
    int n = 0;
    for (int i = 0; i < MAX_GROUPS; i++) {
        if (groups[i].active) {
            printf("%-16s %-5u ", groups[i].name, groups[i].gid);
            for (uint32_t j = 0; j < groups[i].member_count; j++) {
                user_t *u = user_find_by_uid(groups[i].members[j]);
                if (u) printf("%s ", u->username);
                else   printf("%u ", groups[i].members[j]);
            }
            printf("\n");
            n++;
        }
    }
    printf("Total: %d group(s)\n", n);
    return n;
}

/* ================================================================
 *  Session / current user
 * ================================================================ */

int user_login(const char *username, const char *password) {
    if (!username || !password) return -1;
    if (!user_check_password(username, password)) {
        printf("Login incorrect\n");
        return -1;
    }
    user_t *u = user_find_by_name(username);
    current_uid = u->uid;
    current_gid = u->gid;
    logged_in   = true;
    printf("Welcome, %s\n", username);
    return 0;
}

void user_logout(void) {
    /* Return to root (single-user fallback) */
    current_uid = UID_ROOT;
    current_gid = GID_ROOT;
    logged_in   = true;
    printf("Logged out\n");
}

uint32_t user_current_uid(void) { return current_uid; }
uint32_t user_current_gid(void) { return current_gid; }

const char *user_current_name(void) {
    user_t *u = user_find_by_uid(current_uid);
    return u ? u->username : "unknown";
}

bool user_is_root(void) { return current_uid == UID_ROOT; }

/* ================================================================
 *  File permission overlay
 * ================================================================ */

static perm_entry_t *perm_find(const char *path) {
    if (!path) return NULL;
    for (int i = 0; i < MAX_PERM_ENTRIES; i++) {
        if (perms[i].active && strcmp(perms[i].path, path) == 0)
            return &perms[i];
    }
    return NULL;
}

int perm_set(const char *path, uint32_t uid, uint32_t gid, uint16_t mode) {
    if (!path) return -1;
    perm_entry_t *e = perm_find(path);
    if (e) {
        e->uid  = uid;
        e->gid  = gid;
        e->mode = mode;
        return 0;
    }
    /* Allocate new entry */
    for (int i = 0; i < MAX_PERM_ENTRIES; i++) {
        if (!perms[i].active) {
            perms[i].active = true;
            perms[i].uid    = uid;
            perms[i].gid    = gid;
            perms[i].mode   = mode;
            strncpy(perms[i].path, path, sizeof(perms[i].path) - 1);
            perms[i].path[sizeof(perms[i].path) - 1] = '\0';
            return 0;
        }
    }
    printf("perm: permission table full\n");
    return -1;
}

int perm_get(const char *path, uint32_t *uid, uint32_t *gid, uint16_t *mode) {
    perm_entry_t *e = perm_find(path);
    if (!e) {
        /* Default: owned by root, 644 for files */
        if (uid)  *uid  = UID_ROOT;
        if (gid)  *gid  = GID_ROOT;
        if (mode) *mode = PERM_DEFAULT_FILE;
        return -1;   /* not found, defaults used */
    }
    if (uid)  *uid  = e->uid;
    if (gid)  *gid  = e->gid;
    if (mode) *mode = e->mode;
    return 0;
}

int perm_remove(const char *path) {
    perm_entry_t *e = perm_find(path);
    if (!e) return -1;
    e->active = false;
    return 0;
}

bool perm_check_access(const char *path, uint32_t uid, uint32_t gid, int mode) {
    /* Root can do anything */
    if (uid == UID_ROOT) return true;

    uint32_t file_uid, file_gid;
    uint16_t file_mode;
    perm_get(path, &file_uid, &file_gid, &file_mode);

    uint16_t granted = 0;
    if (uid == file_uid) {
        /* Owner permissions */
        if (file_mode & PERM_UR) granted |= ACCESS_READ;
        if (file_mode & PERM_UW) granted |= ACCESS_WRITE;
        if (file_mode & PERM_UX) granted |= ACCESS_EXEC;
    } else if (gid == file_gid || group_is_member(file_gid, uid)) {
        /* Group permissions */
        if (file_mode & PERM_GR) granted |= ACCESS_READ;
        if (file_mode & PERM_GW) granted |= ACCESS_WRITE;
        if (file_mode & PERM_GX) granted |= ACCESS_EXEC;
    } else {
        /* Other permissions */
        if (file_mode & PERM_OR) granted |= ACCESS_READ;
        if (file_mode & PERM_OW) granted |= ACCESS_WRITE;
        if (file_mode & PERM_OX) granted |= ACCESS_EXEC;
    }

    return (granted & mode) == (uint16_t)mode;
}

int perm_chmod(const char *path, uint16_t mode) {
    if (!path) return -1;
    /* Must be root or file owner */
    uint32_t file_uid, file_gid;
    uint16_t old_mode;
    perm_get(path, &file_uid, &file_gid, &old_mode);
    if (!user_is_root() && user_current_uid() != file_uid) {
        printf("chmod: permission denied\n");
        return -1;
    }
    return perm_set(path, file_uid, file_gid, mode);
}

int perm_chown(const char *path, uint32_t uid, uint32_t gid) {
    if (!path) return -1;
    /* Only root can chown */
    if (!user_is_root()) {
        printf("chown: permission denied (must be root)\n");
        return -1;
    }
    uint32_t old_uid, old_gid;
    uint16_t mode;
    perm_get(path, &old_uid, &old_gid, &mode);
    return perm_set(path, uid, gid, mode);
}

void perm_list(const char *directory) {
    printf("Permissions for %s:\n", directory ? directory : "/");
    printf("%-9s %-8s %-8s %s\n", "Mode", "Owner", "Group", "Path");
    printf("----------------------------------------------\n");
    int n = 0;
    for (int i = 0; i < MAX_PERM_ENTRIES; i++) {
        if (!perms[i].active) continue;
        /* If directory specified, filter */
        if (directory && strlen(directory) > 1) {
            size_t dlen = strlen(directory);
            if (strncmp(perms[i].path, directory, dlen) != 0) continue;
        }
        char mode_str[10];
        perm_mode_to_string(perms[i].mode, mode_str);
        user_t  *u = user_find_by_uid(perms[i].uid);
        group_t *g = group_find_by_gid(perms[i].gid);
        printf("%s  %-8s %-8s %s\n",
               mode_str,
               u ? u->username : "?",
               g ? g->name : "?",
               perms[i].path);
        n++;
    }
    if (n == 0) printf("  (no permission entries)\n");
}

/* ================================================================
 *  Persistence  (simple text format on FAT32)
 * ================================================================ */

/*  /PASSWD format – one user per line:
 *    username:hash:uid:gid:home:shell
 *  /GROUP format – one group per line:
 *    name:gid:member_uid,member_uid,...
 *  /PERMS.DB format – one entry per line:
 *    path:uid:gid:mode
 */

/* Parse an unsigned integer from a string, advance pointer */
static uint32_t parse_uint(const char **p) {
    uint32_t val = 0;
    while (**p >= '0' && **p <= '9') {
        val = val * 10 + (**p - '0');
        (*p)++;
    }
    return val;
}

/* Copy field up to delimiter into buf, return pointer past delimiter */
static const char *parse_field(const char *p, char *buf, size_t max, char delim) {
    size_t i = 0;
    while (*p && *p != delim && *p != '\n' && *p != '\r' && i < max - 1) {
        buf[i++] = *p++;
    }
    buf[i] = '\0';
    if (*p == delim) p++;
    return p;
}

int users_save(void) {
    if (!vfs_has_root_mount()) return -1;

    /* Build /PASSWD */
    char buf[4096];
    int pos = 0;
    for (int i = 0; i < MAX_USERS; i++) {
        if (!users[i].active) continue;
        int n = snprintf(buf + pos, sizeof(buf) - pos,
                         "%s:%u:%u:%u:%s:%s\n",
                         users[i].username,
                         users[i].password_hash,
                         users[i].uid,
                         users[i].gid,
                         users[i].home,
                         users[i].shell);
        if (n > 0) pos += n;
    }

    /* Write /PASSWD */
    vfs_write_file("/PASSWD", buf, (uint32_t)pos);

    /* Build /GROUP */
    pos = 0;
    for (int i = 0; i < MAX_GROUPS; i++) {
        if (!groups[i].active) continue;
        int n = snprintf(buf + pos, sizeof(buf) - pos,
                         "%s:%u:",
                         groups[i].name,
                         groups[i].gid);
        if (n > 0) pos += n;
        for (uint32_t j = 0; j < groups[i].member_count; j++) {
            n = snprintf(buf + pos, sizeof(buf) - pos, "%u%s",
                         groups[i].members[j],
                         (j + 1 < groups[i].member_count) ? "," : "");
            if (n > 0) pos += n;
        }
        if (pos < (int)sizeof(buf)) buf[pos++] = '\n';
    }

    vfs_write_file("/GROUP", buf, (uint32_t)pos);

    serial_printf("users: saved to /PASSWD and /GROUP\n");
    return 0;
}

int users_load(void) {
    if (!vfs_has_root_mount()) return -1;

    char buf[4096];
    void *vbuf = NULL;
    int bytes = vfs_read_file_alloc("/PASSWD", &vbuf, sizeof(buf) - 1);
    if (bytes <= 0 || !vbuf) { if (vbuf) kfree(vbuf); return -1; }
    memcpy(buf, vbuf, (size_t)bytes + 1);  /* includes NUL */
    kfree(vbuf);

    /* Parse each line */
    const char *p = buf;
    int idx = 0;
    while (*p && idx < MAX_USERS) {
        if (*p == '\n' || *p == '\r') { p++; continue; }

        char username[MAX_USERNAME_LEN] = {0};
        char hash_str[32] = {0};
        char uid_str[16] = {0}, gid_str[16] = {0};
        char home[MAX_HOME_LEN] = {0};
        char shell[MAX_SHELL_LEN] = {0};

        p = parse_field(p, username, sizeof(username), ':');
        p = parse_field(p, hash_str, sizeof(hash_str), ':');
        p = parse_field(p, uid_str, sizeof(uid_str), ':');
        p = parse_field(p, gid_str, sizeof(gid_str), ':');
        p = parse_field(p, home, sizeof(home), ':');
        p = parse_field(p, shell, sizeof(shell), '\n');

        if (username[0]) {
            users[idx].active = true;
            strncpy(users[idx].username, username, MAX_USERNAME_LEN - 1);
            users[idx].password_hash = (uint32_t)atoi(hash_str);
            /* atoi can't handle full uint32_t, use parse_uint */
            {
                const char *hp = hash_str;
                users[idx].password_hash = parse_uint(&hp);
            }
            {
                const char *up = uid_str;
                users[idx].uid = parse_uint(&up);
            }
            {
                const char *gp = gid_str;
                users[idx].gid = parse_uint(&gp);
            }
            strncpy(users[idx].home, home, MAX_HOME_LEN - 1);
            strncpy(users[idx].shell, shell[0] ? shell : "zeus", MAX_SHELL_LEN - 1);
            idx++;
        }

        /* Skip to next line */
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }

    /* Load groups */
    {
        void *gbuf = NULL;
        bytes = vfs_read_file_alloc("/GROUP", &gbuf, sizeof(buf) - 1);
        if (bytes > 0 && gbuf) {
            memcpy(buf, gbuf, (size_t)bytes + 1);
            kfree(gbuf);
            gbuf = NULL;
            {
                p = buf;
                int gidx = 0;
                while (*p && gidx < MAX_GROUPS) {
                    if (*p == '\n' || *p == '\r') { p++; continue; }

                    char gname[MAX_GROUP_NAME_LEN] = {0};
                    char gid_str2[16] = {0};
                    char members_str[256] = {0};

                    p = parse_field(p, gname, sizeof(gname), ':');
                    p = parse_field(p, gid_str2, sizeof(gid_str2), ':');
                    p = parse_field(p, members_str, sizeof(members_str), '\n');

                    if (gname[0]) {
                        groups[gidx].active = true;
                        strncpy(groups[gidx].name, gname, MAX_GROUP_NAME_LEN - 1);
                        const char *mp = gid_str2;
                        groups[gidx].gid = parse_uint(&mp);
                        groups[gidx].member_count = 0;

                        /* Parse member list */
                        if (members_str[0]) {
                            const char *ms = members_str;
                            while (*ms && groups[gidx].member_count < MAX_GROUP_MEMBERS) {
                                groups[gidx].members[groups[gidx].member_count++] = parse_uint(&ms);
                                if (*ms == ',') ms++;
                            }
                        }
                        gidx++;
                    }

                    while (*p && *p != '\n') p++;
                    if (*p == '\n') p++;
                }
            }
        } else if (gbuf) {
            kfree(gbuf);
        }
    }

    serial_printf("users: loaded from /PASSWD and /GROUP\n");
    return 0;
}

int perms_save(void) {
    if (!vfs_has_root_mount()) return -1;

    char buf[8192];
    int pos = 0;
    for (int i = 0; i < MAX_PERM_ENTRIES; i++) {
        if (!perms[i].active) continue;
        int n = snprintf(buf + pos, sizeof(buf) - pos,
                         "%s:%u:%u:%u\n",
                         perms[i].path,
                         perms[i].uid,
                         perms[i].gid,
                         (uint32_t)perms[i].mode);
        if (n > 0) pos += n;
    }

    vfs_write_file("/PERMS.DB", buf, (uint32_t)pos);
    serial_printf("users: permissions saved to /PERMS.DB\n");
    return 0;
}

int perms_load(void) {
    if (!vfs_has_root_mount()) return -1;

    char buf[8192];
    void *vbuf = NULL;
    int bytes = vfs_read_file_alloc("/PERMS.DB", &vbuf, sizeof(buf) - 1);
    if (bytes <= 0 || !vbuf) { if (vbuf) kfree(vbuf); return -1; }
    memcpy(buf, vbuf, (size_t)bytes + 1);
    kfree(vbuf);

    const char *p = buf;
    int idx = 0;
    while (*p && idx < MAX_PERM_ENTRIES) {
        if (*p == '\n' || *p == '\r') { p++; continue; }

        char path[256] = {0};
        char uid_str[16] = {0}, gid_str[16] = {0}, mode_str[16] = {0};

        p = parse_field(p, path, sizeof(path), ':');
        p = parse_field(p, uid_str, sizeof(uid_str), ':');
        p = parse_field(p, gid_str, sizeof(gid_str), ':');
        p = parse_field(p, mode_str, sizeof(mode_str), '\n');

        if (path[0]) {
            perms[idx].active = true;
            strncpy(perms[idx].path, path, sizeof(perms[idx].path) - 1);
            const char *up = uid_str;
            perms[idx].uid = parse_uint(&up);
            const char *gp = gid_str;
            perms[idx].gid = parse_uint(&gp);
            const char *mp = mode_str;
            perms[idx].mode = (uint16_t)parse_uint(&mp);
            idx++;
        }

        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }

    serial_printf("users: loaded %d permission entries from /PERMS.DB\n", idx);
    return 0;
}
