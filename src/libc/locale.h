#ifndef KERNEL_LIBC_LOCALE_H
#define KERNEL_LIBC_LOCALE_H

/*
 * Freestanding <locale.h>.
 *
 * Lua's llex.c/lobject.c call lua_getlocaledecpoint(), which uses
 * localeconv()->decimal_point.  src/libc_shim.c implements localeconv().
 *
 * NOTE: struct lconv must stay layout-compatible with the definition in
 * libc_shim.c (single member).
 */

#define LC_ALL      6
#define LC_COLLATE  3
#define LC_CTYPE    0
#define LC_MONETARY 4
#define LC_NUMERIC  1
#define LC_TIME     2

struct lconv {
    char *decimal_point;
};

struct lconv *localeconv(void);
char         *setlocale(int category, const char *locale);

#endif /* KERNEL_LIBC_LOCALE_H */
