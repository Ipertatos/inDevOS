#include "syscall_internal.h"
#include "mem.h"
#include "string.h"
#include "thread.h"
#include "uaccess.h"
#include "net_tcp.h"
#include "net_udp.h"
#include "net_ethernet.h"
#include "net_device.h"
#include "../include/indevos_errno.h"

/* ── Helper: drain the NIC receive queue (drives TCP state machine) ───────── */
/* Serialized: concurrent callers (poll/recv/futex-woken threads on other
 * CPUs, the net thread) skip instead of racing the NIC RX ring; the holder
 * drains until empty, so a skipper never loses a packet for long. */
static volatile int poll_net_busy = 0;
void poll_network_once(void) {
    if (__atomic_exchange_n(&poll_net_busy, 1, __ATOMIC_ACQUIRE))
        return;
    uint8_t pkt[1600];
    while (net_has_pending_receive()) {
        int len = (int)net_receive_packet(pkt, sizeof(pkt));
        if (len > 0) ethernet_process_frame(pkt, (uint32_t)len);
        else break;
    }
    __atomic_store_n(&poll_net_busy, 0, __ATOMIC_RELEASE);
}

/* ── Helper: allocate a free fd slot ────────────────────────────────────── */
static int alloc_fd(process_t *proc) {
    for (int i = 3; i < MAX_OPEN_FILES_SYSCALL; i++) {
        if (!proc->fds[i].in_use) return i;
    }
    return -EMFILE;
}

/* ── sys_socket ─────────────────────────────────────────────────────────── */
int sys_sendto(int fd, const void *buf, uint32_t len, int flags,
               const sockaddr_in_t *dest);
int sys_recvfrom(int fd, void *buf, uint32_t len, int flags,
                 sockaddr_in_t *src);

void kernel_socket_destroy(kernel_socket_t *sock) {
    if (!sock) return;
    if (sock->type == SOCK_DGRAM) {
        if (sock->transport)
            udp_pcb_close((udp_pcb_t *)sock->transport);
    } else if (sock->transport) {
        tcp_socket_close((tcp_socket_t *)sock->transport);
    }
    kfree(sock);
}

static void sock_decode_addr(const sockaddr_in_t *ka, uint8_t ip[4], uint16_t *port) {
    uint32_t a = ka->sin_addr;
    ip[0] = (uint8_t)(a & 0xFF);
    ip[1] = (uint8_t)((a >> 8) & 0xFF);
    ip[2] = (uint8_t)((a >> 16) & 0xFF);
    ip[3] = (uint8_t)((a >> 24) & 0xFF);
    *port = (uint16_t)((ka->sin_port >> 8) | (ka->sin_port << 8));
}

static void sock_encode_addr(sockaddr_in_t *out, const uint8_t ip[4], uint16_t port) {
    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port = (uint16_t)((port >> 8) | (port << 8));
    out->sin_addr = (uint32_t)ip[0] | ((uint32_t)ip[1] << 8) |
                    ((uint32_t)ip[2] << 16) | ((uint32_t)ip[3] << 24);
}

int sys_socket(int domain, int type, int protocol) {
    (void)protocol;
    if (domain != AF_INET) return -EAFNOSUPPORT;
    if (type != SOCK_STREAM && type != SOCK_DGRAM) return -EPROTONOSUPPORT;

    process_t *proc = get_current_process();
    if (!proc) return -EINVAL;

    int fd = alloc_fd(proc);
    if (fd < 0) return -EMFILE;

    kernel_socket_t *sock = (kernel_socket_t *)kalloc(sizeof(kernel_socket_t));
    if (!sock) return -ENOMEM;
    memset(sock, 0, sizeof(*sock));
    sock->domain   = domain;
    sock->type     = type;
    sock->protocol = protocol;

    proc->fds[fd].file_handle = sock;
    proc->fds[fd].ref_count   = fd_refcount_create();
    if (!proc->fds[fd].ref_count) {
        kfree(sock);
        return -ENOMEM;
    }
    proc->fds[fd].fd_type     = FD_TYPE_SOCKET;
    proc->fds[fd].flags       = 0;
    proc->fds[fd].position    = 0;
    proc->fds[fd].in_use      = true;
    return fd;
}

/* ── sys_connect ────────────────────────────────────────────────────────── */
int sys_connect(int fd, sockaddr_in_t *addr, uint32_t addrlen) {
    process_t *proc = get_current_process();
    if (!proc || fd < 0 || fd >= MAX_OPEN_FILES_SYSCALL) return -EBADF;
    if (!proc->fds[fd].in_use || proc->fds[fd].fd_type != FD_TYPE_SOCKET) return -ENOTSOCK;
    if (!addr || addrlen < sizeof(sockaddr_in_t)) return -EINVAL;

    kernel_socket_t *sock = (kernel_socket_t *)proc->fds[fd].file_handle;
    if (!sock) return -ENOTSOCK;
    if (sock->connected) return -EISCONN;
    if (sock->type == SOCK_DGRAM) {
        sockaddr_in_t kaddr;
        if (copy_from_user(&kaddr, addr, sizeof(kaddr)) != 0) return -EFAULT;
        uint8_t ip[4];
        uint16_t port;
        sock_decode_addr(&kaddr, ip, &port);
        if (!sock->transport) {
            sock->transport = udp_pcb_create();
            if (!sock->transport) return -ENOMEM;
        }
        int rc = udp_pcb_connect((udp_pcb_t *)sock->transport, ip, port);
        if (rc < 0) return rc;
        sock->connected = true;
        sock->bound = true;
        sock->remote_port = port;
        memcpy(sock->remote_ip, ip, 4);
        return 0;
    }
    if (sock->listening || (sock->bound && sock->transport))
        return -EOPNOTSUPP;

    sockaddr_in_t kaddr;
    if (copy_from_user(&kaddr, addr, sizeof(kaddr)) != 0) return -EFAULT;

    /* Decode big-endian IPv4 address and port */
    uint8_t ip[4];
    uint32_t a = kaddr.sin_addr;
    ip[0] = (uint8_t)(a & 0xFF);
    ip[1] = (uint8_t)((a >> 8)  & 0xFF);
    ip[2] = (uint8_t)((a >> 16) & 0xFF);
    ip[3] = (uint8_t)((a >> 24) & 0xFF);
    uint16_t port = (uint16_t)((kaddr.sin_port >> 8) | (kaddr.sin_port << 8));

    /* Already started (nonblocking EINPROGRESS redo)? */
    if (sock->transport) {
        tcp_socket_t *tcp_sock = (tcp_socket_t *)sock->transport;
        poll_network_once();
        if (tcp_sock->connection &&
            tcp_sock->connection->state == TCP_STATE_ESTABLISHED) {
            sock->connected = true;
            return 0;
        }
        if (tcp_sock->connection &&
            (tcp_sock->connection->state == TCP_STATE_CLOSED ||
             tcp_sock->connection->state == TCP_STATE_SYN_SENT)) {
            /* still handshaking, or failed */
            if (tcp_sock->connection->state == TCP_STATE_SYN_SENT) {
                if (proc->fds[fd].flags & O_NONBLOCK) return -EINPROGRESS;
            }
        }
        if (!tcp_sock->connection ||
            tcp_sock->connection->state == TCP_STATE_CLOSED)
            return -ECONNREFUSED;
        if (proc->fds[fd].flags & O_NONBLOCK) return -EINPROGRESS;
    }

    tcp_socket_t *tcp_sock = tcp_socket_create();
    if (!tcp_sock) return -ENOBUFS;

    if (tcp_socket_connect(tcp_sock, ip, port) != 0) {
        tcp_socket_close(tcp_sock);
        return -ECONNREFUSED;
    }

    sock->transport  = tcp_sock;
    sock->remote_port = port;
    memcpy(sock->remote_ip, ip, 4);

    /* Nonblocking sockets must return EINPROGRESS and complete via poll(POLLOUT).
     * Returning 0 after a blocking wait breaks curl Happy Eyeballs: it never
     * sees "Connected" and never writes the TLS ClientHello. */
    if (proc->fds[fd].flags & O_NONBLOCK) {
        /* Always EINPROGRESS on the first connect() of a nonblocking socket so
         * userland (curl Happy Eyeballs) parks on poll(POLLOUT). A second
         * connect() after ESTABLISHED returns 0 via the redo path above. */
        return -EINPROGRESS;
    }

    /* Blocking: wait for the TCP handshake (max ~5 s) */
    {
        extern uint64_t ktime_deadline_ms(uint64_t);
        extern bool     ktime_expired(uint64_t);
        uint64_t deadline = ktime_deadline_ms(5000);
        while (!ktime_expired(deadline)) {
            poll_network_once();
            if (tcp_sock->connection &&
                tcp_sock->connection->state == TCP_STATE_ESTABLISHED)
                break;
            if (global_scheduler.scheduler_running)
                thread_sleep(1);
            else
                __asm__ volatile("hlt");
        }
    }

    if (!tcp_sock->connection ||
        tcp_sock->connection->state != TCP_STATE_ESTABLISHED) {
        tcp_socket_close(tcp_sock);
        sock->transport = NULL;
        return -ETIMEDOUT;
    }

    sock->connected = true;
    return 0;
}

/* ── sys_send ───────────────────────────────────────────────────────────── */
int sys_send(int fd, const void *buf, uint32_t len, int flags) {
    (void)flags;
    process_t *proc = get_current_process();
    if (!proc || fd < 0 || fd >= MAX_OPEN_FILES_SYSCALL) return -EBADF;
    if (!proc->fds[fd].in_use || proc->fds[fd].fd_type != FD_TYPE_SOCKET) return -ENOTSOCK;

    kernel_socket_t *sock = (kernel_socket_t *)proc->fds[fd].file_handle;
    if (!sock) return -ENOTSOCK;
    if (sock->type == SOCK_DGRAM) {
        if (!sock->connected || !sock->transport) return -ENOTCONN;
        return sys_sendto(fd, buf, len, flags, NULL);
    }
    if (!sock->connected || !sock->transport) return -ENOTCONN;
    if (len > 0 && (!buf || !access_ok_user_range((uint64_t)buf, len))) return -EFAULT;

    if (len == 0) return 0;
    uint8_t *kbuf = (uint8_t*)kalloc(len);
    if (!kbuf) return -ENOMEM;
    if (copy_from_user(kbuf, buf, len) != 0) {
        kfree(kbuf);
        return -EFAULT;
    }
    int rc = tcp_socket_send((tcp_socket_t *)sock->transport, kbuf, len);
    kfree(kbuf);
    return rc;
}

bool tcp_conn_at_eof(tcp_connection_t *c);
/* ── sys_recv ───────────────────────────────────────────────────────────── */
int sys_recv(int fd, void *buf, uint32_t len, int flags) {
    (void)flags;
    process_t *proc = get_current_process();
    if (!proc || fd < 0 || fd >= MAX_OPEN_FILES_SYSCALL) return -EBADF;
    if (!proc->fds[fd].in_use || proc->fds[fd].fd_type != FD_TYPE_SOCKET) return -ENOTSOCK;

    kernel_socket_t *sock = (kernel_socket_t *)proc->fds[fd].file_handle;
    if (!sock) return -ENOTSOCK;
    if (sock->type == SOCK_DGRAM)
        return sys_recvfrom(fd, buf, len, flags, NULL);
    if (!sock->connected || !sock->transport) return -ENOTCONN;
    if (len > 0 && (!buf || !access_ok_user_range((uint64_t)buf, len))) return -EFAULT;

    tcp_socket_t *tcp_sock = (tcp_socket_t *)sock->transport;
    if (len == 0) return 0;
    uint8_t *kbuf = (uint8_t*)kalloc(len);
    if (!kbuf) return -ENOMEM;

    /* Blocking receive: pump the network until we have data or conn closes */
    {
        extern uint64_t ktime_deadline_ms(uint64_t);
        extern bool     ktime_expired(uint64_t);
        uint64_t deadline = ktime_deadline_ms(30000);
        while (!ktime_expired(deadline)) {
            poll_network_once();
            int n = tcp_socket_recv(tcp_sock, kbuf, len);
            if (n > 0) {
                if (copy_to_user(buf, kbuf, (size_t)n) != 0) {
                    kfree(kbuf);
                    return -EFAULT;
                }
                kfree(kbuf);
                return n;
            }
            if (tcp_conn_at_eof(tcp_sock->connection)) {
                kfree(kbuf);
                return 0;
            }
            if (global_scheduler.scheduler_running)
                thread_sleep(1);
            else
                __asm__ volatile("hlt");
        }
    }
    kfree(kbuf);
    return 0; /* timeout */
}

int sys_bind(int fd, const sockaddr_in_t *addr, uint32_t addrlen) {
    process_t *proc = get_current_process();
    if (!proc || fd < 0 || fd >= MAX_OPEN_FILES_SYSCALL)
        return -EBADF;
    if (!proc->fds[fd].in_use || proc->fds[fd].fd_type != FD_TYPE_SOCKET)
        return -EBADF;
    if (!addr || addrlen < sizeof(sockaddr_in_t))
        return -EINVAL;
    kernel_socket_t *sock = (kernel_socket_t *)proc->fds[fd].file_handle;
    if (!sock || sock->bound)
        return -EINVAL;
    sockaddr_in_t ka;
    if (copy_from_user(&ka, addr, sizeof(ka)) != 0)
        return -EFAULT;
    uint16_t port = (uint16_t)((ka.sin_port >> 8) | (ka.sin_port << 8));
    if (port != 0 && port < 1024 && proc->uid != 0)
        return -EACCES;

    if (sock->type == SOCK_DGRAM) {
        udp_pcb_t *pcb = udp_pcb_create();
        if (!pcb) return -ENOMEM;
        int rc = udp_pcb_bind(pcb, port);
        if (rc < 0) {
            udp_pcb_close(pcb);
            return rc;
        }
        sock->transport = pcb;
        sock->bound = true;
        sock->local_port = udp_pcb_local_port(pcb);
        return 0;
    }

    tcp_socket_t *ts = tcp_socket_create();
    if (!ts)
        return -ENOMEM;
    if (tcp_socket_bind(ts, port) != 0) {
        tcp_socket_close(ts);
        return -EADDRINUSE;
    }
    sock->transport = ts;
    sock->bound = true;
    sock->local_port = port;
    return 0;
}

int sys_sendto(int fd, const void *buf, uint32_t len, int flags,
               const sockaddr_in_t *dest) {
    (void)flags;
    process_t *proc = get_current_process();
    if (!proc || fd < 0 || fd >= MAX_OPEN_FILES_SYSCALL) return -EBADF;
    if (!proc->fds[fd].in_use || proc->fds[fd].fd_type != FD_TYPE_SOCKET)
        return -ENOTSOCK;
    kernel_socket_t *sock = (kernel_socket_t *)proc->fds[fd].file_handle;
    if (!sock || sock->type != SOCK_DGRAM) return -EOPNOTSUPP;
    if (len > 1472) return -EMSGSIZE;
    if (len > 0 && (!buf || !access_ok_user_range((uint64_t)buf, len)))
        return -EFAULT;

    uint8_t ip[4];
    uint16_t port = 0;
    if (dest) {
        sockaddr_in_t ka;
        if (copy_from_user(&ka, dest, sizeof(ka)) != 0) return -EFAULT;
        sock_decode_addr(&ka, ip, &port);
    } else if (sock->connected) {
        memcpy(ip, sock->remote_ip, 4);
        port = sock->remote_port;
    } else {
        return -EDESTADDRREQ;
    }

    if (!sock->transport) {
        sock->transport = udp_pcb_create();
        if (!sock->transport) return -ENOMEM;
    }

    uint8_t *kbuf = NULL;
    if (len) {
        kbuf = (uint8_t *)kalloc(len);
        if (!kbuf) return -ENOMEM;
        if (copy_from_user(kbuf, buf, len) != 0) {
            kfree(kbuf);
            return -EFAULT;
        }
    }
    int rc = udp_pcb_sendto((udp_pcb_t *)sock->transport, kbuf, (uint16_t)len,
                            ip, port);
    if (kbuf) kfree(kbuf);
    return rc;
}

int sys_recvfrom(int fd, void *buf, uint32_t len, int flags,
                 sockaddr_in_t *src) {
    (void)flags;
    process_t *proc = get_current_process();
    if (!proc || fd < 0 || fd >= MAX_OPEN_FILES_SYSCALL) return -EBADF;
    if (!proc->fds[fd].in_use || proc->fds[fd].fd_type != FD_TYPE_SOCKET)
        return -ENOTSOCK;
    kernel_socket_t *sock = (kernel_socket_t *)proc->fds[fd].file_handle;
    if (!sock || sock->type != SOCK_DGRAM) return -EOPNOTSUPP;
    if (!sock->transport) return -EINVAL;
    if (len > 0 && (!buf || !access_ok_user_range((uint64_t)buf, len)))
        return -EFAULT;

    uint8_t *kbuf = NULL;
    if (len) {
        kbuf = (uint8_t *)kalloc(len);
        if (!kbuf) return -ENOMEM;
    }

    extern uint64_t ktime_deadline_ms(uint64_t);
    extern bool     ktime_expired(uint64_t);
    uint64_t deadline = ktime_deadline_ms(30000);
    while (!ktime_expired(deadline)) {
        poll_network_once();
        uint8_t sip[4];
        uint16_t sport = 0;
        int n = udp_pcb_recvfrom((udp_pcb_t *)sock->transport, kbuf,
                                 (uint16_t)(len > 65535 ? 65535 : len),
                                 sip, &sport);
        if (n >= 0) {  /* datagram dequeued (payload may be 0 bytes) */
            if (n > 0 && kbuf && copy_to_user(buf, kbuf, (size_t)n) != 0) {
                kfree(kbuf);
                return -EFAULT;
            }
            if (src) {
                sockaddr_in_t out;
                sock_encode_addr(&out, sip, sport);
                if (copy_to_user(src, &out, sizeof(out)) != 0) {
                    if (kbuf) kfree(kbuf);
                    return -EFAULT;
                }
            }
            if (kbuf) kfree(kbuf);
            return n;
        }
        /* n < 0: -EAGAIN when empty */
        if (proc->fds[fd].flags & O_NONBLOCK) {
            if (kbuf) kfree(kbuf);
            return n;
        }
        if (global_scheduler.scheduler_running)
            thread_sleep(1);
        else
            __asm__ volatile("hlt");
    }
    if (kbuf) kfree(kbuf);
    return 0;
}

int sys_listen(int fd, int backlog) {
    process_t *proc = get_current_process();
    if (!proc || fd < 0 || fd >= MAX_OPEN_FILES_SYSCALL)
        return -EBADF;
    if (!proc->fds[fd].in_use || proc->fds[fd].fd_type != FD_TYPE_SOCKET)
        return -EBADF;
    kernel_socket_t *sock = (kernel_socket_t *)proc->fds[fd].file_handle;
    if (!sock || sock->type != SOCK_STREAM || !sock->bound || !sock->transport)
        return -EINVAL;
    if (tcp_socket_listen((tcp_socket_t *)sock->transport, backlog) != 0)
        return -EIO;
    sock->listening = true;
    return 0;
}

int sys_accept(int fd, sockaddr_in_t *addr, uint32_t *addrlen) {
    process_t *proc = get_current_process();
    if (!proc || fd < 0 || fd >= MAX_OPEN_FILES_SYSCALL)
        return -EBADF;
    if (!proc->fds[fd].in_use || proc->fds[fd].fd_type != FD_TYPE_SOCKET)
        return -EBADF;
    kernel_socket_t *ls = (kernel_socket_t *)proc->fds[fd].file_handle;
    if (!ls || !ls->listening || !ls->transport)
        return -EINVAL;

    extern uint64_t ktime_deadline_ms(uint64_t);
    extern bool     ktime_expired(uint64_t);
    uint64_t deadline = ktime_deadline_ms(30000);
    tcp_socket_t *acc = NULL;
    while (!ktime_expired(deadline)) {
        poll_network_once();
        acc = tcp_socket_accept((tcp_socket_t *)ls->transport);
        if (acc)
            break;
        if (global_scheduler.scheduler_running)
            thread_sleep(1);
        else
            __asm__ volatile("hlt");
    }
    if (!acc)
        return -EAGAIN;

    int nfd = alloc_fd(proc);
    if (nfd < 0) {
        tcp_socket_close(acc);
        return -EMFILE;
    }
    kernel_socket_t *ns = (kernel_socket_t *)kalloc(sizeof(kernel_socket_t));
    if (!ns) {
        tcp_socket_close(acc);
        return -ENOMEM;
    }
    memset(ns, 0, sizeof(*ns));
    ns->domain = AF_INET;
    ns->type = SOCK_STREAM;
    ns->transport = acc;
    ns->connected = true;
    ns->bound = true;
    proc->fds[nfd].file_handle = ns;
    proc->fds[nfd].fd_type = FD_TYPE_SOCKET;
    proc->fds[nfd].ref_count = fd_refcount_create();
    if (!proc->fds[nfd].ref_count) {
        kfree(ns);
        tcp_socket_close(acc);
        return -ENOMEM;
    }
    proc->fds[nfd].flags = 0;
    proc->fds[nfd].in_use = true;
    proc->fds[nfd].cloexec = false;

    if (addr && addrlen) {
        uint32_t alen = 0;
        if (copy_from_user(&alen, addrlen, sizeof(alen)) == 0 && alen >= sizeof(sockaddr_in_t)) {
            sockaddr_in_t out;
            memset(&out, 0, sizeof(out));
            out.sin_family = AF_INET;
            tcp_connection_t *c = acc->connection;
            if (c) {
                out.sin_port = (uint16_t)((c->remote_port >> 8) | (c->remote_port << 8));
                uint32_t ip = (uint32_t)c->remote_ip[0] | ((uint32_t)c->remote_ip[1] << 8) |
                              ((uint32_t)c->remote_ip[2] << 16) | ((uint32_t)c->remote_ip[3] << 24);
                out.sin_addr = ip;
            }
            if (copy_to_user(addr, &out, sizeof(out)) == 0) {
                uint32_t outlen = sizeof(sockaddr_in_t);
                copy_to_user(addrlen, &outlen, sizeof(outlen));
            }
        }
    }
    return nfd;
}

int sys_setsockopt(int fd, int level, int optname, const void *optval, uint32_t optlen) {
    process_t *proc = get_current_process();
    if (!proc || fd < 0 || fd >= MAX_OPEN_FILES_SYSCALL)
        return -EBADF;
    if (!proc->fds[fd].in_use || proc->fds[fd].fd_type != FD_TYPE_SOCKET)
        return -ENOTSOCK;
    (void)level; (void)optname; (void)optval; (void)optlen;
    return -ENOPROTOOPT;
}

int sys_getsockopt(int fd, int level, int optname, void *optval, uint32_t *optlen) {
    process_t *proc = get_current_process();
    if (!proc || fd < 0 || fd >= MAX_OPEN_FILES_SYSCALL)
        return -EBADF;
    if (!proc->fds[fd].in_use || proc->fds[fd].fd_type != FD_TYPE_SOCKET)
        return -ENOTSOCK;
    (void)level; (void)optname; (void)optval; (void)optlen;
    return -ENOPROTOOPT;
}

int sys_shutdown(int fd, int how) {
    (void)how;
    process_t *proc = get_current_process();
    if (!proc || fd < 0 || fd >= MAX_OPEN_FILES_SYSCALL)
        return -EBADF;
    if (!proc->fds[fd].in_use || proc->fds[fd].fd_type != FD_TYPE_SOCKET)
        return -EBADF;
    kernel_socket_t *sock = (kernel_socket_t *)proc->fds[fd].file_handle;
    if (sock && sock->transport) {
        if (sock->type == SOCK_DGRAM)
            udp_pcb_close((udp_pcb_t *)sock->transport);
        else
            tcp_socket_close((tcp_socket_t *)sock->transport);
        sock->transport = NULL;
    }
    sock->connected = false;
    return 0;
}


/* ── Linux socket ABI glue (static musl programs: curl, busybox wget, …) ───
 * Linux and inDevOS share the AF_INET sockaddr layout, so addresses are passed
 * through; here we add the Linux type flags, non-blocking I/O, EOF reporting,
 * the generic read/write paths and the name/option queries. */
extern bool proc_kill_pending(process_t *p);
#define LXS_SOCK_NONBLOCK 04000
#define LXS_SOCK_CLOEXEC  02000000
#define LXS_MSG_PEEK      0x2
#define LXS_MSG_DONTWAIT  0x40

static kernel_socket_t *lxs_get(process_t *p, int fd) {
    if (!p || fd < 0 || fd >= MAX_OPEN_FILES_SYSCALL) return NULL;
    if (!p->fds[fd].in_use || p->fds[fd].fd_type != FD_TYPE_SOCKET) return NULL;
    return (kernel_socket_t *)p->fds[fd].file_handle;
}

bool tcp_conn_at_eof(tcp_connection_t *c) {
    if (!c) return false;  /* no connection yet: not EOF, just not readable */
    return c->state == TCP_STATE_CLOSE_WAIT || c->state == TCP_STATE_LAST_ACK ||
           c->state == TCP_STATE_CLOSED || c->state == TCP_STATE_TIME_WAIT ||
           c->state == TCP_STATE_CLOSING;
}

long lx_sock_socket(long domain, long type, long protocol) {
    int base = (int)(type & 0xF);
    if (domain != AF_INET) return -EAFNOSUPPORT;
    int fd = sys_socket((int)domain, base, (int)protocol);
    if (fd < 0) return fd;
    process_t *p = get_current_process();
    if (type & LXS_SOCK_NONBLOCK) p->fds[fd].flags |= O_NONBLOCK;
    if (type & LXS_SOCK_CLOEXEC) p->fds[fd].cloexec = true;
    return fd;
}

/* Stream receive honouring O_NONBLOCK / MSG_DONTWAIT; 0 means EOF. */
long lx_sock_recv(int fd, void *ubuf, size_t len, int flags) {
    process_t *p = get_current_process();
    kernel_socket_t *s = lxs_get(p, fd);
    if (!s) return -ENOTSOCK;
    bool nb = (p->fds[fd].flags & O_NONBLOCK) || (flags & LXS_MSG_DONTWAIT);
    if (s->type == SOCK_DGRAM) {
        if (nb) {
            poll_network_once();
            if (!s->transport || udp_pcb_rx_ready((udp_pcb_t *)s->transport) <= 0) return -EAGAIN;
        }
        return sys_recvfrom(fd, ubuf, (uint32_t)len, 0, NULL);
    }
    if (!s->connected || !s->transport) return -ENOTCONN;
    if (len == 0) return 0;
    if (len > 65536) len = 65536;
    if (!ubuf || !access_ok_user_range((uint64_t)ubuf, len)) return -EFAULT;
    tcp_socket_t *ts = (tcp_socket_t *)s->transport;
    uint8_t *k = (uint8_t *)kalloc(len);
    if (!k) return -ENOMEM;
    extern uint64_t ktime_deadline_ms(uint64_t);
    extern bool     ktime_expired(uint64_t);
    uint64_t deadline = ktime_deadline_ms(60000);
    long r = -EAGAIN;
    for (;;) {
        poll_network_once();
        int n = ts->connection ? tcp_socket_recv(ts, k, (uint32_t)len) : 0;
        if (n > 0) {
            r = copy_to_user(ubuf, k, (size_t)n) ? -EFAULT : n;
            break;
        }
        if (n < 0 || tcp_conn_at_eof(ts->connection)) { r = 0; break; }
        if (nb) { r = -EAGAIN; break; }
        if (ktime_expired(deadline)) { r = -ETIMEDOUT; break; }
        if (proc_kill_pending(p)) { r = -EINTR; break; }
        if (global_scheduler.scheduler_running) thread_sleep(1);
        else __asm__ volatile("hlt");
    }
    kfree(k);
    return r;
}

long lx_sock_send(int fd, const void *ubuf, size_t len, int flags) {
    process_t *p = get_current_process();
    kernel_socket_t *s = lxs_get(p, fd);
    if (!s) return -ENOTSOCK;
    if (s->type == SOCK_DGRAM) return sys_sendto(fd, ubuf, (uint32_t)len, 0, NULL);
    if (!s->transport) return -ENOTCONN;
    tcp_socket_t *ts = (tcp_socket_t *)s->transport;
    if (!ts->connection || ts->connection->state != TCP_STATE_ESTABLISHED) return -ENOTCONN;
    s->connected = true;
    if (len > 65536) len = 65536;
    if (len == 0) return 0;
    bool nb = (p->fds[fd].flags & O_NONBLOCK) || (flags & LXS_MSG_DONTWAIT);
    extern uint64_t ktime_deadline_ms(uint64_t);
    extern bool     ktime_expired(uint64_t);
    uint64_t deadline = ktime_deadline_ms(60000);
    for (;;) {
        /* tcp_send returns 0 when the peer's window is full: never hand a
         * 0 back to the program (OpenSSL would spin); wait or EAGAIN. */
        int r = sys_send(fd, ubuf, (uint32_t)len, 0);
        if (r != 0) return r;
        if (nb) return -EAGAIN;
        if (!ts->connection || ts->connection->state != TCP_STATE_ESTABLISHED) return -EPIPE;
        if (ktime_expired(deadline)) return -ETIMEDOUT;
        if (proc_kill_pending(p)) return -EINTR;
        poll_network_once();
        if (global_scheduler.scheduler_running) thread_sleep(1);
        else __asm__ volatile("hlt");
    }
}

long lx_sock_sendto(int fd, const void *ubuf, size_t len, int flags,
                    const void *uaddr, uint32_t alen) {
    process_t *p = get_current_process();
    kernel_socket_t *s = lxs_get(p, fd);
    if (!s) return -ENOTSOCK;
    if (s->type == SOCK_DGRAM && uaddr) {
        if (alen < sizeof(sockaddr_in_t)) return -EINVAL;
        return sys_sendto(fd, ubuf, (uint32_t)len, 0, (const sockaddr_in_t *)uaddr);
    }
    return lx_sock_send(fd, ubuf, len, flags);
}

long lx_sock_recvfrom(int fd, void *ubuf, size_t len, int flags, void *uaddr, uint32_t *ualen) {
    process_t *p = get_current_process();
    kernel_socket_t *s = lxs_get(p, fd);
    if (!s) return -ENOTSOCK;
    if (s->type != SOCK_DGRAM) return lx_sock_recv(fd, ubuf, len, flags);
    bool nb = (p->fds[fd].flags & O_NONBLOCK) || (flags & LXS_MSG_DONTWAIT);
    if (!s->transport) return nb ? -EAGAIN : -EINVAL;
    if (nb) {
        poll_network_once();
        if (udp_pcb_rx_ready((udp_pcb_t *)s->transport) <= 0) return -EAGAIN;
    }
    sockaddr_in_t *tmp = NULL;
    uint32_t al = 0;
    if (uaddr && ualen) {
        if (copy_from_user(&al, ualen, sizeof(al))) return -EFAULT;
        if (al >= sizeof(sockaddr_in_t)) tmp = (sockaddr_in_t *)uaddr;
    }
    long r = sys_recvfrom(fd, ubuf, (uint32_t)len, 0, tmp);
    if (r >= 0 && tmp) {
        al = sizeof(sockaddr_in_t);
        copy_to_user(ualen, &al, sizeof(al));
    }
    return r;
}

static long lxs_put_name(void *uaddr, uint32_t *ualen, const uint8_t ip[4], uint16_t port) {
    if (!uaddr || !ualen) return -EFAULT;
    uint32_t al;
    if (copy_from_user(&al, ualen, sizeof(al))) return -EFAULT;
    sockaddr_in_t o;
    sock_encode_addr(&o, ip, port);
    uint32_t n = al < sizeof(o) ? al : sizeof(o);
    if (copy_to_user(uaddr, &o, n)) return -EFAULT;
    al = sizeof(o);
    if (copy_to_user(ualen, &al, sizeof(al))) return -EFAULT;
    return 0;
}

long lx_sock_name(int fd, void *uaddr, uint32_t *ualen, bool peer) {
    process_t *p = get_current_process();
    kernel_socket_t *s = lxs_get(p, fd);
    if (!s) return -ENOTSOCK;
    uint8_t ip[4] = {0, 0, 0, 0};
    uint16_t port = 0;
    if (s->type == SOCK_STREAM && s->transport && ((tcp_socket_t *)s->transport)->connection) {
        tcp_connection_t *c = ((tcp_socket_t *)s->transport)->connection;
        if (peer) { memcpy(ip, c->remote_ip, 4); port = c->remote_port; }
        else      { memcpy(ip, c->local_ip, 4);  port = c->local_port; }
    } else if (peer) {
        if (!s->connected) return -ENOTCONN;
        memcpy(ip, s->remote_ip, 4); port = s->remote_port;
    } else {
        port = s->local_port;
        if (s->type == SOCK_DGRAM && s->transport) port = udp_pcb_local_port((udp_pcb_t *)s->transport);
    }
    return lxs_put_name(uaddr, ualen, ip, port);
}

long lx_sock_getsockopt(int fd, int level, int opt, void *uval, uint32_t *ulen) {
    process_t *p = get_current_process();
    kernel_socket_t *s = lxs_get(p, fd);
    if (!s) return -ENOTSOCK;
    int v = 0;
    if (level == 1 && opt == 3) v = s->type;                 /* SO_TYPE */
    else if (level == 1 && opt == 4) {                       /* SO_ERROR */
        v = 0;
        if (s->type == SOCK_STREAM && s->transport) {
            tcp_connection_t *c = ((tcp_socket_t *)s->transport)->connection;
            if (c && c->state == TCP_STATE_ESTABLISHED) s->connected = true;
            else if (!c || c->state == TCP_STATE_CLOSED) v = ECONNREFUSED;
            /* SYN_SENT: SO_ERROR stays 0; completion is POLLOUT */
        }
    }
    else if (level == 1 && (opt == 7 || opt == 8)) v = 65536; /* SO_SNDBUF/RCVBUF */
    if (!uval || !ulen) return -EFAULT;
    uint32_t l;
    if (copy_from_user(&l, ulen, sizeof(l))) return -EFAULT;
    if (l > sizeof(v)) l = sizeof(v);
    if (copy_to_user(uval, &v, l)) return -EFAULT;
    return copy_to_user(ulen, &l, sizeof(l)) ? -EFAULT : 0;
}

long lx_sock_setsockopt(int fd, int level, int opt, const void *uval, uint32_t len) {
    process_t *p = get_current_process();
    return lxs_get(p, fd) ? 0 : -ENOTSOCK;   /* accepted and ignored */
}

long lx_sock_shutdown(int fd, int how) {
    process_t *p = get_current_process();
    kernel_socket_t *s = lxs_get(p, fd);
    if (!s) return -ENOTSOCK;
    /* Accepted but deferred: sending our FIN here would let the FIN_WAIT_1
     * path free the connection while the program still reads from it. The
     * FIN goes out when the descriptor is closed. */
    (void)how;
    return 0;
}

bool lx_fd_is_socket(process_t *p, int fd) { return lxs_get(p, fd) != NULL; }
