/**
 * test_driver.ko — minimal loadable kernel module for kmod testing.
 *
 * Load with:  driver load test_driver.ko
 *         or:  insmod test_driver.ko
 *
 * Prints "driver test" every 5 seconds from a background kernel thread.
 *
 * Must be built with -mcmodel=large -fno-pic -fno-plt (see Makefile) so
 * calls to kernel exports use absolute R_X86_64_64 relocs.  PC32/PLT32
 * cannot reach kernel symbols at 0xffffffff80000000 from kalloc() memory.
 */

typedef unsigned long long uint64_t;
typedef unsigned int       uint32_t;

/* Kernel symbols resolved by kmod at load time */
extern int  printf(const char *fmt, ...);
extern void *thread_create(const char *name, void (*entry)(void *), void *arg,
                           uint64_t stack_size, int priority);
extern void thread_sleep(uint64_t milliseconds);
extern void scheduler_add_thread(void *thread);

static volatile int test_driver_running;
static void *test_driver_thread_handle;

static void test_driver_thread(void *arg) {
    (void)arg;

    while (test_driver_running) {
        printf("driver test\n");
        thread_sleep(5000);
    }
}

int module_init(void) {
    test_driver_running = 1;

    test_driver_thread_handle = thread_create("test_drv", test_driver_thread,
                                              (void *)0, 4096, 1);
    if (!test_driver_thread_handle) {
        printf("test_driver: failed to create thread\n");
        test_driver_running = 0;
        return -1;
    }

    scheduler_add_thread(test_driver_thread_handle);

    printf("test_driver: loaded (printing every 5 seconds)\n");
    return 0;
}

void module_cleanup(void) {
    test_driver_running = 0;
    printf("test_driver: unloaded\n");
}
