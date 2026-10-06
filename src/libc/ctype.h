#ifndef KERNEL_LIBC_CTYPE_H
#define KERNEL_LIBC_CTYPE_H

/*
 * Freestanding <ctype.h>.
 *
 * src/libc_shim.c implements the glibc ctype ABI on purpose:
 * __ctype_b_loc()/__ctype_tolower_loc()/__ctype_toupper_loc() backed by
 * tables carrying glibc's _ISxxx class bits, plus the plain is*()/to*()
 * functions.  Reproducing glibc's macro interface here means vendored code
 * (Lua's lstrlib.c and friends) classifies characters exactly as it did in
 * the Linux build that used the host <ctype.h>.
 */

enum {
    _ISupper  = 0x0100,
    _ISlower  = 0x0200,
    _ISalpha  = 0x0400,
    _ISdigit  = 0x0800,
    _ISxdigit = 0x1000,
    _ISspace  = 0x2000,
    _ISprint  = 0x4000,
    _ISgraph  = 0x8000,
    _ISblank  = 0x0001,
    _IScntrl  = 0x0002,
    _ISpunct  = 0x0004,
    _ISalnum  = 0x0008
};

extern const unsigned short int **__ctype_b_loc(void);
extern const int **__ctype_tolower_loc(void);
extern const int **__ctype_toupper_loc(void);

/* The tables are offset by 128 entries so that negative values (e.g. EOF)
 * are valid indexes, exactly like glibc. */
#define __isctype(c, type) \
    ((*__ctype_b_loc())[(int)(c)] & (unsigned short int)(type))

#define isalnum(c)  __isctype((c), _ISalnum)
#define isalpha(c)  __isctype((c), _ISalpha)
#define isblank(c)  __isctype((c), _ISblank)
#define iscntrl(c)  __isctype((c), _IScntrl)
#define isdigit(c)  __isctype((c), _ISdigit)
#define isgraph(c)  __isctype((c), _ISgraph)
#define islower(c)  __isctype((c), _ISlower)
#define isprint(c)  __isctype((c), _ISprint)
#define ispunct(c)  __isctype((c), _ISpunct)
#define isspace(c)  __isctype((c), _ISspace)
#define isupper(c)  __isctype((c), _ISupper)
#define isxdigit(c) __isctype((c), _ISxdigit)

#define tolower(c) ((int)(*__ctype_tolower_loc())[(int)(c)])
#define toupper(c) ((int)(*__ctype_toupper_loc())[(int)(c)])

#endif /* KERNEL_LIBC_CTYPE_H */
