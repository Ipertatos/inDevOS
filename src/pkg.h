/**
 * @file pkg.h
 * @brief Package manager for inDevOS
 *
 * Provides a simple package management system:
 *   - Package database stored in /PKG/ directory on FAT32
 *   - Package metadata: name, version, description, dependencies, files
 *   - Install / uninstall / list / info commands
 *   - Package format: .ipkg archive (concatenation of header + tar-like file entries)
 *   - Dependency resolution
 *   - Package repository support (local only for now)
 */
#ifndef PKG_H
#define PKG_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ---------- limits ---------- */
#define PKG_MAX_INSTALLED       64
#define PKG_MAX_NAME_LEN        64
#define PKG_MAX_VERSION_LEN     32
#define PKG_MAX_DESC_LEN        256
#define PKG_MAX_DEPS            8
#define PKG_MAX_FILES           32
#define PKG_MAX_PATH_LEN        128
#define PKG_DB_DIR              "/PKG"
#define PKG_DB_FILE             "/PKG/PKGDB"
#define PKG_REPO_DIR            "/PKG/REPO"

/* ---------- package states ---------- */
typedef enum {
    PKG_STATE_NONE = 0,
    PKG_STATE_INSTALLED,
    PKG_STATE_BROKEN,
    PKG_STATE_AVAILABLE      /* known but not installed */
} pkg_state_t;

/* ---------- package file entry ---------- */
typedef struct {
    char path[PKG_MAX_PATH_LEN];   /* installed path on disk */
    uint32_t size;
    bool is_directory;
} pkg_file_entry_t;

/* ---------- package metadata ---------- */
typedef struct {
    char name[PKG_MAX_NAME_LEN];
    char version[PKG_MAX_VERSION_LEN];
    char description[PKG_MAX_DESC_LEN];
    char author[PKG_MAX_NAME_LEN];

    /* Dependencies (by name) */
    char deps[PKG_MAX_DEPS][PKG_MAX_NAME_LEN];
    int  dep_count;

    /* Installed files */
    pkg_file_entry_t files[PKG_MAX_FILES];
    int file_count;

    pkg_state_t state;
    uint32_t    install_time;       /* RTC timestamp */
    uint32_t    total_size;         /* total bytes of all files */
    bool        active;
} pkg_info_t;

/* ---------- .ipkg archive format ---------- */

#define IPKG_MAGIC      "IPKG"
#define IPKG_VERSION    1

typedef struct __attribute__((packed)) {
    char     magic[4];              /* "IPKG" */
    uint8_t  version;               /* format version */
    uint8_t  reserved[3];
    char     name[PKG_MAX_NAME_LEN];
    char     pkg_version[PKG_MAX_VERSION_LEN];
    char     description[PKG_MAX_DESC_LEN];
    char     author[PKG_MAX_NAME_LEN];
    uint32_t file_count;            /* number of file entries */
    uint32_t total_size;            /* total payload size */
} ipkg_header_t;

/* Each file in the archive is preceded by this header */
typedef struct __attribute__((packed)) {
    char     path[PKG_MAX_PATH_LEN];
    uint32_t size;
    uint8_t  is_directory;
    uint8_t  reserved[3];
} ipkg_file_header_t;

/* ---------- public API ---------- */

/**
 * Initialize the package manager (loads DB from FAT32).
 */
void pkg_init(void);

/**
 * Install a package from an .ipkg file.
 * @param path  Path to the .ipkg file on FAT32
 * @return 0 on success, -1 on failure
 */
int pkg_install(const char *path);

/**
 * Uninstall a package by name.
 * @return 0 on success, -1 on failure
 */
int pkg_uninstall(const char *name);

/**
 * List all installed packages.
 */
void pkg_list(void);

/**
 * Print detailed info about a package.
 */
void pkg_info(const char *name);

/**
 * Search for a package by name or keyword.
 */
void pkg_search(const char *keyword);

/**
 * Check if a package is installed.
 */
bool pkg_is_installed(const char *name);

/**
 * Get package info by name. Returns NULL if not found.
 */
pkg_info_t *pkg_find(const char *name);

/**
 * Check if all dependencies of a package are satisfied.
 */
bool pkg_check_deps(const char *name);

/**
 * Save the package database to disk.
 */
int pkg_save_db(void);

/**
 * Load the package database from disk.
 */
int pkg_load_db(void);

/**
 * Create a .ipkg package from a directory.
 * @param name        Package name
 * @param version     Version string
 * @param description Description
 * @param source_dir  Directory containing files to package
 * @param output_path Path for the output .ipkg file
 * @return 0 on success
 */
int pkg_create(const char *name, const char *version, const char *description,
               const char *source_dir, const char *output_path);

/**
 * Get total number of installed packages.
 */
int pkg_count(void);

#endif /* PKG_H */
