#!/usr/bin/env python3
"""Framebuffer main loop: stop idling while work is pending (inDevOS batch 21).

1. schedule.c: schedule_run() returned the time to the next callback measured
   from when it *started*, so a callback that rescheduled itself with 0 ms
   (convert_xml_to_box, qjs timers, fetch callbacks) came back as "wait N ms"
   where N was how long the callbacks had just run.  On inDevOS a poll()
   timeout > 2.5 ms sleeps through the scheduler and wakes 20-300 ms late, so
   box construction / image fetch dispatch ran one slice per oversleep
   (Wikipedia: ~70% of the load spent asleep with work queued).  The timeout
   is now measured from the current time (0 when already due, rounded up
   otherwise), and the list is kept sorted by deadline so equal-deadline
   callbacks (setTimeout(f, 0) chains) run in FIFO order instead of LIFO.
2. gui.c + libnsfb wm surface: while fetches are active the wait also polls
   the fetchers' sockets (fetch_fdset), so arriving network data wakes the
   browser at once instead of waiting for the 10 ms fetch poll timer (which
   the kernel also overshoots).  fetch_fdset() services the fetchers first;
   if that queued redraws or due callbacks the wait does not block.

Usage: fb_event_loop.py <path/to/netsurf-all-3.11>   (idempotent)"""
import os
import sys

if len(sys.argv) != 2:
    sys.exit(__doc__)
R = os.path.join(sys.argv[1], 'netsurf', 'frontends', 'framebuffer')
TAG = 'fb_event_loop'


def edit(path, pairs):
    p = os.path.join(R, path)
    s = open(p).read()
    if TAG in s:
        return
    for old, new in pairs:
        if s.count(old) != 1:
            sys.exit('fb_event_loop: anchor not found in %s: %r' % (path, old[:70]))
        s = s.replace(old, new)
    open(p, 'w').write(s)


SCHED_ADD_OLD = '''        /* add to list front */
        nscb->next = schedule_list;
        schedule_list = nscb;
'''
SCHED_ADD_NEW = '''	/* fb_event_loop: keep the list sorted by deadline; equal deadlines
	 * stay in the order they were scheduled (FIFO, like setTimeout) */
	{
		struct nscallback **pp = &schedule_list;
		while ((*pp != NULL) && !timercmp(&nscb->tv, &(*pp)->tv, <))
			pp = &(*pp)->next;
		nscb->next = *pp;
		*pp = nscb;
	}
'''
RUN_START = '/* exported function documented in framebuffer/schedule.h */\nint schedule_run(void)\n{'
RUN_END = '''        return (rettime.tv_sec * 1000) + (rettime.tv_usec / 1000);
}
'''
RUN_NEW = '''/* exported function documented in framebuffer/schedule.h */
int schedule_run(void)
{
	struct timeval tv;
        struct nscallback *unlnk_nscb;

	gettimeofday(&tv, NULL);

	/* fb_event_loop: the list is sorted, so run callbacks from the front
	 * while they were due on entry.  Callbacks they schedule with 0 ms
	 * run on the next call, after the frontend has polled input/fetches. */
	while ((schedule_list != NULL) && timercmp(&tv, &schedule_list->tv, >)) {
                        unlnk_nscb = schedule_list;
                        schedule_list = unlnk_nscb->next;
                        unlnk_nscb->callback(unlnk_nscb->p);
                        free(unlnk_nscb);
	}

	return schedule_next_ms();
}

/* exported function documented in framebuffer/schedule.h */
int schedule_next_ms(void)
{
	struct timeval tv;
	struct timeval rettime;

        if (schedule_list == NULL)
                return -1; /* no more callbacks scheduled */

	/* measured from now: the callbacks above may have run for a while */
	gettimeofday(&tv, NULL);
	if (!timercmp(&schedule_list->tv, &tv, >))
		return 0;
	timersub(&schedule_list->tv, &tv, &rettime);

	/* round up so the caller does not wake early and spin (24 days max) */
        return (rettime.tv_sec * 1000) + ((rettime.tv_usec + 999) / 1000);
}
'''


def sched():
    p = os.path.join(R, 'schedule.c')
    s = open(p).read()
    if TAG in s:
        return
    if s.count(SCHED_ADD_OLD) != 1:
        sys.exit('fb_event_loop: schedule.c add anchor missing')
    s = s.replace(SCHED_ADD_OLD, SCHED_ADD_NEW)
    a = s.find(RUN_START)
    b = s.find(RUN_END, a)
    if a < 0 or b < 0:
        sys.exit('fb_event_loop: schedule_run anchors missing')
    s = s[:a] + RUN_NEW + s[b + len(RUN_END):]
    open(p, 'w').write(s)
    h = os.path.join(R, 'schedule.h')
    hs = open(h).read()
    if 'schedule_next_ms' not in hs:
        hs = hs.replace('int schedule_run(void);', 'int schedule_run(void);\n\n/** Time in ms until the next scheduled callback, from now (0 = due, -1 = none). fb_event_loop */\nint schedule_next_ms(void);', 1)
        if 'schedule_next_ms' not in hs:
            sys.exit('fb_event_loop: schedule.h anchor missing')
        open(h, 'w').write(hs)


GUI_RUN_OLD = 'static void framebuffer_run(void)\n{'
GUI_RUN_NEW = '''/* fb_event_loop: libnsfb wm surface hook -- extra fds for its input wait */
extern int (*nsfb_wm_wait_fds)(struct pollfd *pfd, int max, int *timeout);

/* Services the fetchers and returns their sockets so network data ends the
 * wait.  Lowers *timeout to 0 if that produced redraws or due callbacks. */
static int fb_fetch_wait_fds(struct pollfd *pfd, int max, int *timeout)
{
	fd_set rs, ws, es;
	int maxfd = -1, n = 0, fd, next;

	if (fetch_fdset(&rs, &ws, &es, &maxfd) != NSERROR_OK)
		return 0;
	if (fbtk_get_redraw_pending(fbtk)) {
		*timeout = 0;
	} else {
		next = schedule_next_ms();
		if ((next >= 0) && ((*timeout < 0) || (next < *timeout)))
			*timeout = next;
	}
	for (fd = 0; (fd <= maxfd) && (n < max); fd++) {
		short ev = (FD_ISSET(fd, &rs) ? POLLIN : 0) |
			   (FD_ISSET(fd, &ws) ? POLLOUT : 0) |
			   (FD_ISSET(fd, &es) ? POLLPRI : 0);
		if (ev) {
			pfd[n].fd = fd;
			pfd[n].events = ev;
			pfd[n].revents = 0;
			n++;
		}
	}
	return n;
}

static void framebuffer_run(void)
{'''
GUI_LOOP_OLD = '''	while (fb_complete != true) {
		/* run the scheduler'''
GUI_LOOP_NEW = '''	nsfb_wm_wait_fds = fb_fetch_wait_fds;

	while (fb_complete != true) {
		/* run the scheduler'''
GUI_INC_OLD = '#include <stdlib.h>\n#include <nsutils/time.h>'
GUI_INC_NEW = '#include <stdlib.h>\n#include <poll.h>\n#include <sys/select.h>\n#include <nsutils/time.h>'


sched()
edit('gui.c', [(GUI_INC_OLD, GUI_INC_NEW), (GUI_RUN_OLD, GUI_RUN_NEW), (GUI_LOOP_OLD, GUI_LOOP_NEW)])
print('fb_event_loop: applied')
