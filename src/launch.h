#ifndef LAUNCH_H
#define LAUNCH_H
/**
 * Desktop launch service: file associations (/etc/indevos/open-with) and
 * opening a path with the right program or kernel app.
 */
#include <stdbool.h>
#include <stdint.h>

/* Icon classes for the desktop / Files (see desktop.c icon set). */
enum {
    LAUNCH_ICON_GENERIC = 0, LAUNCH_ICON_FOLDER, LAUNCH_ICON_TEXT,
    LAUNCH_ICON_EXE, LAUNCH_ICON_IMAGE, LAUNCH_ICON_LUA, LAUNCH_ICON_WAD,
    LAUNCH_ICON_COUNT
};

/** Open @path: folders in Files, files by association.  Returns the spawned
 *  pid (>0), 0 when a kernel app / action handled it, <0 on failure (a toast
 *  explains why). */
int  launch_open_path(const char *path, bool is_dir);
/** Icon class for a name (by extension) or a folder. */
int  launch_icon_for(const char *name, bool is_dir);
/** .desktop shortcut: label (Name=, else the file name) and icon class
 *  (Icon=).  Returns 0, or -1 when @path is not a readable shortcut. */
int  launch_shortcut_info(const char *path, char *label, int n, int *icon);
/** Re-read the association file on next use. */
void launch_reload(void);
/** Pids started through the launch service that are still alive. */
int  launch_running_count(void);

#endif
