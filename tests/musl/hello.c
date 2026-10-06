/* Phase 1 Linux-ABI test: static musl "hello world".
 * Build: tests/musl/build_musl_tests.sh  (musl-gcc -static)
 * Run inside inDevOS: /bin/hello_musl */
#include <stdio.h>

int main(int argc, char **argv)
{
    printf("Hello from static musl on inDevOS! argc=%d argv[0]=%s\n",
           argc, argc > 0 ? argv[0] : "(null)");
    return 0;
}
