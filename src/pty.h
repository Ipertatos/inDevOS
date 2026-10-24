/* Minimal pty-like terminal state shared by the desktop terminal window
 * (kterm) and the Linux ABI ioctl/read paths.  Attached to both pipes of
 * the kterm shell (pipe_t.pty); fds on those pipes report isatty(). */
#ifndef KPTY_H
#define KPTY_H
#include "types.h"

#define KPTY_NCC 19
typedef struct kpty {
    uint32_t iflag, oflag, cflag, lflag;   /* Linux termios bits */
    uint8_t  cc[KPTY_NCC];                  /* Linux c_cc[] (VTIME 5, VMIN 6) */
    uint16_t rows, cols;                    /* TIOCGWINSZ */
    int      pgid;                          /* TIOCSPGRP value (0 = caller's) */
} kpty_t;

#define KPTY_ISIG   0x0001
#define KPTY_ICANON 0x0002
#define KPTY_ECHO   0x0008
#define KPTY_ICRNL  0x0100
#define KPTY_VINTR  0
#define KPTY_VERASE 2
#define KPTY_VTIME  5
#define KPTY_VMIN   6

static inline void kpty_default(kpty_t *t) {
    static const uint8_t cc[KPTY_NCC] = { 3, 28, 127, 21, 4, 0, 1, 0, 17, 19, 26,
                                          0, 18, 15, 23, 22, 0, 0, 0 };
    t->iflag = 0x100 | 0x400;                 /* ICRNL | IXON */
    t->oflag = 0x1 | 0x4;                     /* OPOST | ONLCR */
    t->cflag = 0xF | 0x30 | 0x80;             /* B38400 | CS8 | CREAD */
    t->lflag = 0x1 | 0x2 | 0x8 | 0x10 | 0x20 | 0x8000; /* ISIG ICANON ECHO ECHOE ECHOK IEXTEN */
    for (int i = 0; i < KPTY_NCC; i++) t->cc[i] = cc[i];
    t->pgid = 0;
}
#endif
