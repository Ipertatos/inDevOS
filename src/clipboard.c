/**
 * @file clipboard.c
 * @brief System clipboard implementation
 */

#include "clipboard.h"
#include "mem.h"
#include "serial.h"
#include "atomic.h"
#include "string.h"

/** Clipboard state */
static struct {
    bool initialized;
    clipboard_type_t type;
    uint8_t *data;          /**< Clipboard data buffer */
    size_t size;            /**< Current data size */
    size_t capacity;        /**< Buffer capacity */
    spinlock_t lock;
} clipboard = {0};

int clipboard_init(void) {
    clipboard.data = (uint8_t *)kalloc(CLIPBOARD_MAX_SIZE);
    if (!clipboard.data) {
        serial_puts("clipboard: failed to allocate buffer\n");
        return -1;
    }
    clipboard.capacity = CLIPBOARD_MAX_SIZE;
    clipboard.size = 0;
    clipboard.type = CLIPBOARD_EMPTY;
    clipboard.lock = (spinlock_t){0};
    clipboard.initialized = true;
    serial_puts("clipboard: initialized (64KB buffer)\n");
    return 0;
}

int clipboard_set_text(const char *text) {
    if (!clipboard.initialized || !text) return -1;

    spinlock_lock(&clipboard.lock);
    size_t len = strlen(text);
    if (len >= clipboard.capacity) len = clipboard.capacity - 1;
    memcpy(clipboard.data, text, len);
    clipboard.data[len] = '\0';
    clipboard.size = len + 1;
    clipboard.type = CLIPBOARD_TEXT;
    spinlock_unlock(&clipboard.lock);

    serial_printf("clipboard: copied %d bytes of text\n", (int)len);
    return 0;
}

int clipboard_set_data(const void *data, size_t size) {
    if (!clipboard.initialized || !data || size == 0) return -1;

    spinlock_lock(&clipboard.lock);
    if (size > clipboard.capacity) size = clipboard.capacity;
    memcpy(clipboard.data, data, size);
    clipboard.size = size;
    clipboard.type = CLIPBOARD_BINARY;
    spinlock_unlock(&clipboard.lock);

    return 0;
}

int clipboard_get_text(char *buf, size_t buflen) {
    if (!clipboard.initialized || !buf || buflen == 0) return -1;
    if (clipboard.type != CLIPBOARD_TEXT) return -1;

    spinlock_lock(&clipboard.lock);
    size_t copy_len = clipboard.size - 1;  /* exclude null terminator from count */
    if (copy_len >= buflen) copy_len = buflen - 1;
    memcpy(buf, clipboard.data, copy_len);
    buf[copy_len] = '\0';
    spinlock_unlock(&clipboard.lock);

    return (int)copy_len;
}

int clipboard_get_data(void *buf, size_t buflen) {
    if (!clipboard.initialized || !buf || buflen == 0) return -1;
    if (clipboard.type == CLIPBOARD_EMPTY) return -1;

    spinlock_lock(&clipboard.lock);
    size_t copy_len = clipboard.size;
    if (copy_len > buflen) copy_len = buflen;
    memcpy(buf, clipboard.data, copy_len);
    spinlock_unlock(&clipboard.lock);

    return (int)copy_len;
}

clipboard_type_t clipboard_get_type(void) {
    return clipboard.type;
}

size_t clipboard_get_size(void) {
    return clipboard.size;
}

void clipboard_clear(void) {
    if (!clipboard.initialized) return;
    spinlock_lock(&clipboard.lock);
    clipboard.type = CLIPBOARD_EMPTY;
    clipboard.size = 0;
    spinlock_unlock(&clipboard.lock);
}

bool clipboard_has_data(void) {
    return clipboard.initialized && clipboard.type != CLIPBOARD_EMPTY && clipboard.size > 0;
}
