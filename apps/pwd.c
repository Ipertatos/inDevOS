/*
 * pwd - print the current working directory.
 * Real user program: uses getcwd (the old version just printed "/").
 */
#include "syscalls.h"

int main(int argc, char **argv) {
    char cwd[256];
    (void)argc; (void)argv;
    if (!getcwd(cwd, sizeof(cwd))) {
        printf("pwd: cannot determine current directory\n");
        return 1;
    }
    printf("%s\n", cwd);
    return 0;
}
