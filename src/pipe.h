/**
 * @file pipe.h
 * @brief Kernel pipe implementation for inter-process communication
 *
 * Provides unidirectional data channels (pipes) that can be used for IPC.
 * Each pipe has a read end and a write end with a circular buffer.
 */

#ifndef PIPE_H
#define PIPE_H

#include "types.h"
#include "atomic.h"

/** @brief Default pipe buffer size (4KB, one page) */
#define PIPE_BUF_SIZE       4096

/** @brief Maximum number of concurrent pipes in the system */
#define MAX_PIPES           64

/** @brief File descriptor type flags (stored in file_descriptor_t.fd_type) */
#define FD_TYPE_FILE        0   /**< Regular file (FAT32, etc.) */
#define FD_TYPE_PIPE_READ   1   /**< Read end of a pipe */
#define FD_TYPE_PIPE_WRITE  2   /**< Write end of a pipe */
#define FD_TYPE_SOCKET      3   /**< Network socket (TCP or UDP) */

/**
 * @brief Pipe structure with circular buffer
 *
 * A pipe is a unidirectional IPC channel. Data written to the write end
 * appears at the read end in FIFO order. The buffer is a fixed-size
 * circular buffer protected by a spinlock.
 */
typedef struct pipe {
    uint8_t *buffer;            /**< Circular buffer for data */
    uint32_t buf_size;          /**< Buffer capacity in bytes */
    uint32_t read_pos;          /**< Current read position (head) */
    uint32_t write_pos;         /**< Current write position (tail) */
    uint32_t count;             /**< Number of bytes currently in buffer */
    spinlock_t lock;            /**< Spinlock for concurrent access */
    int readers;                /**< Number of open read endpoints */
    int writers;                /**< Number of open write endpoints */
    bool active;                /**< Whether this pipe slot is in use */
    void *waiting_reader;       /**< thread_t* blocked in read */
    void *waiting_writer;       /**< thread_t* blocked in write */
} pipe_t;

/**
 * @brief Initialize the pipe subsystem
 *
 * Must be called once during kernel initialization before any pipe
 * operations are attempted.
 */
void pipe_init(void);

/**
 * @brief Create a new pipe
 *
 * Allocates a pipe structure and its buffer from kernel memory.
 *
 * @return Pointer to the new pipe, or NULL on failure
 */
pipe_t* pipe_create(void);

/**
 * @brief Destroy a pipe and free its resources
 *
 * Only destroys the pipe if both reader and writer counts are zero.
 *
 * @param pipe Pipe to destroy
 */
void pipe_destroy(pipe_t *pipe);

/**
 * @brief Read data from a pipe
 *
 * Reads up to @p count bytes from the pipe's read end into @p buf.
 * If the pipe is empty and writers still exist, returns -EAGAIN.
 * If the pipe is empty and no writers exist, returns 0 (EOF).
 *
 * @param pipe   Pipe to read from
 * @param buf    Destination buffer
 * @param count  Maximum number of bytes to read
 * @return Number of bytes read, 0 on EOF, -EAGAIN if empty, -1 on error
 */
int pipe_read(pipe_t *pipe, void *buf, size_t count);

/**
 * @brief Write data to a pipe
 *
 * Writes up to @p count bytes from @p buf into the pipe's write end.
 * If the pipe is full, returns -EAGAIN. If no readers exist, returns -1 (EPIPE).
 *
 * @param pipe   Pipe to write to
 * @param buf    Source buffer
 * @param count  Number of bytes to write
 * @return Number of bytes written, -EAGAIN if full, -1 on error (broken pipe)
 */
int pipe_write(pipe_t *pipe, const void *buf, size_t count);

/**
 * @brief Close the read end of a pipe
 *
 * Decrements the reader count. If both readers and writers are zero,
 * the pipe is automatically destroyed.
 *
 * @param pipe Pipe to close read end of
 */
void pipe_close_read(pipe_t *pipe);

/**
 * @brief Close the write end of a pipe
 *
 * Decrements the writer count. If both readers and writers are zero,
 * the pipe is automatically destroyed.
 *
 * @param pipe Pipe to close write end of
 */
void pipe_close_write(pipe_t *pipe);

/** After fork(): second process shares the same pipe end. */
void pipe_fork_dup_reader(pipe_t *pipe);
void pipe_fork_dup_writer(pipe_t *pipe);
void pipe_set_waiting_reader(pipe_t *pipe, void *thr);
void pipe_set_waiting_writer(pipe_t *pipe, void *thr);

/**
 * @brief Get the number of bytes available to read from a pipe
 *
 * @param pipe Pipe to query
 * @return Number of bytes available, or -1 on error
 */
int pipe_available(pipe_t *pipe);

#endif /* PIPE_H */
