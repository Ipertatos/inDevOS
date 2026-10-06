/*
 * appio.c - the single definition of the shared stdout buffer.
 *
 * The printf/puts helpers live in include/syscalls.h so that every program gets
 * them for free, but they are inline: a buffer declared `static` in the header
 * would exist separately in each translation unit, and the copy that crt0's
 * exit() flushes would always be empty - output that ends in a newline (i.e.
 * almost all of it) would silently disappear.
 *
 * Linked into every program (see apps/build_apps.sh), including those that
 * bring their own _start.
 */
#include "syscalls.h"

#ifndef INDEVOS_UNBUFFERED_STDOUT
char   indevos_stdout_buf[INDEVOS_STDOUT_BUFSZ];
size_t indevos_stdout_len = 0;

void indevos_flush_stdout(void) {
    if (indevos_stdout_len == 0) return;
    size_t n = indevos_stdout_len;
    indevos_stdout_len = 0;          /* clear first: write() may fail */
    write(STDOUT_FILENO, indevos_stdout_buf, n);
}
#endif
