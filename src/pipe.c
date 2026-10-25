/**
 * @file pipe.c
 * @brief Kernel pipe implementation for inter-process communication
 *
 * Implements unidirectional data channels using circular buffers.
 * Pipes are the fundamental IPC mechanism in Unix-like systems.
 */

#include "pipe.h"
#include "mem.h"
#include "serial.h"
#include "utils.h"
#include "string.h"
#include "../include/indevos_errno.h"

/** @brief Global pipe table */
static pipe_t pipe_table[MAX_PIPES];

/** @brief Lock for pipe table allocation */
static spinlock_t pipe_table_lock = {0};


void pipe_init(void) {
    memset(pipe_table, 0, sizeof(pipe_table));
    serial_puts("pipe: subsystem initialized\n");
}

pipe_t* pipe_create(void) {
    spinlock_lock(&pipe_table_lock);

    /* Find a free slot in the pipe table */
    pipe_t *pipe = NULL;
    for (int i = 0; i < MAX_PIPES; i++) {
        if (!pipe_table[i].active) {
            pipe = &pipe_table[i];
            break;
        }
    }

    if (!pipe) {
        spinlock_unlock(&pipe_table_lock);
        serial_puts("pipe: no free pipe slots\n");
        return NULL;
    }

    /* Allocate the circular buffer */
    pipe->buffer = (uint8_t *)kalloc(PIPE_BUF_SIZE);
    if (!pipe->buffer) {
        spinlock_unlock(&pipe_table_lock);
        serial_puts("pipe: failed to allocate buffer\n");
        return NULL;
    }

    memset(pipe->buffer, 0, PIPE_BUF_SIZE);
    pipe->buf_size  = PIPE_BUF_SIZE;
    pipe->read_pos  = 0;
    pipe->write_pos = 0;
    pipe->count     = 0;
    pipe->lock      = (spinlock_t){0};
    pipe->readers   = 1;
    pipe->writers   = 1;
    pipe->active    = true;
    pipe->waiting_reader = NULL;
    pipe->waiting_writer = NULL;
    pipe->pty = NULL;

    spinlock_unlock(&pipe_table_lock);

    return pipe;
}

void pipe_destroy(pipe_t *pipe) {
    if (!pipe || !pipe->active) return;
    { extern void fifo_pipe_gone(pipe_t *); fifo_pipe_gone(pipe); }  /* named FIFO registry */

    spinlock_lock(&pipe_table_lock);

    if (pipe->buffer) {
        kfree(pipe->buffer);
        pipe->buffer = NULL;
    }

    pipe->active    = false;
    pipe->buf_size  = 0;
    pipe->read_pos  = 0;
    pipe->write_pos = 0;
    pipe->count     = 0;
    pipe->readers   = 0;
    pipe->writers   = 0;

    spinlock_unlock(&pipe_table_lock);

}

int pipe_read(pipe_t *pipe, void *buf, size_t count) {
    if (!pipe || !buf) return -1;

    spinlock_lock(&pipe->lock);
    if (!pipe->active || !pipe->buffer) {
        spinlock_unlock(&pipe->lock);
        return -1;
    }

    if (pipe->count == 0) {
        int writers = pipe->writers;
        spinlock_unlock(&pipe->lock);
        /* If no writers remain, this is EOF */
        if (writers == 0) return 0;
        return -EAGAIN;
    }

    /* Read as many bytes as available, up to the requested count */
    uint32_t to_read = (count < pipe->count) ? (uint32_t)count : pipe->count;
    uint8_t *dst = (uint8_t *)buf;

    for (uint32_t i = 0; i < to_read; i++) {
        dst[i] = pipe->buffer[pipe->read_pos];
        pipe->read_pos = (pipe->read_pos + 1) % pipe->buf_size;
    }
    pipe->count -= to_read;
    void *ww = pipe->waiting_writer;
    pipe->waiting_writer = NULL;
    spinlock_unlock(&pipe->lock);
    if (ww) { extern void thread_wake(void *); thread_wake(ww); }
    return (int)to_read;
}

int pipe_write(pipe_t *pipe, const void *buf, size_t count) {
    if (!pipe || !buf) return -EPIPE;

    spinlock_lock(&pipe->lock);
    if (!pipe->active || !pipe->buffer) {
        spinlock_unlock(&pipe->lock);
        return -EPIPE;
    }
    /* Broken pipe: no readers left (pipe object stays alive while writers>0). */
    if (pipe->readers == 0) {
        spinlock_unlock(&pipe->lock);
        return -EPIPE;
    }

    uint32_t space = pipe->buf_size - pipe->count;
    if (space == 0) {
        spinlock_unlock(&pipe->lock);
        return -EAGAIN;
    }

    uint32_t to_write = (count < space) ? (uint32_t)count : space;
    const uint8_t *src = (const uint8_t *)buf;

    for (uint32_t i = 0; i < to_write; i++) {
        pipe->buffer[pipe->write_pos] = src[i];
        pipe->write_pos = (pipe->write_pos + 1) % pipe->buf_size;
    }
    pipe->count += to_write;
    void *wr = pipe->waiting_reader;
    pipe->waiting_reader = NULL;
    spinlock_unlock(&pipe->lock);
    if (wr) { extern void thread_wake(void *); thread_wake(wr); }
    return (int)to_write;
}


void pipe_close_read(pipe_t *pipe) {
    if (!pipe || !pipe->active) return;

    spinlock_lock(&pipe->lock);
    if (pipe->readers > 0) pipe->readers--;
    bool should_destroy = (pipe->readers == 0 && pipe->writers == 0);
    void *ww = pipe->waiting_writer;
    void *wr = pipe->waiting_reader;
    if (pipe->readers == 0) {
        pipe->waiting_writer = NULL;
        pipe->waiting_reader = NULL;
    }
    spinlock_unlock(&pipe->lock);
    if (ww) { extern void thread_wake(void *); thread_wake(ww); }
    if (wr) { extern void thread_wake(void *); thread_wake(wr); }

    if (should_destroy) {
        pipe_destroy(pipe);
    }
}


void pipe_close_write(pipe_t *pipe) {
    if (!pipe || !pipe->active) return;

    spinlock_lock(&pipe->lock);
    if (pipe->writers > 0) pipe->writers--;
    bool should_destroy = (pipe->readers == 0 && pipe->writers == 0);
    void *wr = pipe->waiting_reader;
    void *ww = pipe->waiting_writer;
    if (pipe->writers == 0) {
        pipe->waiting_reader = NULL;
        pipe->waiting_writer = NULL;
    }
    spinlock_unlock(&pipe->lock);
    if (wr) { extern void thread_wake(void *); thread_wake(wr); }
    if (ww) { extern void thread_wake(void *); thread_wake(ww); }

    if (should_destroy) {
        pipe_destroy(pipe);
    }
}


void pipe_fork_dup_reader(pipe_t *pipe) {
    if (!pipe || !pipe->active) return;
    spinlock_lock(&pipe->lock);
    pipe->readers++;
    spinlock_unlock(&pipe->lock);
}

void pipe_fork_dup_writer(pipe_t *pipe) {
    if (!pipe || !pipe->active) return;
    spinlock_lock(&pipe->lock);
    pipe->writers++;
    spinlock_unlock(&pipe->lock);
}

int pipe_available(pipe_t *pipe) {
    if (!pipe || !pipe->active) return -1;

    spinlock_lock(&pipe->lock);
    int avail = (int)pipe->count;
    spinlock_unlock(&pipe->lock);

    return avail;
}

void pipe_set_waiting_reader(pipe_t *pipe, void *thr) {
    if (!pipe || !pipe->active) return;
    spinlock_lock(&pipe->lock);
    pipe->waiting_reader = thr;
    spinlock_unlock(&pipe->lock);
}
void pipe_set_waiting_writer(pipe_t *pipe, void *thr) {
    if (!pipe || !pipe->active) return;
    spinlock_lock(&pipe->lock);
    pipe->waiting_writer = thr;
    spinlock_unlock(&pipe->lock);
}

/* Non-blocking variants for kernel main-loop callers (desktop terminal).
 * pipe->lock is a plain spinlock that a Ring-3 reader/writer can hold
 * while it is preempted; the non-preemptible main loop must never spin on
 * it, so these give up with -EAGAIN when the lock is busy. */
static bool pipe_trylock(pipe_t *pipe) {
    return __sync_lock_test_and_set(&pipe->lock.lock, 1) == 0;
}

int pipe_try_read(pipe_t *pipe, void *buf, size_t count) {
    if (!pipe || !buf) return -1;
    if (!pipe_trylock(pipe)) return -EAGAIN;
    if (!pipe->active || !pipe->buffer) {
        spinlock_unlock(&pipe->lock);
        return -1;
    }
    if (pipe->count == 0) {
        int writers = pipe->writers;
        spinlock_unlock(&pipe->lock);
        return writers == 0 ? 0 : -EAGAIN;
    }
    uint32_t to_read = (count < pipe->count) ? (uint32_t)count : pipe->count;
    uint8_t *dst = (uint8_t *)buf;
    for (uint32_t i = 0; i < to_read; i++) {
        dst[i] = pipe->buffer[pipe->read_pos];
        pipe->read_pos = (pipe->read_pos + 1) % pipe->buf_size;
    }
    pipe->count -= to_read;
    void *ww = pipe->waiting_writer;
    pipe->waiting_writer = NULL;
    spinlock_unlock(&pipe->lock);
    if (ww) { extern void thread_wake(void *); thread_wake(ww); }
    return (int)to_read;
}

int pipe_try_write(pipe_t *pipe, const void *buf, size_t count) {
    if (!pipe || !buf) return -EPIPE;
    if (!pipe_trylock(pipe)) return -EAGAIN;
    if (!pipe->active || !pipe->buffer || pipe->readers == 0) {
        spinlock_unlock(&pipe->lock);
        return -EPIPE;
    }
    uint32_t space = pipe->buf_size - pipe->count;
    if (space == 0) {
        spinlock_unlock(&pipe->lock);
        return -EAGAIN;
    }
    uint32_t to_write = (count < space) ? (uint32_t)count : space;
    const uint8_t *src = (const uint8_t *)buf;
    for (uint32_t i = 0; i < to_write; i++) {
        pipe->buffer[pipe->write_pos] = src[i];
        pipe->write_pos = (pipe->write_pos + 1) % pipe->buf_size;
    }
    pipe->count += to_write;
    void *wr = pipe->waiting_reader;
    pipe->waiting_reader = NULL;
    spinlock_unlock(&pipe->lock);
    if (wr) { extern void thread_wake(void *); thread_wake(wr); }
    return (int)to_write;
}
