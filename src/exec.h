/*
 * exec.h - Simple executable loader for inDevOS
 * Supports minimal custom header format and stub for ELF detection.
 *
 * Custom format:
 *   Offset 0: 4-byte magic "EXE!"
 *   Offset 4: 4-byte little-endian entry point offset from start of file
 *   Code/data follow immediately after optional header.
 */
#ifndef EXEC_H
#define EXEC_H

#include "types.h"

/* Try to load a program file from the currently mounted filesystem and execute
 * it in kernel context.
 * Returns 0 on success or negative error code on failure. */
int exec_file(const char *filename);

#endif // EXEC_H 