/* Single source of truth for syscall numbers (kernel + user). */
#ifndef INDEVOS_SYSCALL_NUMBERS_H
#define INDEVOS_SYSCALL_NUMBERS_H

#define SYS_EXIT         0
#define SYS_READ         1
#define SYS_WRITE        2
#define SYS_OPEN         3
#define SYS_CLOSE        4
#define SYS_SEEK         5
#define SYS_BRK          6
#define SYS_MMAP         7
#define SYS_MUNMAP       8
#define SYS_GETPID       9
#define SYS_FORK        10
#define SYS_EXEC        11
#define SYS_SLEEP       12
#define SYS_TIME        13
#define SYS_PRINT       14
#define SYS_GETCHAR     15
#define SYS_MKDIR       16
#define SYS_RMDIR       17
#define SYS_UNLINK      18
#define SYS_STAT        19
#define SYS_GETCWD      20
#define SYS_CHDIR       21
#define SYS_MOUNT       22
#define SYS_UNMOUNT     23
#define SYS_READDIR     24
#define SYS_NETWORK     25
#define SYS_WAIT        26
#define SYS_SIGNAL      27
#define SYS_KILL        28

/* SDL / desktop (29–49) */
#define SYS_SDL_INIT               29
#define SYS_SDL_QUIT               30
#define SYS_SDL_CREATE_WINDOW      31
#define SYS_SDL_DESTROY_WINDOW     32
#define SYS_SDL_CREATE_RENDERER    33
#define SYS_SDL_DESTROY_RENDERER   34
#define SYS_SDL_SET_DRAW_COLOR     35
#define SYS_SDL_RENDER_CLEAR       36
#define SYS_SDL_RENDER_PRESENT     37
#define SYS_SDL_POLL_EVENT         38
#define SYS_SDL_GET_TICKS          39
#define SYS_SDL_DELAY              40
#define SYS_SDL_RENDER_DRAW_RECT   41
#define SYS_SDL_RENDER_FILL_RECT   42
#define SYS_SDL_GET_ERROR          43
#define SYS_SDL_COPY_PIXELS        44
#define SYS_SDL_GET_KEYBOARD_STATE 45
#define SYS_SDL_GET_MOUSE_REL_ACCUM 46
#define SYS_SDL_GET_MOUSE_BUTTONS  47
#define SYS_SDL_GET_DISPLAY_SIZE     48
#define SYS_DESKTOP_EMBED_INFO       49

/* Sockets + poll/time (50–62) */
#define SYS_SOCKET        50
#define SYS_BIND          51
#define SYS_LISTEN        52
#define SYS_ACCEPT        53
#define SYS_CONNECT       54
#define SYS_SEND          55
#define SYS_RECV          56
#define SYS_SETSOCKOPT    57
#define SYS_GETSOCKOPT    58
#define SYS_SHUTDOWN      59
#define SYS_GETTIMEOFDAY  60
#define SYS_SELECT        61
#define SYS_POLL          62

/* Framebuffer / keyboard / control */
#define SYS_FB_CLEAR            63
#define SYS_FB_FILLRECT         64
#define SYS_FB_GET_WIDTH        65
#define SYS_FB_GET_HEIGHT       66
#define SYS_KB_READ             67
#define SYS_KB_CLEAR            68
#define SYS_GET_TERMINATE_FLAG  69
#define SYS_DESKTOP_COMPOSITE   70
#define SYS_SETUID              71
#define SYS_SETGID              72
#define SYS_GETUID              73
#define SYS_GETGID              74

/* Extended POSIX-style (avoid collision with SDL 29–49) */
#define SYS_PIPE           75
#define SYS_DUP            76
#define SYS_DUP2           77
#define SYS_FCNTL          78
#define SYS_IOCTL          79
#define SYS_SIGACTION      80
#define SYS_SIGPROCMASK    81
#define SYS_FSTAT          82
#define SYS_LSTAT          83
#define SYS_RENAME         84
#define SYS_LINK           85
#define SYS_SYMLINK        86
#define SYS_READLINK       87
#define SYS_CHMOD          88
#define SYS_FCHMOD         89
#define SYS_UMASK          90
#define SYS_CLOCK_GETTIME  91
#define SYS_NANOSLEEP      92
#define SYS_GETPPID        93
#define SYS_DESKTOP_POLL_EVENT 94
#define SYS_RTC_TIME       95
#define SYS_SENDTO         96
#define SYS_RECVFROM       97

#define SYS_MAX            98

#endif
