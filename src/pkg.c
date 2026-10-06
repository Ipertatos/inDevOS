/**
 * @file pkg.c
 * @brief Package manager implementation
 */

#include "pkg.h"
#include "vfs.h"
#include "mem.h"
#include "serial.h"
#include "string.h"

extern int  printf(const char *fmt, ...);
extern int  snprintf(char *str, size_t size, const char *fmt, ...);
extern void *memset(void *, int, size_t);
extern void *memcpy(void *, const void *, size_t);
extern int   memcmp(const void *, const void *, size_t);
extern int   strcmp(const char *a, const char *b);
extern int   strncmp(const char *a, const char *b, size_t n);
extern size_t strlen(const char *s);
extern char *strcpy(char *dst, const char *src);
extern char *strncpy(char *dst, const char *src, size_t n);

/* Forward declarations */
static bool strstr_simple(const char *haystack, const char *needle);

/* ================================================================
 *  Internal state
 * ================================================================ */

static pkg_info_t packages[PKG_MAX_INSTALLED];
static bool       pkg_ready = false;

/* ================================================================
 *  Initialisation
 * ================================================================ */

void pkg_init(void) {
    memset(packages, 0, sizeof(packages));

    /* Ensure PKG directory exists */
    if (vfs_has_root_mount()) {
        if (!vfs_exists(PKG_DB_DIR)) {
            vfs_mkdir(PKG_DB_DIR);
        }
        pkg_load_db();
    }

    pkg_ready = true;
    serial_printf("pkg: initialized (%d packages installed)\n", pkg_count());
}

/* ================================================================
 *  Helpers
 * ================================================================ */

int pkg_count(void) {
    int n = 0;
    for (int i = 0; i < PKG_MAX_INSTALLED; i++) {
        if (packages[i].active && packages[i].state == PKG_STATE_INSTALLED)
            n++;
    }
    return n;
}

pkg_info_t *pkg_find(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < PKG_MAX_INSTALLED; i++) {
        if (packages[i].active && strcmp(packages[i].name, name) == 0)
            return &packages[i];
    }
    return NULL;
}

bool pkg_is_installed(const char *name) {
    pkg_info_t *p = pkg_find(name);
    return p && p->state == PKG_STATE_INSTALLED;
}

bool pkg_check_deps(const char *name) {
    pkg_info_t *p = pkg_find(name);
    if (!p) return false;
    for (int i = 0; i < p->dep_count; i++) {
        if (!pkg_is_installed(p->deps[i])) {
            printf("pkg: missing dependency '%s' for '%s'\n", p->deps[i], name);
            return false;
        }
    }
    return true;
}

/*
 * Validate a file path coming from an untrusted .ipkg archive.
 * Rejects: empty paths, unterminated strings, ".." traversal components,
 * backslashes, and control characters. Returns true if safe to use.
 */
static bool pkg_path_valid(const char *path, size_t max_len) {
    if (!path) return false;

    /* Must be NUL-terminated within the fixed-size field */
    size_t len = 0;
    while (len < max_len && path[len] != '\0') len++;
    if (len == 0 || len >= max_len) return false;

    const char *p = path;
    while (*p) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == '\\') return false;

        /* Check each path component for "." / ".." */
        if (p == path || p[-1] == '/') {
            if (p[0] == '.' &&
                (p[1] == '\0' || p[1] == '/' ||
                 (p[1] == '.' && (p[2] == '\0' || p[2] == '/')))) {
                return false;
            }
        }
        p++;
    }
    return true;
}

/* ================================================================
 *  Install from .ipkg
 * ================================================================ */

int pkg_install(const char *path) {
    if (!pkg_ready || !path) return -1;
    if (!vfs_has_root_mount()) {
        printf("pkg: no filesystem mounted\n");
        return -1;
    }

    /* Read the .ipkg file via VFS */
    void *vdata = NULL;
    int bytes_read = vfs_read_file_alloc(path, &vdata, 0);
    if (bytes_read < 0 || !vdata) {
        printf("pkg: cannot open '%s'\n", path);
        if (vdata) kfree(vdata);
        return -1;
    }
    uint32_t fsize = (uint32_t)bytes_read;
    uint8_t *data = (uint8_t *)vdata;
    if (fsize < sizeof(ipkg_header_t)) {
        printf("pkg: file too small\n");
        kfree(data);
        return -1;
    }

    /* Verify header */
    ipkg_header_t *hdr = (ipkg_header_t *)data;
    if (memcmp(hdr->magic, IPKG_MAGIC, 4) != 0) {
        printf("pkg: invalid package format\n");
        kfree(data);
        return -1;
    }
    if (hdr->version != IPKG_VERSION) {
        printf("pkg: unsupported package version %u\n", hdr->version);
        kfree(data);
        return -1;
    }

    /* Force NUL termination of untrusted header string fields before use */
    hdr->name[PKG_MAX_NAME_LEN - 1] = '\0';
    hdr->pkg_version[PKG_MAX_VERSION_LEN - 1] = '\0';
    hdr->description[PKG_MAX_DESC_LEN - 1] = '\0';
    hdr->author[PKG_MAX_NAME_LEN - 1] = '\0';

    /* Check if already installed */
    if (pkg_is_installed(hdr->name)) {
        printf("pkg: '%s' is already installed\n", hdr->name);
        kfree(data);
        return -1;
    }

    /* Find a free slot */
    int slot = -1;
    for (int i = 0; i < PKG_MAX_INSTALLED; i++) {
        if (!packages[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        printf("pkg: package database full\n");
        kfree(data);
        return -1;
    }

    /* Fill metadata */
    pkg_info_t *pkg = &packages[slot];
    memset(pkg, 0, sizeof(pkg_info_t));
    pkg->active = true;
    pkg->state  = PKG_STATE_INSTALLED;
    strncpy(pkg->name, hdr->name, PKG_MAX_NAME_LEN - 1);
    strncpy(pkg->version, hdr->pkg_version, PKG_MAX_VERSION_LEN - 1);
    strncpy(pkg->description, hdr->description, PKG_MAX_DESC_LEN - 1);
    strncpy(pkg->author, hdr->author, PKG_MAX_NAME_LEN - 1);
    pkg->total_size = hdr->total_size;

    /* Extract files */
    uint32_t offset = sizeof(ipkg_header_t);
    int file_count = 0;

    for (uint32_t fi = 0; fi < hdr->file_count && file_count < PKG_MAX_FILES; fi++) {
        if (offset + sizeof(ipkg_file_header_t) > fsize) break;

        ipkg_file_header_t *fh = (ipkg_file_header_t *)(data + offset);
        offset += sizeof(ipkg_file_header_t);

        /* Reject malicious paths ("../...", unterminated, control chars):
         * a hostile archive must not be able to write outside its tree. */
        if (!pkg_path_valid(fh->path, PKG_MAX_PATH_LEN)) {
            printf("pkg: rejecting invalid path in archive (entry %u)\n", fi);
            if (!fh->is_directory) {
                if (fh->size > fsize - offset) break;  /* overflow-safe */
                offset += fh->size;  /* skip this entry's payload */
            }
            continue;
        }

        strncpy(pkg->files[file_count].path, fh->path, PKG_MAX_PATH_LEN - 1);
        pkg->files[file_count].size = fh->size;
        pkg->files[file_count].is_directory = fh->is_directory;

        if (fh->is_directory) {
            /* Create directory */
            vfs_mkdir(fh->path);
        } else {
            /* Write file data (overflow-safe size check) */
            if (fh->size > fsize - offset) {
                printf("pkg: truncated archive at file '%s'\n", fh->path);
                break;
            }

            vfs_write_file(fh->path, data + offset, fh->size);
            offset += fh->size;
        }

        file_count++;
    }
    pkg->file_count = file_count;

    kfree(data);

    /* Save database */
    pkg_save_db();

    printf("pkg: installed '%s' v%s (%d files)\n", pkg->name, pkg->version, file_count);
    return 0;
}

/* ================================================================
 *  Uninstall
 * ================================================================ */

int pkg_uninstall(const char *name) {
    if (!pkg_ready || !name) return -1;

    pkg_info_t *pkg = pkg_find(name);
    if (!pkg || pkg->state != PKG_STATE_INSTALLED) {
        printf("pkg: '%s' is not installed\n", name);
        return -1;
    }

    /* Check if any other package depends on this one */
    for (int i = 0; i < PKG_MAX_INSTALLED; i++) {
        if (!packages[i].active || packages[i].state != PKG_STATE_INSTALLED) continue;
        if (strcmp(packages[i].name, name) == 0) continue;
        for (int d = 0; d < packages[i].dep_count; d++) {
            if (strcmp(packages[i].deps[d], name) == 0) {
                printf("pkg: cannot uninstall '%s' – required by '%s'\n",
                       name, packages[i].name);
                return -1;
            }
        }
    }

    /* Delete installed files (in reverse order to remove files before dirs) */
    for (int i = pkg->file_count - 1; i >= 0; i--) {
        if (!pkg->files[i].is_directory) {
            vfs_unlink(pkg->files[i].path);
        }
        /* Directories would need rmdir – skip for now as they may contain other files */
    }

    printf("pkg: uninstalled '%s' (%d files removed)\n", name, pkg->file_count);

    memset(pkg, 0, sizeof(pkg_info_t));
    pkg_save_db();

    return 0;
}

/* ================================================================
 *  Listing / info
 * ================================================================ */

void pkg_list(void) {
    printf("%-20s %-12s %-8s %s\n", "Package", "Version", "Size", "State");
    printf("--------------------------------------------------------\n");
    int n = 0;
    for (int i = 0; i < PKG_MAX_INSTALLED; i++) {
        if (!packages[i].active) continue;
        const char *state_str = "?";
        switch (packages[i].state) {
        case PKG_STATE_INSTALLED: state_str = "installed"; break;
        case PKG_STATE_BROKEN:    state_str = "broken"; break;
        case PKG_STATE_AVAILABLE: state_str = "available"; break;
        default: break;
        }
        printf("%-20s %-12s %-8u %s\n",
               packages[i].name,
               packages[i].version,
               packages[i].total_size,
               state_str);
        n++;
    }
    if (n == 0) printf("  No packages installed\n");
    printf("Total: %d package(s)\n", n);
}

void pkg_info(const char *name) {
    pkg_info_t *p = pkg_find(name);
    if (!p) {
        printf("pkg: '%s' not found\n", name);
        return;
    }

    printf("Package:     %s\n", p->name);
    printf("Version:     %s\n", p->version);
    printf("Description: %s\n", p->description);
    printf("Author:      %s\n", p->author);
    printf("State:       %s\n",
           p->state == PKG_STATE_INSTALLED ? "installed" :
           p->state == PKG_STATE_BROKEN ? "broken" : "available");
    printf("Total size:  %u bytes\n", p->total_size);
    printf("Files:       %d\n", p->file_count);

    if (p->dep_count > 0) {
        printf("Dependencies: ");
        for (int i = 0; i < p->dep_count; i++) {
            printf("%s%s", p->deps[i], (i < p->dep_count - 1) ? ", " : "");
        }
        printf("\n");
    }

    if (p->file_count > 0) {
        printf("\nInstalled files:\n");
        for (int i = 0; i < p->file_count; i++) {
            printf("  %c %s (%u bytes)\n",
                   p->files[i].is_directory ? 'd' : '-',
                   p->files[i].path,
                   p->files[i].size);
        }
    }
}

void pkg_search(const char *keyword) {
    if (!keyword) return;
    int found = 0;
    for (int i = 0; i < PKG_MAX_INSTALLED; i++) {
        if (!packages[i].active) continue;
        /* Simple substring search in name and description */
        bool match = false;
        if (strstr_simple(packages[i].name, keyword)) match = true;
        if (strstr_simple(packages[i].description, keyword)) match = true;
        if (match) {
            printf("%-20s %-12s %s\n", packages[i].name, packages[i].version,
                   packages[i].description);
            found++;
        }
    }
    if (found == 0) printf("No packages matching '%s'\n", keyword);
}

/* Simple substring search */
static bool strstr_simple(const char *haystack, const char *needle) {
    if (!haystack || !needle) return false;
    size_t nlen = strlen(needle);
    size_t hlen = strlen(haystack);
    if (nlen > hlen) return false;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        if (strncmp(haystack + i, needle, nlen) == 0) return true;
    }
    return false;
}

/* ================================================================
 *  Database persistence (simple text format)
 *
 *  Format: one package per block, fields separated by newlines:
 *    NAME\nVERSION\nDESCRIPTION\nAUTHOR\nSTATE\nFILE_COUNT\n
 *    DEP:dep1\nDEP:dep2\n
 *    FILE:path:size:is_dir\n
 *    ---\n   (separator between packages)
 * ================================================================ */

static uint32_t parse_uint_s(const char **p) {
    uint32_t val = 0;
    while (**p >= '0' && **p <= '9') {
        val = val * 10 + (**p - '0');
        (*p)++;
    }
    return val;
}

static const char *read_line(const char *p, char *buf, size_t max) {
    size_t i = 0;
    while (*p && *p != '\n' && i < max - 1) {
        buf[i++] = *p++;
    }
    buf[i] = '\0';
    if (*p == '\n') p++;
    return p;
}

int pkg_save_db(void) {
    if (!vfs_has_root_mount()) return -1;

    /* Ensure directory exists */
    if (!vfs_exists(PKG_DB_DIR))
        vfs_mkdir(PKG_DB_DIR);

    char *buf = (char *)kalloc(16384);
    if (!buf) return -1;
    int pos = 0;

    for (int i = 0; i < PKG_MAX_INSTALLED; i++) {
        if (!packages[i].active) continue;
        pkg_info_t *p = &packages[i];

        pos += snprintf(buf + pos, 16384 - pos, "%s\n", p->name);
        pos += snprintf(buf + pos, 16384 - pos, "%s\n", p->version);
        pos += snprintf(buf + pos, 16384 - pos, "%s\n", p->description);
        pos += snprintf(buf + pos, 16384 - pos, "%s\n", p->author);
        pos += snprintf(buf + pos, 16384 - pos, "%u\n", (uint32_t)p->state);
        pos += snprintf(buf + pos, 16384 - pos, "%u\n", p->total_size);

        /* Dependencies */
        for (int d = 0; d < p->dep_count; d++) {
            pos += snprintf(buf + pos, 16384 - pos, "DEP:%s\n", p->deps[d]);
        }

        /* Files */
        for (int f = 0; f < p->file_count; f++) {
            pos += snprintf(buf + pos, 16384 - pos, "FILE:%s:%u:%d\n",
                           p->files[f].path, p->files[f].size, p->files[f].is_directory);
        }

        pos += snprintf(buf + pos, 16384 - pos, "---\n");
    }

    vfs_write_file(PKG_DB_FILE, buf, (uint32_t)pos);

    kfree(buf);
    serial_printf("pkg: database saved (%d packages)\n", pkg_count());
    return 0;
}

int pkg_load_db(void) {
    if (!vfs_has_root_mount()) return -1;

    void *vbuf = NULL;
    int bytes = vfs_read_file_alloc(PKG_DB_FILE, &vbuf, 16383);
    if (bytes <= 0 || !vbuf) { if (vbuf) kfree(vbuf); return -1; }
    char *buf = (char *)vbuf;  /* NUL-terminated by vfs_read_file_alloc */

    const char *p = buf;
    int idx = 0;
    char line[512];

    while (*p && idx < PKG_MAX_INSTALLED) {
        pkg_info_t *pkg = &packages[idx];
        memset(pkg, 0, sizeof(pkg_info_t));

        /* Read fields */
        p = read_line(p, pkg->name, PKG_MAX_NAME_LEN);
        if (pkg->name[0] == '\0') break;

        p = read_line(p, pkg->version, PKG_MAX_VERSION_LEN);
        p = read_line(p, pkg->description, PKG_MAX_DESC_LEN);
        p = read_line(p, pkg->author, PKG_MAX_NAME_LEN);

        p = read_line(p, line, sizeof(line));
        { const char *lp = line; pkg->state = (pkg_state_t)parse_uint_s(&lp); }

        p = read_line(p, line, sizeof(line));
        { const char *lp = line; pkg->total_size = parse_uint_s(&lp); }

        /* Read DEP: and FILE: lines until --- separator */
        while (*p) {
            p = read_line(p, line, sizeof(line));
            if (strcmp(line, "---") == 0) break;

            if (strncmp(line, "DEP:", 4) == 0 && pkg->dep_count < PKG_MAX_DEPS) {
                strncpy(pkg->deps[pkg->dep_count], line + 4, PKG_MAX_NAME_LEN - 1);
                pkg->dep_count++;
            } else if (strncmp(line, "FILE:", 5) == 0 && pkg->file_count < PKG_MAX_FILES) {
                /* Parse FILE:path:size:is_dir */
                char fpath[PKG_MAX_PATH_LEN] = {0};
                const char *fp = line + 5;
                int fi = 0;
                while (*fp && *fp != ':' && fi < PKG_MAX_PATH_LEN - 1) fpath[fi++] = *fp++;
                fpath[fi] = '\0';
                if (*fp == ':') fp++;
                uint32_t fsize_val = parse_uint_s(&fp);
                if (*fp == ':') fp++;
                uint32_t is_dir = parse_uint_s(&fp);

                strncpy(pkg->files[pkg->file_count].path, fpath, PKG_MAX_PATH_LEN - 1);
                pkg->files[pkg->file_count].size = fsize_val;
                pkg->files[pkg->file_count].is_directory = (is_dir != 0);
                pkg->file_count++;
            }
        }

        pkg->active = true;
        idx++;
    }

    kfree(buf);
    serial_printf("pkg: loaded %d packages from database\n", idx);
    return 0;
}

/* ================================================================
 *  Create a .ipkg package
 * ================================================================ */

int pkg_create(const char *name, const char *version, const char *description,
               const char *source_dir, const char *output_path) {
    if (!vfs_has_root_mount() || !name || !output_path) return -1;

    /* List files in source directory via VFS */
    static vfs_dirent_t entries[PKG_MAX_FILES];
    int file_count = vfs_readdir(source_dir, entries, PKG_MAX_FILES);
    if (file_count <= 0) {
        printf("pkg: no files found in '%s'\n", source_dir);
        return -1;
    }

    /* Calculate total size needed */
    uint32_t total_payload = 0;
    for (int i = 0; i < file_count; i++) {
        total_payload += sizeof(ipkg_file_header_t) + entries[i].size;
    }

    uint32_t archive_size = sizeof(ipkg_header_t) + total_payload;
    uint8_t *archive = (uint8_t *)kalloc(archive_size);
    if (!archive) {
        printf("pkg: out of memory for archive\n");
        return -1;
    }
    memset(archive, 0, archive_size);

    /* Build header */
    ipkg_header_t *hdr = (ipkg_header_t *)archive;
    memcpy(hdr->magic, IPKG_MAGIC, 4);
    hdr->version = IPKG_VERSION;
    strncpy(hdr->name, name, PKG_MAX_NAME_LEN - 1);
    strncpy(hdr->pkg_version, version ? version : "1.0", PKG_MAX_VERSION_LEN - 1);
    strncpy(hdr->description, description ? description : "", PKG_MAX_DESC_LEN - 1);
    hdr->file_count = (uint32_t)file_count;
    hdr->total_size = total_payload;

    /* Pack each file */
    uint32_t offset = sizeof(ipkg_header_t);
    for (int i = 0; i < file_count; i++) {
        ipkg_file_header_t *fh = (ipkg_file_header_t *)(archive + offset);
        snprintf(fh->path, PKG_MAX_PATH_LEN, "%s/%s", source_dir, entries[i].name);
        fh->size = entries[i].size;
        fh->is_directory = (entries[i].type == VFS_DIRECTORY) ? 1 : 0;
        offset += sizeof(ipkg_file_header_t);

        if (entries[i].type != VFS_DIRECTORY && entries[i].size > 0) {
            /* Read file content */
            char filepath[256];
            snprintf(filepath, sizeof(filepath), "%s/%s", source_dir, entries[i].name);
            int sfd = vfs_open(filepath, VFS_O_READ);
            if (sfd >= 0) {
                uint32_t got = 0;
                while (got < entries[i].size) {
                    int n = vfs_read(sfd, archive + offset + got, entries[i].size - got);
                    if (n <= 0) break;
                    got += (uint32_t)n;
                }
                vfs_close(sfd);
            }
            offset += entries[i].size;
        }
    }

    /* Write the archive */
    vfs_write_file(output_path, archive, offset);

    kfree(archive);
    printf("pkg: created '%s' (%u bytes, %d files)\n", output_path, offset, file_count);
    return 0;
}
