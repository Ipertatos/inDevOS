/*
 * telnet_server.c – kernel-internal Telnet server (RFC 854)
 *
 * Listens on TCP port 23.  Accepts one session at a time.  Incoming
 * characters are line-buffered and dispatched to zeus_process_command_service
 * when a newline arrives.  All output produced by Zeus (via term_write) is
 * mirrored to the TCP connection through the term output hook.
 *
 * IAC negotiation: we accept SGA and ECHO (so the client stops local echo)
 * and refuse every other option with WONT/DONT.
 */

#include "telnet_server.h"
#include "net_tcp.h"
#include "net_device.h"
#include "term.h"
#include "serial.h"
#include "string.h"
#include "string.h"
#include "mem.h"
#include "zeus.h"

/* ── Telnet command bytes (RFC 854) ─────────────────────────────────────── */
#define IAC    255
#define SE     240   /* sub-negotiation end          */
#define SB     250   /* sub-negotiation begin        */
#define WILL   251
#define WONT   252
#define DO     253
#define DONT   254

/* Telnet option codes */
#define TELOPT_ECHO      1
#define TELOPT_SGA       3   /* suppress go-ahead */
#define TELOPT_LINEMODE  34

/* ── Line-input buffer ───────────────────────────────────────────────────── */
#define LINE_BUF_SIZE  512
#define SEND_BUF_SIZE  4096
/* Queues bytes when tcp_send() cannot send (cwnd / peer window full). */
#define PENDING_RAW_MAX 8192

/* ── Server state ────────────────────────────────────────────────────────── */
static tcp_connection_t *active_conn    = NULL;  /* current live session  */

static char  line_buf[LINE_BUF_SIZE];            /* inbound line buffer   */
static int   line_len = 0;

/* IAC parser state */
typedef enum {
    IAC_NORMAL,
    IAC_SEEN,
    IAC_OPTION,
    IAC_SUBNEG,      /* inside SB … SE (option subnegotiation) */
    IAC_SUBNEG_IAC,  /* saw IAC in subneg; next byte is SE, IAC, or other */
} iac_state_t;
static iac_state_t iac_state = IAC_NORMAL;
static uint8_t     iac_cmd   = 0;

/* Output send buffer (avoids calling tcp_send for every single byte) */
static uint8_t out_buf[SEND_BUF_SIZE];
static int     out_len = 0;

static uint8_t pending_raw[PENDING_RAW_MAX];
static uint32_t pending_raw_len = 0;

/* ── Forward declarations ────────────────────────────────────────────────── */
static void telnet_accept_conn(tcp_connection_t *conn);
static void telnet_on_data(tcp_connection_t *conn, const uint8_t *data, uint32_t len);
static void telnet_on_close(tcp_connection_t *conn);
static void telnet_raw_send(const uint8_t *buf, uint32_t len);
static void telnet_flush(void);
static void telnet_drain_pending_raw(void);

/* ── Output hook (called from term_write for every printf/puts) ──────────── */
static void telnet_output_hook(const char *buf, size_t len) {
    if (!active_conn || active_conn->state != TCP_STATE_ESTABLISHED) return;

    /* Translate bare '\n' → '\r\n' so standard terminals render correctly */
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == '\n') {
            out_buf[out_len++] = '\r';
            if (out_len >= SEND_BUF_SIZE - 1) telnet_flush();
        }
        out_buf[out_len++] = (uint8_t)buf[i];
        if (out_len >= SEND_BUF_SIZE - 1) telnet_flush();
    }
    /* Don't flush here; telnet_server_poll() will flush at the end of the tick */
}

/* ── Flush out_buf to the TCP connection ─────────────────────────────────── */
static void telnet_flush(void) {
    if (!active_conn || active_conn->state != TCP_STATE_ESTABLISHED || out_len <= 0)
        return;

    int n = tcp_send(active_conn, out_buf, (uint32_t)out_len);
    if (n <= 0)
        return;

    if ((uint32_t)n < (uint32_t)out_len) {
        memmove(out_buf, out_buf + n, (size_t)(out_len - n));
        out_len -= n;
    } else {
        out_len = 0;
    }
}

/* ── Drain queued raw bytes (IAC, echoes, prompts) when the window opens ─── */
static void telnet_drain_pending_raw(void) {
    while (pending_raw_len > 0 && active_conn &&
           active_conn->state == TCP_STATE_ESTABLISHED) {
        int n = tcp_send(active_conn, pending_raw, pending_raw_len);
        if (n <= 0)
            break;
        if ((uint32_t)n < pending_raw_len)
            memmove(pending_raw, pending_raw + n, pending_raw_len - (uint32_t)n);
        pending_raw_len -= (uint32_t)n;
    }
}

/* ── Send a raw byte sequence (for IAC negotiation) ─────────────────────── */
static void telnet_raw_send(const uint8_t *buf, uint32_t len) {
    if (!active_conn || active_conn->state != TCP_STATE_ESTABLISHED || len == 0)
        return;

    telnet_drain_pending_raw();

    uint32_t off = 0;
    while (off < len) {
        int n = tcp_send(active_conn, buf + off, len - off);
        if (n <= 0) {
            if (pending_raw_len + (len - off) <= PENDING_RAW_MAX) {
                memcpy(pending_raw + pending_raw_len, buf + off, len - off);
                pending_raw_len += len - off;
            }
            break;
        }
        off += (uint32_t)n;
    }
}

/* ── Send the initial IAC option negotiation ─────────────────────────────── */
static void telnet_send_greeting(void) {
    /* We:  WILL ECHO   → client should not echo locally
     *      WILL SGA    → no go-ahead characters
     *      DO   SGA    → ask client to suppress go-ahead too */
    static const uint8_t neg[] = {
        IAC, WILL, TELOPT_ECHO,
        IAC, WILL, TELOPT_SGA,
        IAC, DO,   TELOPT_SGA,
    };
    telnet_raw_send(neg, sizeof(neg));

    /* Welcome banner */
    const char *banner =
        "\r\n"
        "  _       ____              ___  ____\r\n"
        " (_)_ __  |  _ \\  _____   __/ _ \\/ ___|\r\n"
        " | | '_ \\ | | | |/ _ \\ \\ / / | | \\___ \\\r\n"
        " | | | | || |_| |  __/\\ V /| |_| |___) |\r\n"
        " |_|_| |_||____/ \\___| \\_/  \\___/|____/\r\n"
        "\r\n"
        " Telnet session – Zeus shell\r\n"
        " Type 'help' for commands, 'exit' to disconnect.\r\n"
        "\r\n"
        "> ";
    telnet_raw_send((const uint8_t *)banner, (uint32_t)strlen(banner));
}

/* ── Accept callback (called by TCP stack when SYN+ACK completes) ─────────── */
static void telnet_accept_conn(tcp_connection_t *conn) {
    if (active_conn) {
        /* Already have a session – refuse */
        const char *busy = "\r\nBusy – only one session at a time.\r\n";
        tcp_send(conn, (const uint8_t *)busy, (uint32_t)strlen(busy));
        tcp_close(conn);
        serial_printf("[telnet] Rejected new connection – session already active\n");
        return;
    }

    serial_printf("[telnet] New session from %d.%d.%d.%d:%d\n",
                  conn->remote_ip[0], conn->remote_ip[1],
                  conn->remote_ip[2], conn->remote_ip[3],
                  conn->remote_port);

    active_conn        = conn;
    conn->data_callback  = telnet_on_data;
    conn->close_callback = telnet_on_close;

    line_len  = 0;
    out_len   = 0;
    pending_raw_len = 0;
    iac_state = IAC_NORMAL;

    term_set_output_hook(telnet_output_hook);
    telnet_send_greeting();
}

/* ── Process one character from the network after IAC stripping ─────────── */
static void telnet_process_char(uint8_t c) {
    if (c == '\r' || c == '\n') {
        /* Execute the buffered line */
        line_buf[line_len] = '\0';

        if (line_len == 0) {
            /* Empty line – just re-issue the prompt */
            telnet_raw_send((const uint8_t *)"\r\n> ", 4);
            return;
        }

        /* Echo the newline to the client */
        telnet_raw_send((const uint8_t *)"\r\n", 2);

        /* Check for disconnect command before handing to Zeus */
        if (strcmp(line_buf, "exit") == 0 ||
            strcmp(line_buf, "quit") == 0 ||
            strcmp(line_buf, "logout") == 0) {
            const char *bye = "Goodbye.\r\n";
            telnet_raw_send((const uint8_t *)bye, (uint32_t)strlen(bye));
            tcp_close(active_conn);
            /* telnet_on_close will clean up */
            return;
        }

        /* Flush any pending output before running the command so the
         * client receives output in the correct order.              */
        telnet_flush();

        serial_printf("[telnet] cmd: '%s'\n", line_buf);
        zeus_process_command_service(line_buf);

        /* Flush output produced by the command */
        telnet_flush();

        /* Re-issue the prompt */
        telnet_raw_send((const uint8_t *)"> ", 2);

        line_len = 0;
        return;
    }

    if (c == 127 || c == 8) {  /* DEL / Backspace */
        if (line_len > 0) {
            line_len--;
            /* Erase character on client terminal */
            telnet_raw_send((const uint8_t *)"\x08 \x08", 3);
        }
        return;
    }

    /* Printable character */
    if (c >= 32 && c < 127 && line_len < LINE_BUF_SIZE - 1) {
        line_buf[line_len++] = (char)c;
        /* Echo the character back (we told the client WILL ECHO) */
        telnet_raw_send(&c, 1);
    }
}

/* ── IAC option reply helper ─────────────────────────────────────────────── */
static void telnet_reply_option(uint8_t response, uint8_t opt) {
    uint8_t pkt[3] = { IAC, response, opt };
    telnet_raw_send(pkt, 3);
}

/* ── Data callback (called by TCP stack for each received segment) ───────── */
static void telnet_on_data(tcp_connection_t *conn,
                           const uint8_t *data, uint32_t len) {
    (void)conn;
    for (uint32_t i = 0; i < len; i++) {
        uint8_t c = data[i];

        switch (iac_state) {
        case IAC_NORMAL:
            if (c == IAC) {
                iac_state = IAC_SEEN;
            } else {
                telnet_process_char(c);
            }
            break;

        case IAC_SEEN:
            if (c == IAC) {
                /* Escaped 0xFF – treat as literal 0xFF */
                telnet_process_char(0xFF);
                iac_state = IAC_NORMAL;
            } else if (c == SB) {
                iac_state = IAC_SUBNEG;
            } else if (c == WILL || c == WONT || c == DO || c == DONT) {
                iac_cmd   = c;
                iac_state = IAC_OPTION;
            } else {
                /* Single-byte command (NOP, DM, GA, etc.) – ignore */
                iac_state = IAC_NORMAL;
            }
            break;

        case IAC_SUBNEG:
            if (c == IAC)
                iac_state = IAC_SUBNEG_IAC;
            break;

        case IAC_SUBNEG_IAC:
            if (c == SE) {
                iac_state = IAC_NORMAL;
            } else if (c == IAC) {
                iac_state = IAC_SUBNEG;
            } else {
                iac_state = IAC_SUBNEG;
            }
            break;

        case IAC_OPTION: {
            uint8_t opt = c;
            if (iac_cmd == DO) {
                /* Client asks us to enable opt */
                if (opt == TELOPT_ECHO || opt == TELOPT_SGA) {
                    telnet_reply_option(WILL, opt);   /* already active */
                } else {
                    telnet_reply_option(WONT, opt);   /* refuse */
                }
            } else if (iac_cmd == DONT) {
                telnet_reply_option(WONT, opt);
            } else if (iac_cmd == WILL) {
                /* Client offers to enable opt */
                if (opt == TELOPT_SGA) {
                    telnet_reply_option(DO, opt);
                } else {
                    telnet_reply_option(DONT, opt);   /* refuse all others */
                }
            } else { /* WONT */
                telnet_reply_option(DONT, opt);
            }
            iac_state = IAC_NORMAL;
            break;
        }
        } /* switch */
    } /* for */
}

/* ── Close callback ──────────────────────────────────────────────────────── */
static void telnet_on_close(tcp_connection_t *conn) {
    if (conn == active_conn) {
        serial_printf("[telnet] Session closed\n");
        term_set_output_hook(NULL);
        active_conn = NULL;
        line_len    = 0;
        out_len         = 0;
        pending_raw_len = 0;
        iac_state       = IAC_NORMAL;
    }
}

/* ── tcp_listen accept wrapper ───────────────────────────────────────────── */
static void telnet_listen_accept(tcp_connection_t *conn) {
    telnet_accept_conn(conn);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void telnet_server_init(void) {
    int rc = tcp_listen(23, telnet_listen_accept);
    if (rc == 0) {
        serial_printf("[telnet] Server listening on port 23\n");
        printf("Telnet server: listening on port 23\n");
    } else {
        serial_printf("[telnet] Failed to start server (rc=%d)\n", rc);
    }
}

void telnet_server_poll(void) {
    /* Retry sends when the TCP window opens (banner + IAC often fill cwnd first). */
    if (active_conn && active_conn->state == TCP_STATE_ESTABLISHED) {
        telnet_drain_pending_raw();
        if (out_len > 0)
            telnet_flush();
    }

    /* Detect stale connection (closed by TCP stack but callback not fired) */
    if (active_conn && active_conn->state == TCP_STATE_CLOSED) {
        telnet_on_close(active_conn);
    }
}
