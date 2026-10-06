/**
 * @file file_dialog.h
 * @brief File open/save dialog for the desktop environment
 *
 * Provides modal file browser dialogs that integrate with the FAT32
 * filesystem for opening and saving files.
 */

#ifndef FILE_DIALOG_H
#define FILE_DIALOG_H

#include "types.h"

/** Maximum path length */
#define FD_MAX_PATH     256

/** Maximum filename length */
#define FD_MAX_FILENAME 128

/** Maximum files displayed in dialog */
#define FD_MAX_FILES    64

/** File dialog result */
typedef enum {
    FD_RESULT_NONE = 0,     /**< Dialog still open / not shown */
    FD_RESULT_OK,           /**< User selected a file */
    FD_RESULT_CANCEL,       /**< User cancelled */
    FD_RESULT_ERROR         /**< Error occurred */
} fd_result_t;

/** File dialog type */
typedef enum {
    FD_TYPE_OPEN = 0,       /**< Open file dialog */
    FD_TYPE_SAVE,           /**< Save file dialog */
    FD_TYPE_DIR             /**< Select directory dialog */
} fd_type_t;

/** File entry in the dialog listing */
typedef struct {
    char name[FD_MAX_FILENAME];
    uint32_t size;
    bool is_directory;
} fd_file_entry_t;

/** File dialog state */
typedef struct {
    fd_type_t type;
    fd_result_t result;
    char current_path[FD_MAX_PATH];
    char selected_file[FD_MAX_PATH];
    char filename_input[FD_MAX_FILENAME];
    char filter[32];                    /**< File extension filter (e.g. ".txt") */
    fd_file_entry_t files[FD_MAX_FILES];
    int file_count;
    int selected_index;
    int scroll_offset;
    bool visible;
    bool active;
    const char *title;
} file_dialog_t;

/**
 * @brief Initialize a file dialog
 * @param dialog    Dialog state structure
 * @param type      Dialog type (open/save/dir)
 * @param title     Dialog title
 * @param start_path Initial directory path
 * @param filter    File extension filter (NULL for all files)
 */
void file_dialog_init(file_dialog_t *dialog, fd_type_t type,
                      const char *title, const char *start_path,
                      const char *filter);

/**
 * @brief Show the file dialog and populate file list
 * @param dialog Dialog state
 */
void file_dialog_show(file_dialog_t *dialog);

/**
 * @brief Refresh the file listing for the current directory
 * @param dialog Dialog state
 */
void file_dialog_refresh(file_dialog_t *dialog);

/**
 * @brief Navigate into a directory
 * @param dialog Dialog state
 * @param dirname Directory name to enter
 */
void file_dialog_navigate(file_dialog_t *dialog, const char *dirname);

/**
 * @brief Navigate to parent directory
 * @param dialog Dialog state
 */
void file_dialog_go_up(file_dialog_t *dialog);

/**
 * @brief Select the currently highlighted item
 *
 * If it's a directory, navigates into it. If it's a file, sets result to OK.
 *
 * @param dialog Dialog state
 */
void file_dialog_select(file_dialog_t *dialog);

/**
 * @brief Cancel the file dialog
 * @param dialog Dialog state
 */
void file_dialog_cancel(file_dialog_t *dialog);

/**
 * @brief Get the full path of the selected file
 * @param dialog Dialog state
 * @param buf    Output buffer
 * @param buflen Buffer size
 * @return 0 on success, -1 if no file selected
 */
int file_dialog_get_path(file_dialog_t *dialog, char *buf, size_t buflen);

/**
 * @brief Move selection up
 * @param dialog Dialog state
 */
void file_dialog_select_prev(file_dialog_t *dialog);

/**
 * @brief Move selection down
 * @param dialog Dialog state
 */
void file_dialog_select_next(file_dialog_t *dialog);

/**
 * @brief Render the file dialog to the terminal (text-mode fallback)
 *
 * For use outside the graphical desktop environment, renders the dialog
 * as a text-based interface in the terminal.
 *
 * @param dialog Dialog state
 */
void file_dialog_render_text(file_dialog_t *dialog);

#endif /* FILE_DIALOG_H */
