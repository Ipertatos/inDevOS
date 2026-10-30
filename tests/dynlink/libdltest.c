/* Shared library for the dlopen / DT_NEEDED tests. */
#include <stdio.h>
#include <string.h>

int dlt_value = 1234;
static int ctor_ran;
static __thread int dlt_tls = 7;           /* dynamic TLS when dlopen'ed */

__attribute__((constructor)) static void dlt_ctor(void) { ctor_ran = 1; }

int dlt_ctor_ran(void) { return ctor_ran; }
int dlt_add(int a, int b) { return a + b + (dlt_tls++ - 7); }
const char *dlt_name(void) { static char b[32]; strcpy(b, "libdltest"); return b; }
