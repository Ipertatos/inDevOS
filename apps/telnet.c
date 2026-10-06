/*
 * telnet.c  –  inDevOS Telnet client (Ring 3 ELF)
 *
 * Usage:  telnet <ip>            (connects to port 23)
 *         telnet <ip> <port>     (connects to custom port)
 *
 * Implements RFC 854 Telnet: negotiates options with the server (refusing
 * everything except SGA / ECHO) and relays keyboard ↔ socket I/O.
 *
 * Built with: bash build_userapp.sh apps/telnet.c telnet.exe
 */

#include "../include/syscalls.h"

/* ── Telnet IAC bytes ───────────────────────────────────────────────────── */
#define IAC    255
#define SE     240
#define SB     250
#define WILL   251
#define WONT   252
#define DO     253
#define DONT   254

#define TELOPT_ECHO      1
#define TELOPT_SGA       3

/* ── Minimal stdio-like helpers (no libc available) ─────────────────────── */

/* 'print' is already defined in syscalls.h as the SYS_PRINT wrapper */

static void print_char(char c) {
    char buf[2] = { c, '\0' };
    write(STDOUT_FILENO, buf, 1);
}

static int str_eq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return (*a == '\0' && *b == '\0');
}

/* Minimal decimal integer parser */
static int parse_int(const char *s) {
    int n = 0;
    while (*s >= '0' && *s <= '9') n = n * 10 + (*s++ - '0');
    return n;
}

/* Parse "d.d.d.d" into ip[4]; returns 0 on success */
static int parse_ip(const char *s, uint8_t ip[4]) {
    int i = 0, v = 0;
    for (int pos = 0; ; pos++) {
        char c = s[pos];
        if (c >= '0' && c <= '9') {
            v = v * 10 + (c - '0');
        } else if (c == '.' || c == '\0') {
            if (i > 3) return -1;
            ip[i++] = (uint8_t)v;
            v = 0;
            if (c == '\0') break;
        } else {
            return -1;
        }
    }
    return (i == 4) ? 0 : -1;
}

/* Print a decimal number */
static void print_int(int n) {
    if (n < 0) { print("-"); n = -n; }
    char buf[12]; int i = 11;
    buf[i] = '\0';
    if (n == 0) { print("0"); return; }
    while (n > 0) { buf[--i] = (char)('0' + n % 10); n /= 10; }
    print(buf + i);
}

/* ── IAC negotiation: reply buffer ─────────────────────────────────────── */
static uint8_t iac_reply[3];

static void reply_option(int sock_fd, uint8_t response, uint8_t opt) {
    iac_reply[0] = IAC;
    iac_reply[1] = response;
    iac_reply[2] = opt;
    send(sock_fd, iac_reply, 3, 0);
}

/* ── Main ────────────────────────────────────────────────────────────────── */

/* We need a _start since we're -nostdlib */
void _start(int argc, char *argv[]) {
    /* ── Argument parsing ── */
    if (argc < 2) {
        print("Usage: telnet <ip> [port]\n");
        exit(1);
    }

    uint8_t ip[4];
    if (parse_ip(argv[1], ip) != 0) {
        print("telnet: invalid IP address '");
        print(argv[1]);
        print("'\n");
        exit(1);
    }
    int port = (argc >= 3) ? parse_int(argv[2]) : 23;
    if (port <= 0 || port > 65535) port = 23;

    /* ── Connect ── */
    print("Trying ");
    print(argv[1]);
    print(":");
    print_int(port);
    print("...\n");

    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        print("telnet: socket() failed\n");
        exit(1);
    }

    sockaddr_in_t addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    /* sin_addr: kernel expects little-endian bytes (ip[0] at lowest byte) */
    addr.sin_addr = (uint32_t)ip[0]
                  | ((uint32_t)ip[1] << 8)
                  | ((uint32_t)ip[2] << 16)
                  | ((uint32_t)ip[3] << 24);

    if (connect(fd, &addr, sizeof(addr)) != 0) {
        print("telnet: connection refused or timed out\n");
        close(fd);
        exit(1);
    }

    print("Connected to ");
    print(argv[1]);
    print(".\n");
    print("Escape character is '^]'.\n");

    /* ── Event loop ── */
    typedef enum { S_NORMAL, S_IAC, S_OPT } state_t;
    state_t state   = S_NORMAL;
    uint8_t iac_cmd = 0;

    /* Receive and keyboard buffers */
    uint8_t rbuf[256];
    uint8_t kbuf[64];
    uint8_t line[256];
    int line_len = 0;

    int running = 1;
    while (running) {
        /* ── Check for keyboard input (non-blocking SYS_KB_READ) ── */
        int kn = (int)syscall(SYS_KB_READ, (uint64_t)kbuf, sizeof(kbuf), 0, 0, 0);
        for (int i = 0; i < kn && running; i++) {
            uint8_t k = kbuf[i];

            /* Ctrl+] → disconnect */
            if (k == 29) {
                print("\nConnection closed.\n");
                running = 0;
                break;
            }

            if (k == '\r' || k == '\n') {
                /* Send buffered line + CRLF */
                if (line_len > 0) send(fd, line, (uint32_t)line_len, 0);
                send(fd, (const uint8_t *)"\r\n", 2, 0);
                print_char('\n');  /* echo newline locally */
                line_len = 0;
            } else if (k == 127 || k == 8) {
                /* Backspace */
                if (line_len > 0) {
                    line_len--;
                    print_char('\b');
                    print_char(' ');
                    print_char('\b');
                }
            } else if (k >= 32 && k < 127) {
                /* Printable – echo locally and buffer */
                if (line_len < (int)sizeof(line) - 1) {
                    line[line_len++] = k;
                    print_char((char)k);
                }
            }
        }

        /* ── Receive from server (non-blocking: try once) ── */
        /* We use a short blocking recv with timeout handled in kernel */
        /* For non-blocking behaviour we peek: attempt recv with 0 as
         * a hint – kernel returns immediately if nothing available.   */
        int rn = recv(fd, rbuf, sizeof(rbuf), 0);
        if (rn < 0) {
            print("\nConnection reset.\n");
            running = 0;
            break;
        }

        for (int i = 0; i < rn && running; i++) {
            uint8_t c = rbuf[i];

            switch (state) {
            case S_NORMAL:
                if (c == IAC) {
                    state = S_IAC;
                } else if (c == '\r') {
                    /* Eat bare \r (will follow \n) */
                } else {
                    print_char((char)c);
                }
                break;

            case S_IAC:
                if (c == IAC) {
                    print_char((char)0xFF);   /* escaped literal */
                    state = S_NORMAL;
                } else if (c == WILL || c == WONT || c == DO || c == DONT) {
                    iac_cmd = c;
                    state   = S_OPT;
                } else {
                    state = S_NORMAL;  /* single-byte command – ignore */
                }
                break;

            case S_OPT: {
                uint8_t opt = c;
                if (iac_cmd == DO) {
                    /* Server asks us to enable opt */
                    /* We agree to suppress go-ahead; refuse everything else */
                    if (opt == TELOPT_SGA) reply_option(fd, WILL, opt);
                    else                   reply_option(fd, WONT, opt);
                } else if (iac_cmd == DONT) {
                    reply_option(fd, WONT, opt);
                } else if (iac_cmd == WILL) {
                    /* Server offers to enable opt – accept ECHO and SGA */
                    if (opt == TELOPT_ECHO || opt == TELOPT_SGA)
                        reply_option(fd, DO, opt);
                    else
                        reply_option(fd, DONT, opt);
                } else { /* WONT */
                    reply_option(fd, DONT, opt);
                }
                state = S_NORMAL;
                break;
            }
            } /* switch */
        } /* for rn */

        /* Small yield to avoid burning the CPU */
        syscall(SYS_SLEEP, 0, 0, 0, 0, 0);   /* 0-second sleep = one tick yield */
    }

    close(fd);
    exit(0);
}
