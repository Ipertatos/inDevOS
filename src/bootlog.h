#ifndef BOOTLOG_H
#define BOOTLOG_H
#include "types.h"
#include <stdbool.h>

/* Persistent boot log.
 *
 * Every byte that goes to the serial console (including the [   s.us]
 * timestamps) is also kept in a RAM buffer from the first line of kmain.
 * Once the boot FAT volume is mounted, bootlog_attach() looks for the
 * preallocated files /BOOTLOG.TXT and /BOOTLOG.OLD in its root directory,
 * rotates TXT -> OLD, bumps the boot counter, and from then on writes the
 * buffer straight into the clusters of /BOOTLOG.TXT (raw sector writes +
 * device flush; no FAT or directory updates), so a hard power-off or triple
 * fault leaves the log readable on the stick. */

#define BOOTLOG_FILE_BYTES (1024u * 1024u)        /* size of the preallocated files */

void bootlog_putc(char c);                /* serial.c: every output byte */
void bootlog_attach(void);                /* after the boot volume is mounted (FAT32 /BOOTLOG.TXT) */
void bootlog_attach_usb(void);            /* after USB storage is up (exFAT INDEVOS_BOOTLOG.TXT) */
void bootlog_flush(void);                 /* write pending text now (not from IRQ) */
void bootlog_panic_puts(const char *s);   /* exception context: COM1 + RAM log, no locks */
void bootlog_flush_emergency(void);       /* exception context: best-effort file write */
void bootlog_tick(void);                  /* main loop: periodic flush */
void bootlog_stage(const char *name);     /* "[stage] name" + flush */
bool bootlog_attached(void);
void bootlog_status(void);                /* zeus "bootlog" */
#endif
