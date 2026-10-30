/* Dynamically linked hello (musl libc.so via PT_INTERP /lib/ld-musl-x86_64.so.1).
 * Prints argc/argv, an environment variable, the address of main (shows the PIE
 * load bias) and AT_BASE (where the kernel put the interpreter). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>

static __thread int tls_counter = 40;

int main(int argc, char **argv) {
    tls_counter += 2;
    const char *p = getenv("PATH");
    char *dup = strdup("heap ok");
    printf("DYN_HELLO argc=%d argv0=%s tls=%d %s path=%s main=%p at_base=%#lx at_entry=%#lx\n",
           argc, argv[0], tls_counter, dup ? dup : "heap FAIL", p ? "set" : "unset",
           (void *)main, getauxval(AT_BASE), getauxval(AT_ENTRY));
    free(dup);
    return argc > 1 ? atoi(argv[1]) : 0;
}
