#ifndef KERNEL_LIBC_SIGNAL_H
#define KERNEL_LIBC_SIGNAL_H

/*
 * Freestanding <signal.h>.
 *
 * Only sig_atomic_t is actually required: Lua's lstate.h includes <signal.h>
 * to define l_signalT.  No signal handling exists in the kernel, so the
 * declarations below exist for source compatibility only.
 */

typedef int sig_atomic_t;

typedef void (*sighandler_t)(int);

#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIG_ERR ((sighandler_t)-1)

#define SIGHUP  1
#define SIGINT  2
#define SIGQUIT 3
#define SIGILL  4
#define SIGABRT 6
#define SIGFPE  8
#define SIGKILL 9
#define SIGSEGV 11
#define SIGTERM 15

sighandler_t signal(int signum, sighandler_t handler);
int raise(int sig);

#endif /* KERNEL_LIBC_SIGNAL_H */
