/* Linked against libdltest.so at build time (DT_NEEDED): the dynamic loader
 * must find /lib/libdltest.so before main runs. Prints NEEDED_OK. */
#include <stdio.h>
extern int dlt_value;
int dlt_add(int, int);
int dlt_ctor_ran(void);
int main(void) {
    int r = dlt_add(40, 2);
    if (r != 42 || dlt_value != 1234 || !dlt_ctor_ran()) { printf("NEEDED_FAIL r=%d v=%d\n", r, dlt_value); return 1; }
    printf("NEEDED_OK r=%d v=%d ctor=1\n", r, dlt_value);
    return 0;
}
