/*
 * crt0.c - minimal C runtime for inDevOS user programs.
 *
 * Linked into every app that defines main() (apps that provide their own
 * _start are linked without it).  The kernel's exec leaves the x86-64 process
 * initial stack as [rsp]=argc, [rsp+8]=argv[0]... and does NOT set RDI/RSI, so
 * argc/argv must be read before anything touches RSP - hence the naked stub.
 * (A normal C function's prologue moves RSP first and picked up garbage.)
 */
#include "syscalls.h"

extern int main(int argc, char **argv);

__attribute__((naked, noreturn)) void _start(void) {
    __asm__ volatile(
        "movq (%rsp), %rdi\n\t"     /* argc */
        "leaq 8(%rsp), %rsi\n\t"    /* argv */
        "andq $-16, %rsp\n\t"       /* 16-byte align for the call */
        "call app_main\n\t"         /* never returns */
        "ud2\n\t"
    );
}

void app_main(long argc, char **argv) {
    static char *fallback[] = { "app", (char *)0 };
    if (argc < 1 || argv == (char **)0 || argv[0] == (char *)0) {
        argc = 1;
        argv = fallback;
    }
    int rc = main((int)argc, argv);
    exit(rc);
    for (;;) { }
}
