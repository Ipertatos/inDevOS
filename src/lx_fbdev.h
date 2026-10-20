#ifndef LX_FBDEV_H
#define LX_FBDEV_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "syscall.h"
#define FD_TYPE_DEV_FB 9   /* /dev/fb0 (Linux fbdev) */
int  fbdev_open(process_t *p);
void fbdev_close(process_t *p);
long fbdev_ioctl(process_t *p, unsigned long req, void *arg);
long fbdev_read(process_t *p, uint32_t *pos, void *ubuf, size_t n);
long fbdev_write(process_t *p, uint32_t *pos, const void *ubuf, size_t n);
long fbdev_lseek(uint32_t *pos, int64_t off, long whence);
long fbdev_mmap(process_t *p, uint64_t addr, uint64_t len, long prot, long flags, int64_t off);
long fbdev_tty_ioctl(process_t *p, unsigned long req, void *arg, bool *handled);
bool fbdev_display_owned(void);   /* compositor/console must not paint */
void fbdev_tick(void);            /* main loop: present while owned */
#endif
