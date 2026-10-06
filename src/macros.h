#ifndef MACROS_H
#define MACROS_H

/* Canonical kernel-wide utility macros.  Include this header (directly or
 * via math.h) instead of redefining MIN/MAX/ALIGN_* locally. */

typedef char linker_symbol_ptr[];

#define ALIGN_UP(x, base)   ((((x) + (base) - 1) / (base)) * (base))
#define ALIGN_DOWN(x, base) (((x) / (base)) * (base))

#define EXTRACT_BITS(value, start_index, end_index) (((value) & (((1ul << ((end_index) - (start_index) + 1)) - 1) << (start_index))) >> (start_index))

#define DIV_ROUNDUP(x, y)   (((x) + (y) - 1) / (y))
#define DIV_ROUNDDOWN(x, y) ((x) / (y))

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

#endif /* MACROS_H */
