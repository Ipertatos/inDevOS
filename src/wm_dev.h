/* wm_dev.h - /dev/wm window-server device (kernel side). See wm_abi.h. */
#ifndef WM_DEV_H
#define WM_DEV_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "desktop.h"

#define FD_TYPE_DEV_WM 12   /* /dev/wm connection */

struct process;
/* Linux-ABI file operations (linux_syscall.c / syscall.c dispatch here). */
long wm_dev_open(void **handle);
void wm_dev_release(void *handle);
long wm_dev_ioctl(void *handle, unsigned long req, void *arg);
long wm_dev_read(void *handle, void *ubuf, size_t n, bool nonblock);
long wm_dev_mmap(void *handle, uint64_t addr, uint64_t len, long prot, long flags, int64_t off);
bool wm_dev_poll_ready(void *handle);
bool wm_dev_pid_connected(uint32_t pid);   /* pid has an open /dev/wm */

/* desktop.c hooks.  All return quickly and never block on a client. */
bool wm_dev_owns_window(uint32_t desk_id);
void wm_dev_pointer(uint32_t desk_id, const desktop_ptr_event_t *ev);
bool wm_dev_key(uint32_t desk_id, int32_t keycode, bool pressed);   /* true = consumed */
bool wm_dev_close_request(uint32_t desk_id);                        /* true = consumed */
void wm_dev_resized(uint32_t desk_id, int w, int h);
void wm_dev_focus_update(uint32_t focused_desk_id);
#endif
