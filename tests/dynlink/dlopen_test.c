/* dlopen() of /lib/libdltest.so (or argv[1]): symbol lookup, data, ctor, TLS,
 * dlerror() for a missing library, dlclose(). Prints DLOPEN_OK on success. */
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    const char *lib = argc > 1 ? argv[1] : "libdltest.so";
    void *h = dlopen(lib, RTLD_NOW);
    if (!h) { printf("DLOPEN_FAIL open %s: %s\n", lib, dlerror()); return 1; }
    int (*add)(int, int) = (int (*)(int, int))dlsym(h, "dlt_add");
    int (*ctor)(void) = (int (*)(void))dlsym(h, "dlt_ctor_ran");
    const char *(*name)(void) = (const char *(*)(void))dlsym(h, "dlt_name");
    int *val = (int *)dlsym(h, "dlt_value");
    if (!add || !ctor || !name || !val) { printf("DLOPEN_FAIL dlsym: %s\n", dlerror()); return 2; }
    int a1 = add(2, 3), a2 = add(2, 3);            /* TLS counter: 5 then 6 */
    if (a1 != 5 || a2 != 6 || *val != 1234 || !ctor() || strcmp(name(), "libdltest")) {
        printf("DLOPEN_FAIL values add=%d,%d val=%d ctor=%d name=%s\n", a1, a2, *val, ctor(), name());
        return 3;
    }
    void *bad = dlopen("libnonexistent_ostest.so", RTLD_NOW);
    const char *e0 = bad ? NULL : dlerror();
    if (bad || !e0) { printf("DLOPEN_FAIL missing lib opened\n"); return 4; }
    char e[64]; snprintf(e, sizeof(e), "%s", e0);   /* glibc frees the dlerror text later */
    int v = *val;                                  /* glibc unmaps the library at dlclose */
    if (dlclose(h) != 0) { printf("DLOPEN_FAIL dlclose\n"); return 5; }
    printf("DLOPEN_OK add=%d,%d val=%d ctor=1 missing='%.60s'\n", a1, a2, v, e);
    return 0;
}
