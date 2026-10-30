/**
 * @file kpoll.h
 * @brief Event wakeups for poll/epoll/eventfd/timerfd/signalfd/inotify waits.
 *
 * One global event sequence.  Every state change a poller may wait for
 * (pipe data/room/close, AF_UNIX traffic, eventfd counter, inotify event,
 * posted signal, timerfd re-arm, epoll_ctl) calls kpoll_wake(), which bumps
 * the sequence and wakes every thread sleeping in kpoll_sleep_ms().  A waiter
 * snapshots the sequence BEFORE it evaluates readiness and sleeps only if
 * it is unchanged (checked under the scheduler lock), so no wakeup is lost.
 * Waits whose readiness has no wakeup source (TCP/UDP, ttys, evdev, /dev/wm)
 * keep the 1-tick stepping of sys_wait_step_us().
 */
#ifndef KPOLL_H
#define KPOLL_H

#include "types.h"

extern volatile uint32_t kpoll_seq;

static inline uint32_t kpoll_snapshot(void) {
    return __atomic_load_n(&kpoll_seq, __ATOMIC_SEQ_CST);
}

/* Readiness evaluation hints for the wait that follows. */
typedef struct {
    bool     nonevent;   /* some fd has no wakeup source: step per tick */
    uint64_t next_us;    /* earliest timerfd expiry (ktime us), 0 = none */
} kpoll_hint_t;

static inline void kpoll_hint_timer(kpoll_hint_t *h, uint64_t at_us) {
    if (h && at_us && (!h->next_us || at_us < h->next_us)) h->next_us = at_us;
}

/* An event happened: wake all kpoll sleepers (any context, IRQ-safe). */
void kpoll_wake(void);
/* Sleep up to ms unless kpoll_seq != snap (thread.c). */
void kpoll_sleep_ms(uint32_t snap, uint64_t ms);
/* One wait step: until an event after snap, dl_us (0 = none) or the timer
 * hint tmr_us (0 = none), whichever first (syscall.c). */
void kpoll_wait_us(uint32_t snap, uint64_t dl_us, uint64_t tmr_us);

#endif
