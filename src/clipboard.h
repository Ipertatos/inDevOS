/**
 * @file clipboard.h
 * @brief System clipboard for copy/paste operations
 *
 * Provides a global clipboard buffer that can store text and binary data.
 * Used by the desktop environment and terminal for copy/paste.
 */

#ifndef CLIPBOARD_H
#define CLIPBOARD_H

#include "types.h"

/** Maximum clipboard data size (64 KB) */
#define CLIPBOARD_MAX_SIZE  (64 * 1024)

/** Clipboard data types */
typedef enum {
    CLIPBOARD_EMPTY = 0,
    CLIPBOARD_TEXT,         /**< Plain text (null-terminated) */
    CLIPBOARD_BINARY,       /**< Raw binary data */
} clipboard_type_t;

/**
 * @brief Initialize the clipboard system
 * @return 0 on success, -1 on error
 */
int clipboard_init(void);

/**
 * @brief Copy text to the clipboard
 * @param text   Null-terminated text string
 * @return 0 on success, -1 on error
 */
int clipboard_set_text(const char *text);

/**
 * @brief Copy binary data to the clipboard
 * @param data   Data buffer
 * @param size   Data size in bytes
 * @return 0 on success, -1 on error
 */
int clipboard_set_data(const void *data, size_t size);

/**
 * @brief Get text from the clipboard
 * @param buf    Output buffer
 * @param buflen Buffer size
 * @return Number of characters copied (excluding null), or -1 if clipboard is not text
 */
int clipboard_get_text(char *buf, size_t buflen);

/**
 * @brief Get binary data from the clipboard
 * @param buf    Output buffer
 * @param buflen Buffer size
 * @return Number of bytes copied, or -1 if clipboard is empty
 */
int clipboard_get_data(void *buf, size_t buflen);

/**
 * @brief Get the current clipboard data type
 * @return Clipboard type
 */
clipboard_type_t clipboard_get_type(void);

/**
 * @brief Get the size of clipboard data
 * @return Size in bytes, or 0 if empty
 */
size_t clipboard_get_size(void);

/**
 * @brief Clear the clipboard
 */
void clipboard_clear(void);

/**
 * @brief Check if clipboard has data
 * @return true if clipboard contains data
 */
bool clipboard_has_data(void);

#endif /* CLIPBOARD_H */
