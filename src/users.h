/**
 * @file users.h
 * @brief User accounts and file permissions system
 *
 * Provides Unix-like user/group management and file permission checking.
 * User data is persisted to /PASSWD and /GROUP on FAT32.
 * File permissions are stored in /PERMS.DB as a metadata overlay.
 */
#ifndef USERS_H
#define USERS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ---------- limits ---------- */
#define MAX_USERS           32
#define MAX_GROUPS          32
#define MAX_USERNAME_LEN    32
#define MAX_GROUP_NAME_LEN  32
#define MAX_PASSWORD_LEN    64
#define MAX_HOME_LEN        128
#define MAX_SHELL_LEN       64
#define MAX_GROUP_MEMBERS   16
#define MAX_PERM_ENTRIES    256

/* ---------- special UIDs / GIDs ---------- */
#define UID_ROOT    0
#define GID_ROOT    0
#define UID_NOBODY  65534
#define GID_NOBODY  65534

/* ---------- permission bits (Unix-style) ---------- */
#define PERM_UR   0400   /* owner read   */
#define PERM_UW   0200   /* owner write  */
#define PERM_UX   0100   /* owner exec   */
#define PERM_GR   0040   /* group read   */
#define PERM_GW   0020   /* group write  */
#define PERM_GX   0010   /* group exec   */
#define PERM_OR   0004   /* other read   */
#define PERM_OW   0002   /* other write  */
#define PERM_OX   0001   /* other exec   */

#define PERM_DEFAULT_FILE  (PERM_UR | PERM_UW | PERM_GR | PERM_OR)        /* 644 */
#define PERM_DEFAULT_DIR   (PERM_UR | PERM_UW | PERM_UX | PERM_GR | PERM_GX | PERM_OR | PERM_OX) /* 755 */

/* ---------- access check modes ---------- */
#define ACCESS_READ     4
#define ACCESS_WRITE    2
#define ACCESS_EXEC     1

/* ---------- structures ---------- */

typedef struct {
    uint32_t uid;
    uint32_t gid;                           /* primary group */
    char     username[MAX_USERNAME_LEN];
    uint32_t password_hash;                 /* simple DJB2 hash */
    char     home[MAX_HOME_LEN];
    char     shell[MAX_SHELL_LEN];
    bool     active;
} user_t;

typedef struct {
    uint32_t gid;
    char     name[MAX_GROUP_NAME_LEN];
    uint32_t members[MAX_GROUP_MEMBERS];    /* UIDs */
    uint32_t member_count;
    bool     active;
} group_t;

typedef struct {
    char     path[256];                     /* absolute file path */
    uint32_t uid;                           /* owner */
    uint32_t gid;                           /* group */
    uint16_t mode;                          /* permission bits */
    bool     active;
} perm_entry_t;

/* ---------- initialisation ---------- */
void     users_init(void);

/* ---------- user management ---------- */
int      user_add(const char *username, const char *password, uint32_t uid, uint32_t gid,
                  const char *home, const char *shell);
int      user_remove(const char *username);
user_t  *user_find_by_name(const char *username);
user_t  *user_find_by_uid(uint32_t uid);
int      user_set_password(const char *username, const char *password);
bool     user_check_password(const char *username, const char *password);
int      user_list(void);

/* ---------- group management ---------- */
int      group_add(const char *name, uint32_t gid);
int      group_remove(const char *name);
group_t *group_find_by_name(const char *name);
group_t *group_find_by_gid(uint32_t gid);
int      group_add_member(const char *group_name, uint32_t uid);
int      group_remove_member(const char *group_name, uint32_t uid);
bool     group_is_member(uint32_t gid, uint32_t uid);
int      group_list(void);

/* ---------- session / current user ---------- */
int      user_login(const char *username, const char *password);
void     user_logout(void);
uint32_t user_current_uid(void);
uint32_t user_current_gid(void);
const char *user_current_name(void);
bool     user_is_root(void);

/* ---------- file permission overlay ---------- */
int      perm_set(const char *path, uint32_t uid, uint32_t gid, uint16_t mode);
int      perm_get(const char *path, uint32_t *uid, uint32_t *gid, uint16_t *mode);
int      perm_remove(const char *path);
bool     perm_check_access(const char *path, uint32_t uid, uint32_t gid, int mode);
int      perm_chmod(const char *path, uint16_t mode);
int      perm_chown(const char *path, uint32_t uid, uint32_t gid);
void     perm_list(const char *directory);

/* ---------- persistence ---------- */
int      users_save(void);
int      users_load(void);
int      perms_save(void);
int      perms_load(void);

/* ---------- utility ---------- */
uint32_t password_hash(const char *password);
void     perm_mode_to_string(uint16_t mode, char *buf);   /* buf must be >= 10 chars */

#endif /* USERS_H */
