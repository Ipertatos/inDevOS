/**
 * @file file_dialog.c
 * @brief File open/save dialog implementation
 */

#include "file_dialog.h"
#include "fat32.h"
#include "mem.h"
#include "serial.h"
#include "term.h"
#include "string.h"

extern int snprintf(char *str, size_t size, const char *fmt, ...);

/* Internal: build full path from current_path + name */
static void build_path(char *out, size_t out_sz, const char *dir, const char *name) {
    size_t dlen = strlen(dir);
    if (dlen == 0 || (dlen == 1 && dir[0] == '/')) {
        snprintf(out, out_sz, "/%s", name);
    } else {
        snprintf(out, out_sz, "%s/%s", dir, name);
    }
}

/* Internal: check if filename matches extension filter */
static bool matches_filter(const char *name, const char *filter) {
    if (!filter || filter[0] == '\0') return true;
    size_t nlen = strlen(name);
    size_t flen = strlen(filter);
    if (nlen < flen) return false;
    /* Compare suffix case-insensitively */
    for (size_t i = 0; i < flen; i++) {
        char a = name[nlen - flen + i];
        char b = filter[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return false;
    }
    return true;
}

void file_dialog_init(file_dialog_t *dialog, fd_type_t type,
                      const char *title, const char *start_path,
                      const char *filter) {
    if (!dialog) return;
    memset(dialog, 0, sizeof(file_dialog_t));
    dialog->type = type;
    dialog->title = title ? title : (type == FD_TYPE_OPEN ? "Open File" :
                                     type == FD_TYPE_SAVE ? "Save File" : "Select Directory");
    if (start_path && start_path[0]) {
        strncpy(dialog->current_path, start_path, FD_MAX_PATH - 1);
    } else {
        strncpy(dialog->current_path, "/", FD_MAX_PATH - 1);
    }
    if (filter) {
        strncpy(dialog->filter, filter, sizeof(dialog->filter) - 1);
    }
    dialog->selected_index = 0;
    dialog->scroll_offset = 0;
    dialog->result = FD_RESULT_NONE;
    dialog->visible = false;
    dialog->active = false;
}

void file_dialog_show(file_dialog_t *dialog) {
    if (!dialog) return;
    dialog->visible = true;
    dialog->active = true;
    dialog->result = FD_RESULT_NONE;
    file_dialog_refresh(dialog);
}

void file_dialog_refresh(file_dialog_t *dialog) {
    if (!dialog) return;

    dialog->file_count = 0;
    dialog->selected_index = 0;
    dialog->scroll_offset = 0;

    if (!fat32_is_mounted()) {
        serial_puts("file_dialog: no filesystem mounted\n");
        return;
    }

    /* Add ".." entry for navigation (unless at root) */
    if (strlen(dialog->current_path) > 1) {
        strncpy(dialog->files[dialog->file_count].name, "..", FD_MAX_FILENAME);
        dialog->files[dialog->file_count].is_directory = true;
        dialog->files[dialog->file_count].size = 0;
        dialog->file_count++;
    }

    /* Read directory entries */
    static fat32_dir_list_t entries[64];
    int count = fat32_list_directory(dialog->current_path, entries, 64);

    if (count < 0) {
        serial_printf("file_dialog: failed to list '%s'\n", dialog->current_path);
        return;
    }

    /* First pass: add directories */
    for (int i = 0; i < count && dialog->file_count < FD_MAX_FILES; i++) {
        if (entries[i].is_directory) {
            /* Skip "." and ".." from FAT32 */
            if (strcmp(entries[i].name, ".") == 0 || strcmp(entries[i].name, "..") == 0)
                continue;
            strncpy(dialog->files[dialog->file_count].name, entries[i].name, FD_MAX_FILENAME - 1);
            dialog->files[dialog->file_count].is_directory = true;
            dialog->files[dialog->file_count].size = 0;
            dialog->file_count++;
        }
    }

    /* Second pass: add files (with filter) */
    for (int i = 0; i < count && dialog->file_count < FD_MAX_FILES; i++) {
        if (!entries[i].is_directory) {
            /* Apply extension filter for Open dialog */
            if (dialog->type == FD_TYPE_DIR) continue;  /* Directory selection only */
            if (!matches_filter(entries[i].name, dialog->filter)) continue;

            strncpy(dialog->files[dialog->file_count].name, entries[i].name, FD_MAX_FILENAME - 1);
            dialog->files[dialog->file_count].is_directory = false;
            dialog->files[dialog->file_count].size = entries[i].size;
            dialog->file_count++;
        }
    }

    serial_printf("file_dialog: listed %d items in '%s'\n", dialog->file_count, dialog->current_path);
}

void file_dialog_navigate(file_dialog_t *dialog, const char *dirname) {
    if (!dialog || !dirname) return;

    char new_path[FD_MAX_PATH];
    build_path(new_path, sizeof(new_path), dialog->current_path, dirname);
    strncpy(dialog->current_path, new_path, FD_MAX_PATH - 1);
    file_dialog_refresh(dialog);
}

void file_dialog_go_up(file_dialog_t *dialog) {
    if (!dialog) return;

    /* Find last '/' and truncate */
    size_t len = strlen(dialog->current_path);
    if (len <= 1) return;  /* Already at root */

    /* Remove trailing slash if present */
    if (dialog->current_path[len - 1] == '/') {
        dialog->current_path[len - 1] = '\0';
        len--;
    }

    /* Find previous slash */
    while (len > 0 && dialog->current_path[len - 1] != '/') {
        len--;
    }
    if (len == 0) len = 1;  /* Keep root "/" */
    dialog->current_path[len] = '\0';
    if (len > 1 && dialog->current_path[len - 1] == '/')
        dialog->current_path[len - 1] = '\0';

    file_dialog_refresh(dialog);
}

void file_dialog_select(file_dialog_t *dialog) {
    if (!dialog || dialog->file_count == 0) return;
    if (dialog->selected_index >= dialog->file_count) return;

    fd_file_entry_t *entry = &dialog->files[dialog->selected_index];

    if (strcmp(entry->name, "..") == 0) {
        file_dialog_go_up(dialog);
        return;
    }

    if (entry->is_directory) {
        if (dialog->type == FD_TYPE_DIR) {
            /* Directory selection mode: selecting a dir is the result */
            build_path(dialog->selected_file, sizeof(dialog->selected_file),
                       dialog->current_path, entry->name);
            dialog->result = FD_RESULT_OK;
            dialog->active = false;
        } else {
            /* Navigate into directory */
            file_dialog_navigate(dialog, entry->name);
        }
        return;
    }

    /* File selected */
    build_path(dialog->selected_file, sizeof(dialog->selected_file),
               dialog->current_path, entry->name);
    strncpy(dialog->filename_input, entry->name, FD_MAX_FILENAME - 1);
    dialog->result = FD_RESULT_OK;
    dialog->active = false;
}

void file_dialog_cancel(file_dialog_t *dialog) {
    if (!dialog) return;
    dialog->result = FD_RESULT_CANCEL;
    dialog->active = false;
    dialog->visible = false;
}

int file_dialog_get_path(file_dialog_t *dialog, char *buf, size_t buflen) {
    if (!dialog || !buf || buflen == 0) return -1;
    if (dialog->result != FD_RESULT_OK) return -1;
    strncpy(buf, dialog->selected_file, buflen - 1);
    buf[buflen - 1] = '\0';
    return 0;
}

void file_dialog_select_prev(file_dialog_t *dialog) {
    if (!dialog || dialog->file_count == 0) return;
    if (dialog->selected_index > 0) {
        dialog->selected_index--;
        if (dialog->selected_index < dialog->scroll_offset)
            dialog->scroll_offset = dialog->selected_index;
    }
}

void file_dialog_select_next(file_dialog_t *dialog) {
    if (!dialog || dialog->file_count == 0) return;
    if (dialog->selected_index < dialog->file_count - 1) {
        dialog->selected_index++;
        /* Auto-scroll (show 16 items at a time) */
        if (dialog->selected_index >= dialog->scroll_offset + 16)
            dialog->scroll_offset = dialog->selected_index - 15;
    }
}

void file_dialog_render_text(file_dialog_t *dialog) {
    if (!dialog || !dialog->visible) return;

    printf("\n");
    printf("+--- %s ---+\n", dialog->title);
    printf("| Path: %s\n", dialog->current_path);
    printf("|-----------------------------------\n");

    int visible_start = dialog->scroll_offset;
    int visible_end = visible_start + 16;
    if (visible_end > dialog->file_count) visible_end = dialog->file_count;

    if (dialog->file_count == 0) {
        printf("| (empty directory)\n");
    }

    for (int i = visible_start; i < visible_end; i++) {
        fd_file_entry_t *f = &dialog->files[i];
        const char *marker = (i == dialog->selected_index) ? " >> " : "    ";
        if (f->is_directory) {
            printf("|%s[DIR]  %s/\n", marker, f->name);
        } else {
            printf("|%s%6d  %s\n", marker, f->size, f->name);
        }
    }

    if (dialog->file_count > 16) {
        printf("| (%d more items...)\n", dialog->file_count - visible_end);
    }

    printf("|-----------------------------------\n");
    if (dialog->type == FD_TYPE_SAVE) {
        printf("| Filename: %s\n", dialog->filename_input);
    }
    printf("| [Enter]=Select [Backspace]=Up [Esc]=Cancel\n");
    printf("+-----------------------------------+\n");
}
