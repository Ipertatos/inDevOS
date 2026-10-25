#include "net_ethernet.h"
#include "net_tcp.h"
#include "../include/indevos_errno.h"
#include "net_ip.h"
#include "mem.h"
#include "string.h"
#include "stdio.h"
#include "net_device.h"
#include "string.h"
#include "utils.h"
#include "serial.h"
#include "atomic.h"
#include "ktime.h"

// Global TCP state – protected by tcp_lock for concurrent access from
// IRQ handler, network thread, and syscall context.
static tcp_connection_t *tcp_connections = NULL;
static uint32_t tcp_isn_counter = 0x12345678;
static spinlock_t tcp_lock = {0};

#define TCP_LOCK()   klock(&tcp_lock)
#define TCP_UNLOCK() kunlock(&tcp_lock)

/* Per-packet debug logging: compiled out by default (rx hot path). */
#define TCP_RX_DEBUG 0
#if TCP_RX_DEBUG
#define tcp_dbg(...) serial_printf(__VA_ARGS__)
#else
#define tcp_dbg(...) do {} while (0)
#endif

/* ── Listener registry ──────────────────────────────────────────────────── */
#define TCP_MAX_LISTENERS 8
typedef struct {
    uint16_t port;
    void (*accept_callback)(tcp_connection_t *conn);
} tcp_listener_t;
static tcp_listener_t tcp_listeners[TCP_MAX_LISTENERS];
static int tcp_listener_count = 0;

static tcp_connection_t *tcp_find_user_listen(uint16_t port);
static tcp_listener_t *tcp_find_listener(uint16_t port) {
    for (int i = 0; i < tcp_listener_count; i++) {
        if (tcp_listeners[i].port == port) return &tcp_listeners[i];
    }
    return NULL;
}

// TCP timeouts (in milliseconds)
#define TCP_SYN_TIMEOUT          3000
#define TCP_ESTABLISH_TIMEOUT    5000
#define TCP_CLOSE_TIMEOUT        2000
#define TCP_RETRANSMIT_TIMEOUT   1000
#define TCP_TIME_WAIT_TIMEOUT    60000   // 60 seconds for TIME_WAIT state
#define TCP_STALE_CONN_TIMEOUT   120000  // 2 minutes for stale connections

// Buffer sizes
#define TCP_DEFAULT_BUFFER_SIZE 4096
#define TCP_RECV_BUFFER_SIZE    32768   /* advertised receive window */
#define TCP_MAX_CONNECTIONS 16

// Function prototypes
static tcp_connection_t* tcp_find_connection(const uint8_t *local_ip, uint16_t local_port,
                                           const uint8_t *remote_ip, uint16_t remote_port);
static tcp_connection_t* tcp_create_connection(const uint8_t *local_ip, uint16_t local_port,
                                             const uint8_t *remote_ip, uint16_t remote_port);
static void tcp_destroy_connection(tcp_connection_t *conn);
static int tcp_send_packet(tcp_connection_t *conn, uint8_t flags, const uint8_t *data, uint32_t data_len);
static int tcp_send_packet_with_options(tcp_connection_t *conn, uint8_t flags,
                                        const uint8_t *data, uint32_t data_len,
                                        const uint8_t *options, uint32_t options_len);
static void tcp_handle_syn(tcp_connection_t *conn, const tcp_header_t *tcp_header, uint32_t payload_len);
static void tcp_handle_ack(tcp_connection_t *conn, const tcp_header_t *tcp_header);
static void tcp_handle_fin(tcp_connection_t *conn, const tcp_header_t *tcp_header);
static uint32_t tcp_handle_data(tcp_connection_t *conn, const uint8_t *data, uint32_t len);
static void tcp_retransmit_enqueue(tcp_connection_t *conn, uint32_t seq, uint8_t flags,
                                   const uint8_t *data, uint32_t data_len);
static void tcp_retransmit_ack(tcp_connection_t *conn, uint32_t ack_num);
static void tcp_retransmit_flush(tcp_connection_t *conn);
static void tcp_parse_options(tcp_connection_t *conn, const uint8_t *opts, uint32_t opts_len);

// Helper: get HPET ticks as ms (approximate)
static uint64_t tcp_now_ms(void) {
    return ktime_now_ms();
}

void tcp_init(void) {
    printf("TCP: Initialized\n");
    tcp_isn_counter = 0x12345678; // Simple ISN initialization
}

/**
 * @brief Optimized TCP checksum calculation with pseudo-header
 *
 * All 16-bit words are summed in network (big-endian) byte order so the
 * pseudo-header and payload contributions are consistent.
 */
uint16_t tcp_checksum(const uint8_t *src_ip, const uint8_t *dst_ip,
                     const uint8_t *tcp_packet, uint16_t len) {
    uint32_t sum = 0;

    // Add pseudo-header (IP addresses, protocol, length)
    // Access as 16-bit values for efficiency
    sum += ((uint16_t)src_ip[0] << 8) | src_ip[1];
    sum += ((uint16_t)src_ip[2] << 8) | src_ip[3];
    sum += ((uint16_t)dst_ip[0] << 8) | dst_ip[1];
    sum += ((uint16_t)dst_ip[2] << 8) | dst_ip[3];
    sum += 0x0006; // TCP protocol number (in network byte order)
    sum += len;

    // Sum TCP header + data as big-endian 16-bit words (must match the
    // pseudo-header byte order above).
    uint16_t remaining = len;
    const uint8_t *ptr = tcp_packet;

    while (remaining >= 2) {
        sum += ((uint16_t)ptr[0] << 8) | ptr[1];
        ptr += 2;
        remaining -= 2;
    }

    if (remaining == 1) {
        sum += ((uint16_t)ptr[0]) << 8;
    }

    // Fold 32-bit sum into 16 bits (handle overflow)
    // Two iterations are sufficient for any input
    sum = (sum >> 16) + (sum & 0xFFFF);
    sum = (sum >> 16) + (sum & 0xFFFF);

    return (uint16_t)(~sum);
}

void tcp_print_packet(const tcp_header_t *header) {
    printf("TCP: src_port=%u, dst_port=%u, seq=%u, ack=%u\n",
           header->src_port, header->dst_port,
           header->seq_num, header->ack_num);
    printf("TCP: flags=");
    if (header->flags & TCP_FLAG_SYN) printf("SYN ");
    if (header->flags & TCP_FLAG_ACK) printf("ACK ");
    if (header->flags & TCP_FLAG_FIN) printf("FIN ");
    if (header->flags & TCP_FLAG_RST) printf("RST ");
    if (header->flags & TCP_FLAG_PSH) printf("PSH ");
    if (header->flags & TCP_FLAG_URG) printf("URG ");
    printf(", window=%u\n", header->window);
}

static tcp_connection_t* tcp_find_connection(const uint8_t *local_ip, uint16_t local_port,
                                           const uint8_t *remote_ip, uint16_t remote_port) {
    TCP_LOCK();
    tcp_connection_t *conn = tcp_connections;
    while (conn) {
        if (memcmp(conn->local_ip, local_ip, 4) == 0 &&
            memcmp(conn->remote_ip, remote_ip, 4) == 0 &&
            conn->local_port == local_port &&
            conn->remote_port == remote_port) {
            TCP_UNLOCK();
            return conn;
        }
        conn = conn->next;
    }
    TCP_UNLOCK();
    return NULL;
}

// Forward declaration for cleanup
static void tcp_update_activity(tcp_connection_t *conn);

static tcp_connection_t* tcp_create_connection(const uint8_t *local_ip, uint16_t local_port,
                                             const uint8_t *remote_ip, uint16_t remote_port) {
    // Check connection limit
    int connection_count = 0;
    tcp_connection_t *conn = tcp_connections;
    while (conn) {
        connection_count++;
        conn = conn->next;
    }

    if (connection_count >= TCP_MAX_CONNECTIONS) {
        printf("TCP: Connection limit reached\n");
        return NULL;
    }

    // Allocate connection
    tcp_connection_t *new_conn = kalloc(sizeof(tcp_connection_t));
    if (!new_conn) {
        printf("TCP: Failed to allocate connection\n");
        return NULL;
    }

    memset(new_conn, 0, sizeof(tcp_connection_t));

    // Initialize connection
    memcpy(new_conn->local_ip, local_ip, 4);
    memcpy(new_conn->remote_ip, remote_ip, 4);
    new_conn->local_port = local_port;
    new_conn->remote_port = remote_port;
    new_conn->state = TCP_STATE_CLOSED;
    new_conn->connected = false;
    new_conn->window_size = TCP_DEFAULT_BUFFER_SIZE;
    new_conn->remote_window = TCP_DEFAULT_BUFFER_SIZE;

    // Allocate buffers
    new_conn->send_buffer = kalloc(TCP_DEFAULT_BUFFER_SIZE);
    new_conn->recv_buffer = kalloc(TCP_RECV_BUFFER_SIZE);

    if (!new_conn->send_buffer || !new_conn->recv_buffer) {
        printf("TCP: Failed to allocate buffers\n");
        if (new_conn->send_buffer) kfree(new_conn->send_buffer);
        if (new_conn->recv_buffer) kfree(new_conn->recv_buffer);
        kfree(new_conn);
        return NULL;
    }

    new_conn->send_buffer_size = TCP_DEFAULT_BUFFER_SIZE;
    new_conn->recv_buffer_size = TCP_RECV_BUFFER_SIZE;
    new_conn->send_buffer_used = 0;
    new_conn->recv_buffer_used = 0;

    // MSS negotiation defaults
    new_conn->local_mss  = TCP_MAX_MSS;
    new_conn->remote_mss = TCP_DEFAULT_MSS;

    // Retransmission state (RFC 6298)
    new_conn->retransmit_queue = NULL;
    new_conn->rto_ms     = TCP_RTO_MIN_MS;
    new_conn->srtt_ms    = 0;
    new_conn->rttvar_ms  = 0;
    new_conn->snd_una    = 0;

    // Congestion control (RFC 5681 – TCP Reno)
    new_conn->cwnd           = TCP_MAX_MSS;      // IW = 1 MSS (conservative start)
    new_conn->ssthresh       = 65535;             // Start with max threshold
    new_conn->dup_ack_count  = 0;
    new_conn->last_ack_num   = 0;
    new_conn->in_fast_recovery = false;

    // Initialize activity timestamp
    new_conn->last_activity = tcp_now_ms();

    TCP_LOCK();
    new_conn->next = tcp_connections;
    tcp_connections = new_conn;
    TCP_UNLOCK();

    serial_printf("TCP: Created connection %d.%d.%d.%d:%u -> %d.%d.%d.%d:%u\n",
                  local_ip[0], local_ip[1], local_ip[2], local_ip[3], local_port,
                  remote_ip[0], remote_ip[1], remote_ip[2], remote_ip[3], remote_port);

    return new_conn;
}

// Update connection activity timestamp
static void tcp_update_activity(tcp_connection_t *conn) {
    if (conn) {
        extern uint64_t hpet_get_ticks(void);
        conn->last_activity = hpet_get_ticks();
    }
}

/* Unlink and free a connection. Caller must hold tcp_lock. */
static void tcp_destroy_connection_locked(tcp_connection_t *conn) {
    if (!conn) return;

    if (tcp_connections == conn) {
        tcp_connections = conn->next;
    } else {
        tcp_connection_t *prev = tcp_connections;
        while (prev && prev->next != conn) {
            prev = prev->next;
        }
        if (prev) {
            prev->next = conn->next;
        }
    }

    tcp_retransmit_flush(conn);

    if (conn->send_buffer) kfree(conn->send_buffer);
    if (conn->recv_buffer) kfree(conn->recv_buffer);

    serial_printf("TCP: Destroyed connection %d.%d.%d.%d:%u -> %d.%d.%d.%d:%u\n",
                  conn->local_ip[0], conn->local_ip[1], conn->local_ip[2], conn->local_ip[3], conn->local_port,
                  conn->remote_ip[0], conn->remote_ip[1], conn->remote_ip[2], conn->remote_ip[3], conn->remote_port);

    kfree(conn);
}

static void tcp_destroy_connection(tcp_connection_t *conn) {
    if (!conn) return;
    TCP_LOCK();
    tcp_destroy_connection_locked(conn);
    TCP_UNLOCK();
}

/**
 * @brief Clean up stale TCP connections to prevent memory leaks
 *
 * This function should be called periodically (e.g., from a timer or network task)
 * to clean up connections that have been idle for too long or are stuck in
 * TIME_WAIT state.
 */
void tcp_cleanup_stale_connections(void) {
    /* last_activity is stored in milliseconds (hpet_get_ticks / ktime). */
    uint64_t now = ktime_now_ms();
    uint64_t time_wait_ticks = TCP_TIME_WAIT_TIMEOUT;
    uint64_t stale_ticks = TCP_STALE_CONN_TIMEOUT;

    TCP_LOCK();
    tcp_connection_t *conn = tcp_connections;
    tcp_connection_t *next;

    while (conn) {
        next = conn->next;  // Save next before potential destruction

        uint64_t idle_time = now - conn->last_activity;

        if (conn->owned) { conn = next; continue; }   /* socket still holds it */

        // Clean up TIME_WAIT connections after timeout
        if (conn->state == TCP_STATE_TIME_WAIT && idle_time > time_wait_ticks) {
            serial_printf("TCP: Cleaning up TIME_WAIT connection (idle %lu ms)\n",
                   (unsigned long)idle_time);
            tcp_destroy_connection_locked(conn);
        }
        // Clean up connections stuck in intermediate states
        else if ((conn->state == TCP_STATE_SYN_SENT ||
                  conn->state == TCP_STATE_SYN_RECEIVED ||
                  conn->state == TCP_STATE_CLOSING ||
                  conn->state == TCP_STATE_LAST_ACK) && 
                 idle_time > stale_ticks) {
            serial_printf("TCP: Cleaning up stale connection in state %d (idle %lu ms)\n",
                   conn->state, (unsigned long)idle_time);
            tcp_destroy_connection_locked(conn);
        }
        // Clean up established connections that have been idle too long (optional)
        else if (conn->state == TCP_STATE_CLOSED && idle_time > stale_ticks) {
            serial_printf("TCP: Cleaning up closed connection (idle %lu ms)\n",
                   (unsigned long)idle_time);
            tcp_destroy_connection_locked(conn);
        }

        conn = next;
    }
    TCP_UNLOCK();
}

/**
 * @brief Get number of active TCP connections
 * @return Number of connections in the connection list
 */
int tcp_get_connection_count(void) {
    int count = 0;
    TCP_LOCK();
    tcp_connection_t *conn = tcp_connections;
    while (conn) {
        count++;
        conn = conn->next;
    }
    TCP_UNLOCK();
    return count;
}

// ---- Retransmission queue management ----------------------------------------

static void tcp_retransmit_enqueue(tcp_connection_t *conn, uint32_t seq, uint8_t flags,
                                   const uint8_t *data, uint32_t data_len) {
    tcp_retransmit_entry_t *entry = kalloc(sizeof(tcp_retransmit_entry_t));
    if (!entry) return;

    memset(entry, 0, sizeof(tcp_retransmit_entry_t));
    entry->seq_start = seq;
    entry->flags     = flags;
    entry->sent_at   = tcp_now_ms();
    entry->rto_ms    = conn->rto_ms;
    entry->retries   = 0;

    // Calculate seq_end: data consumes data_len, SYN/FIN consume 1 each
    uint32_t consumed = data_len;
    if (flags & TCP_FLAG_SYN) consumed++;
    if (flags & TCP_FLAG_FIN) consumed++;
    entry->seq_end = seq + consumed;

    if (data && data_len > 0) {
        entry->data = kalloc(data_len);
        if (entry->data) {
            memcpy(entry->data, data, data_len);
            entry->data_len = data_len;
        }
    }

    entry->next = NULL;
    if (!conn->retransmit_queue) {
        conn->retransmit_queue = entry;
        conn->retransmit_queue_tail = entry;
    } else {
        conn->retransmit_queue_tail->next = entry;
        conn->retransmit_queue_tail = entry;
    }
}

/**
 * Remove entries from the retransmit queue that have been fully acknowledged.
 * Also implements TCP Reno congestion control (RFC 5681).
 */
static void tcp_retransmit_ack(tcp_connection_t *conn, uint32_t ack_num) {
    uint32_t mss = conn->remote_mss;
    if (mss == 0) mss = TCP_DEFAULT_MSS;

    // Detect duplicate ACK (same ACK number, no new data acknowledged)
    if (ack_num == conn->last_ack_num && ack_num == conn->snd_una &&
        conn->retransmit_queue != NULL) {
        conn->dup_ack_count++;

        if (conn->dup_ack_count == 3 && !conn->in_fast_recovery) {
            // Fast Retransmit + Fast Recovery (RFC 5681 Section 3.2)
            serial_printf("TCP: 3 dup ACKs -> fast retransmit (cwnd %u -> ssthresh %u)\n",
                          conn->cwnd, conn->cwnd / 2);
            conn->ssthresh = conn->cwnd / 2;
            if (conn->ssthresh < 2 * mss) conn->ssthresh = 2 * mss;
            conn->cwnd = conn->ssthresh + 3 * mss;
            conn->in_fast_recovery = true;

            // Retransmit the first unacknowledged segment immediately
            tcp_retransmit_entry_t *first = conn->retransmit_queue;
            if (first) {
                tcp_send_packet(conn, first->flags, first->data, first->data_len);
                first->sent_at = tcp_now_ms();
                first->retries++;
            }
        } else if (conn->dup_ack_count > 3 && conn->in_fast_recovery) {
            // Each additional dup ACK: inflate cwnd by 1 MSS
            conn->cwnd += mss;
        }
        return;  // Don't advance snd_una on duplicate ACK
    }

    // New ACK – advance snd_una and remove acknowledged entries
    bool new_data_acked = false;
    while (conn->retransmit_queue) {
        tcp_retransmit_entry_t *entry = conn->retransmit_queue;
        int32_t diff = (int32_t)(ack_num - entry->seq_end);
        if (diff >= 0) {
            conn->retransmit_queue = entry->next;
            if (entry->data) kfree(entry->data);
            kfree(entry);
            new_data_acked = true;
        } else {
            break;
        }
    }
    if (!conn->retransmit_queue)
        conn->retransmit_queue_tail = NULL;
    conn->snd_una = ack_num;
    conn->last_ack_num = ack_num;
    conn->dup_ack_count = 0;

    if (!new_data_acked) return;

    // Congestion control: update cwnd on new ACK
    if (conn->in_fast_recovery) {
        // Exit fast recovery: deflate cwnd to ssthresh (RFC 5681 Section 3.2 step 6)
        conn->cwnd = conn->ssthresh;
        conn->in_fast_recovery = false;
        tcp_dbg("TCP: Exiting fast recovery, cwnd=%u\n", conn->cwnd);
    } else if (conn->cwnd < conn->ssthresh) {
        // Slow Start: increase cwnd by MSS for each ACK (exponential growth)
        conn->cwnd += mss;
        tcp_dbg("TCP: Slow start cwnd=%u ssthresh=%u\n", conn->cwnd, conn->ssthresh);
    } else {
        // Congestion Avoidance: increase cwnd by MSS*MSS/cwnd per ACK (linear growth)
        uint32_t inc = (mss * mss) / conn->cwnd;
        if (inc == 0) inc = 1;
        conn->cwnd += inc;
        tcp_dbg("TCP: Congestion avoidance cwnd=%u\n", conn->cwnd);
    }
}

/**
 * Flush (free) the entire retransmit queue.
 */
static void tcp_retransmit_flush(tcp_connection_t *conn) {
    tcp_retransmit_entry_t *entry = conn->retransmit_queue;
    while (entry) {
        tcp_retransmit_entry_t *next = entry->next;
        if (entry->data) kfree(entry->data);
        kfree(entry);
        entry = next;
    }
    conn->retransmit_queue = NULL;
    conn->retransmit_queue_tail = NULL;
}

/**
 * Check all connections for retransmission timeouts and resend expired segments.
 * Should be called periodically from a timer or network task.
 */
void tcp_retransmit_check(void) {
    uint64_t now = tcp_now_ms();

    TCP_LOCK();
    tcp_connection_t *conn = tcp_connections;

    while (conn) {
        tcp_retransmit_entry_t *entry = conn->retransmit_queue;
        while (entry) {
            tcp_retransmit_entry_t *next = entry->next;
            uint64_t elapsed = now - entry->sent_at;

            if (elapsed >= entry->rto_ms) {
                if (entry->retries >= TCP_MAX_RETRIES) {
                    // Too many retries – give up on this connection
                    printf("TCP: Max retries reached for seq %u, resetting connection\n",
                           entry->seq_start);
                    conn->state = TCP_STATE_CLOSED;
                    tcp_retransmit_flush(conn);
                    break;
                }

                // Congestion control: timeout = severe congestion (RFC 5681 Section 3.1)
                {
                    uint32_t mss_val = conn->remote_mss;
                    if (mss_val == 0) mss_val = TCP_DEFAULT_MSS;
                    conn->ssthresh = conn->cwnd / 2;
                    if (conn->ssthresh < 2 * mss_val) conn->ssthresh = 2 * mss_val;
                    conn->cwnd = mss_val;  // Reset to 1 MSS
                    conn->in_fast_recovery = false;
                    conn->dup_ack_count = 0;
                    serial_printf("TCP: RTO timeout -> cwnd=%u ssthresh=%u\n",
                                  conn->cwnd, conn->ssthresh);
                }

                // Retransmit: resend the segment
                NET_RX_LOG("TCP: Retransmitting seq %u (attempt %d, rto=%u ms)\n",
                       entry->seq_start, entry->retries + 1, entry->rto_ms);

                // Save and restore seq_num to resend with the original sequence
                uint32_t saved_seq = conn->seq_num;
                conn->seq_num = entry->seq_start;
                tcp_send_packet(conn, entry->flags, entry->data, entry->data_len);
                conn->seq_num = saved_seq;  // Don't advance seq_num for retransmits

                // Exponential backoff
                entry->retries++;
                entry->rto_ms *= 2;
                if (entry->rto_ms > TCP_RTO_MAX_MS) entry->rto_ms = TCP_RTO_MAX_MS;
                entry->sent_at = now;
            }
            entry = next;
        }
        conn = conn->next;
    }
    TCP_UNLOCK();
}

// ---- MSS option parsing -----------------------------------------------------

/**
 * Parse TCP options from SYN/SYN-ACK segments to extract MSS.
 */
static void tcp_parse_options(tcp_connection_t *conn, const uint8_t *opts, uint32_t opts_len) {
    uint32_t i = 0;
    while (i < opts_len) {
        uint8_t kind = opts[i];
        if (kind == 0) break;       // End of options
        if (kind == 1) { i++; continue; }  // NOP (padding)

        if (i + 1 >= opts_len) break;
        uint8_t len = opts[i + 1];
        if (len < 2 || i + len > opts_len) break;

        if (kind == 2 && len == 4) {
            // MSS option
            uint16_t mss = ((uint16_t)opts[i + 2] << 8) | opts[i + 3];
            if (mss > 0) {
                conn->remote_mss = mss;
                NET_RX_LOG("TCP: Peer MSS = %u\n", mss);
            }
        }

        i += len;
    }
}

// ---- Packet sending with options --------------------------------------------

static int tcp_send_packet_with_options(tcp_connection_t *conn, uint8_t flags,
                                        const uint8_t *data, uint32_t data_len,
                                        const uint8_t *options, uint32_t options_len) {
    // TCP header size must be a multiple of 4; options are padded with NOP
    uint32_t header_size = sizeof(tcp_header_t) + options_len;
    // Pad to 4-byte boundary
    uint32_t padding = (4 - (header_size % 4)) % 4;
    header_size += padding;

    uint32_t packet_size = header_size + data_len;
    uint8_t *packet = kalloc(packet_size);
    if (!packet) {
        printf("TCP: Failed to allocate packet buffer\n");
        return -ENOMEM;
    }
    memset(packet, 0, packet_size);

    // Build TCP header
    tcp_header_t *tcp_header = (tcp_header_t*)packet;
    tcp_header->src_port = conn->local_port;
    tcp_header->dst_port = conn->remote_port;
    tcp_header->seq_num = conn->seq_num;
    tcp_header->ack_num = conn->ack_num;
    tcp_header->data_offset = (header_size / 4) << 4;
    tcp_header->flags = flags;

    // Flow control: advertise our available receive window
    uint32_t avail_window = conn->recv_buffer_size - conn->recv_buffer_used;
    if (avail_window > 0xFFFF) avail_window = 0xFFFF;
    tcp_header->window = (uint16_t)avail_window;
    tcp_header->checksum = 0;
    tcp_header->urgent_ptr = 0;

    // Convert to network byte order
    tcp_header->src_port = ((tcp_header->src_port >> 8) & 0xFF) | ((tcp_header->src_port & 0xFF) << 8);
    tcp_header->dst_port = ((tcp_header->dst_port >> 8) & 0xFF) | ((tcp_header->dst_port & 0xFF) << 8);
    tcp_header->seq_num = ((conn->seq_num >> 24) & 0xFF) | ((conn->seq_num >> 8) & 0xFF00) |
                         ((conn->seq_num & 0xFF00) << 8) | ((conn->seq_num & 0xFF) << 24);
    tcp_header->ack_num = ((conn->ack_num >> 24) & 0xFF) | ((conn->ack_num >> 8) & 0xFF00) |
                         ((conn->ack_num & 0xFF00) << 8) | ((conn->ack_num & 0xFF) << 24);
    tcp_header->window = ((tcp_header->window >> 8) & 0xFF) | ((tcp_header->window & 0xFF) << 8);

    // Copy options after header
    if (options && options_len > 0) {
        memcpy(packet + sizeof(tcp_header_t), options, options_len);
    }

    // Copy data
    if (data && data_len > 0) {
        memcpy(packet + header_size, data, data_len);
    }

    // Calculate checksum
    tcp_header->checksum = tcp_checksum(conn->local_ip, conn->remote_ip, packet, packet_size);
    tcp_header->checksum = ((tcp_header->checksum >> 8) & 0xFF) | ((tcp_header->checksum & 0xFF) << 8);

    // Send packet
    tcp_dbg("TCP: send_packet flags=0x%02x seq=%u ack=%u size=%u -> %d.%d.%d.%d:%u\n",
            flags, conn->seq_num, conn->ack_num, packet_size,
            conn->remote_ip[0], conn->remote_ip[1],
            conn->remote_ip[2], conn->remote_ip[3], conn->remote_port);
    int result = ip_send_packet(conn->remote_ip, 6, packet, packet_size);
    tcp_dbg("TCP: ip_send_packet returned %d\n", result);
    kfree(packet);

    if (result == 0) {
        // Update sequence number
        if (data_len > 0 && (flags & TCP_FLAG_SYN)) {
            conn->seq_num += data_len + 1;
        } else if (data_len > 0) {
            conn->seq_num += data_len;
        } else if (flags & TCP_FLAG_SYN) {
            conn->seq_num += 1;
        } else if (flags & TCP_FLAG_FIN) {
            conn->seq_num += 1;
        }
    }

    return result;
}

static int tcp_send_packet(tcp_connection_t *conn, uint8_t flags, const uint8_t *data, uint32_t data_len) {
    return tcp_send_packet_with_options(conn, flags, data, data_len, NULL, 0);
}

/*
 * Send a bare RST segment without a connection object (e.g. in response to a
 * SYN aimed at a closed port, RFC 793). seq/ack are host byte order.
 */
static void tcp_send_rst_raw(const uint8_t *local_ip, uint16_t local_port,
                             const uint8_t *remote_ip, uint16_t remote_port,
                             uint32_t seq, uint32_t ack, uint8_t flags) {
    uint8_t packet[sizeof(tcp_header_t)];
    memset(packet, 0, sizeof(packet));

    tcp_header_t *hdr = (tcp_header_t *)packet;
    hdr->src_port    = (uint16_t)((local_port >> 8) | (local_port << 8));
    hdr->dst_port    = (uint16_t)((remote_port >> 8) | (remote_port << 8));
    hdr->seq_num     = ((seq >> 24) & 0xFF) | ((seq >> 8) & 0xFF00) |
                       ((seq & 0xFF00) << 8) | ((seq & 0xFF) << 24);
    hdr->ack_num     = ((ack >> 24) & 0xFF) | ((ack >> 8) & 0xFF00) |
                       ((ack & 0xFF00) << 8) | ((ack & 0xFF) << 24);
    hdr->data_offset = (sizeof(tcp_header_t) / 4) << 4;
    hdr->flags       = flags;
    hdr->window      = 0;
    hdr->urgent_ptr  = 0;
    hdr->checksum    = 0;

    uint16_t csum = tcp_checksum(local_ip, remote_ip, packet, sizeof(packet));
    hdr->checksum = (uint16_t)((csum >> 8) | (csum << 8));

    ip_send_packet(remote_ip, 6, packet, sizeof(packet));
}

// Connection establishment
tcp_connection_t* tcp_connect(const uint8_t *remote_ip, uint16_t remote_port,
                             void (*connect_callback)(tcp_connection_t *conn)) {
    uint8_t local_ip[4];
    ip_select_src(remote_ip, local_ip);   /* 127.x talks from 127.x */

    // Find an available local port (49152-65535)
    uint16_t local_port = 49152;
    while (tcp_find_connection(local_ip, local_port, remote_ip, remote_port) != NULL) {
        local_port++;
        if (local_port == 0) {
            printf("TCP: No available local ports\n");
            return NULL;
        }
    }

    // Create connection
    tcp_connection_t *conn = tcp_create_connection(local_ip, local_port, remote_ip, remote_port);
    if (!conn) {
        return NULL;
    }

    TCP_LOCK();
    conn->connect_callback = connect_callback;
    conn->state = TCP_STATE_SYN_SENT;
    conn->seq_num = tcp_isn_counter++;
    conn->initial_seq = conn->seq_num;
    conn->snd_una = conn->seq_num;

    serial_printf("TCP: Connecting to %d.%d.%d.%d:%u from port %u\n",
           remote_ip[0], remote_ip[1], remote_ip[2], remote_ip[3], remote_port, local_port);

    // Build MSS option for SYN: kind=2, len=4, MSS value (big-endian)
    uint8_t mss_opt[4];
    mss_opt[0] = 2;   // Kind: MSS
    mss_opt[1] = 4;   // Length
    mss_opt[2] = (uint8_t)(conn->local_mss >> 8);
    mss_opt[3] = (uint8_t)(conn->local_mss & 0xFF);

    // Enqueue SYN for retransmission before sending
    uint32_t syn_seq = conn->seq_num;
    tcp_retransmit_enqueue(conn, syn_seq, TCP_FLAG_SYN, NULL, 0);

    // Send SYN packet with MSS option
    if (tcp_send_packet_with_options(conn, TCP_FLAG_SYN, NULL, 0, mss_opt, 4) != 0) {
        printf("TCP: Failed to send SYN packet\n");
        tcp_retransmit_flush(conn);
        tcp_destroy_connection_locked(conn);
        TCP_UNLOCK();
        return NULL;
    }
    TCP_UNLOCK();

    return conn;
}

// Data transmission with flow control and MSS segmentation
int tcp_send(tcp_connection_t *conn, const uint8_t *data, uint32_t len) {
    if (!conn) return -EINVAL;

    TCP_LOCK();
    if (conn->state != TCP_STATE_ESTABLISHED && conn->state != TCP_STATE_CLOSE_WAIT) {
        TCP_UNLOCK();
        printf("TCP: Connection not established\n");
        return -ENOTCONN;
    }

    // Flow control: effective window = min(cwnd, remote_window)
    uint32_t effective_window = conn->cwnd;
    if (conn->remote_window < effective_window)
        effective_window = conn->remote_window;

    // Calculate bytes already in-flight (sent but not yet ACKed)
    uint32_t in_flight = (uint32_t)(conn->seq_num - conn->snd_una);
    uint32_t send_limit = (effective_window > in_flight) ? (effective_window - in_flight) : 0;

    if (send_limit == 0) {
        tcp_dbg("TCP: Window full (cwnd=%u rwnd=%u inflight=%u)\n",
                conn->cwnd, conn->remote_window, in_flight);
        TCP_UNLOCK();
        return 0;  // Window full – caller should retry later
    }
    if (len > send_limit) len = send_limit;

    tcp_dbg("TCP: Sending %u bytes (cwnd=%u rwnd=%u inflight=%u)\n",
            len, conn->cwnd, conn->remote_window, in_flight);

    // Segment data according to negotiated MSS
    uint32_t mss = conn->remote_mss;
    if (mss == 0) mss = TCP_DEFAULT_MSS;
    uint32_t sent = 0;

    while (sent < len) {
        uint32_t chunk = len - sent;
        if (chunk > mss) chunk = mss;

        uint8_t flags = TCP_FLAG_ACK;
        // Set PSH on the last segment
        if (sent + chunk >= len) flags |= TCP_FLAG_PSH;

        uint32_t seg_seq = conn->seq_num;
        tcp_retransmit_enqueue(conn, seg_seq, flags, data + sent, chunk);

        if (tcp_send_packet(conn, flags, data + sent, chunk) != 0) {
            printf("TCP: Failed to send segment at offset %u\n", sent);
            TCP_UNLOCK();
            return (sent > 0) ? (int)sent : -EIO;
        }

        sent += chunk;
    }

    TCP_UNLOCK();
    return (int)sent;
}

// Connection closure
int tcp_close(tcp_connection_t *conn) {
    if (!conn) return -EINVAL;

    TCP_LOCK();
    if (conn->owned) conn->disowned = true;
    conn->owned = false;
    conn->half_close = false;
    if (conn->state == TCP_STATE_LISTEN && conn->disowned && !conn->user_child) {
        /* Socket listener going away: drop children nobody accepted. */
        tcp_connection_t *c = tcp_connections, *nx;
        while (c) {
            nx = c->next;
            if (c != conn && c->user_child && !c->accepted && c->local_port == conn->local_port)
                tcp_destroy_connection_locked(c);
            c = nx;
        }
    }
    if (conn->state == TCP_STATE_ESTABLISHED) {
        serial_printf("TCP: Closing connection\n");
        conn->state = TCP_STATE_FIN_WAIT_1;
        uint32_t fin_seq = conn->seq_num;
        tcp_retransmit_enqueue(conn, fin_seq, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0);
        tcp_send_packet(conn, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0);
    } else if (conn->state == TCP_STATE_CLOSE_WAIT) {
        /* Peer already half-closed (socket semantics): send our FIN now. */
        conn->state = TCP_STATE_LAST_ACK;
        uint32_t fin_seq = conn->seq_num;
        tcp_retransmit_enqueue(conn, fin_seq, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0);
        tcp_send_packet(conn, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0);
    } else if (conn->state == TCP_STATE_LAST_ACK) {
        /* FIN already out; tcp_cleanup_stale_connections() reaps it. */
    } else {
        tcp_destroy_connection_locked(conn);
    }
    TCP_UNLOCK();

    return 0;
}

// Packet handling
void tcp_handle_packet(const uint8_t *frame, uint32_t len) {
    if (len < 34) { // Ethernet (14) + minimal IP (20)
        tcp_dbg("TCP: Packet too short\n");
        return;
    }
    uint32_t ip_header_len = (frame[14] & 0x0F) * 4;
    uint32_t tcp_start = 14 + ip_header_len;
    if (ip_header_len < 20 || len < tcp_start + 20) {
        return;
    }

    // Extract IPs from IP header (fixed offsets in IP header)
    uint8_t src_ip[4], dst_ip[4];
    memcpy(src_ip, frame + 26, 4);
    memcpy(dst_ip, frame + 30, 4);

    // Extract ports from TCP header (at variable tcp_start)
    uint16_t src_port = (frame[tcp_start + 0] << 8) | frame[tcp_start + 1];
    uint16_t dst_port = (frame[tcp_start + 2] << 8) | frame[tcp_start + 3];

    tcp_dbg("TCP: Received packet from %d.%d.%d.%d:%u to %d.%d.%d.%d:%u\n",
           src_ip[0], src_ip[1], src_ip[2], src_ip[3], src_port,
           dst_ip[0], dst_ip[1], dst_ip[2], dst_ip[3], dst_port);

    /*
     * Determine the true TCP segment length. Ethernet frames shorter than
     * 60 bytes are padded, so `len - tcp_start` can include padding; use the
     * IP total_length when it is sane, and fall back to the frame length for
     * drivers that report total_length incorrectly.
     */
    uint32_t ip_total = ((uint32_t)frame[16] << 8) | frame[17];
    bool ip_len_sane = (ip_total >= ip_header_len + 20) && (14 + ip_total <= len);
    uint32_t tcp_seg_len = ip_len_sane ? (ip_total - ip_header_len)
                                       : (len - tcp_start);

    // Validate the TCP checksum on receive (only when the segment length is
    // trustworthy; summing over padding would reject valid packets).
    if (ip_len_sane && tcp_seg_len <= 0xFFFF) {
        if (tcp_checksum(src_ip, dst_ip, frame + tcp_start,
                         (uint16_t)tcp_seg_len) != 0) {
            serial_printf("TCP: Bad rx checksum from %d.%d.%d.%d:%u, dropping\n",
                          src_ip[0], src_ip[1], src_ip[2], src_ip[3], src_port);
            return;
        }
    }

    uint8_t rx_flags = frame[tcp_start + 13] & 0x3F;
    uint32_t rx_seq = ((uint32_t)frame[tcp_start + 4] << 24) |
                      ((uint32_t)frame[tcp_start + 5] << 16) |
                      ((uint32_t)frame[tcp_start + 6] << 8)  |
                       (uint32_t)frame[tcp_start + 7];
    uint32_t rx_ack = ((uint32_t)frame[tcp_start + 8] << 24) |
                      ((uint32_t)frame[tcp_start + 9] << 16) |
                      ((uint32_t)frame[tcp_start + 10] << 8) |
                       (uint32_t)frame[tcp_start + 11];

    // Find existing connection
    tcp_connection_t *conn = tcp_find_connection(dst_ip, dst_port, src_ip, src_port);

    // If no connection found, check if it's a new connection (SYN packet)
    if (!conn) {
        if (rx_flags & TCP_FLAG_SYN) {
            // Only accept if we have a registered listener on this port
            tcp_listener_t *listener = tcp_find_listener(dst_port);
            tcp_connection_t *ulisten = listener ? NULL : tcp_find_user_listen(dst_port);
            if (ulisten) {
                conn = tcp_create_connection(dst_ip, dst_port, src_ip, src_port);
                if (!conn) return;
                conn->state = TCP_STATE_LISTEN;
                conn->user_child = true;
                conn->half_close = true;
            } else if (!listener) {
                serial_printf("TCP: No listener on port %u, sending RST\n", dst_port);
                // RFC 793: <SEQ=0><ACK=SEG.SEQ+SEG.LEN+1><CTL=RST,ACK>
                tcp_send_rst_raw(dst_ip, dst_port, src_ip, src_port,
                                 0, rx_seq + 1, TCP_FLAG_RST | TCP_FLAG_ACK);
                return;
            }

            tcp_dbg("TCP: New connection attempt detected\n");

            // Create the connection in LISTEN state so tcp_handle_syn
            // will send a SYN-ACK when it is called below.
            if (!conn) conn = tcp_create_connection(dst_ip, dst_port, src_ip, src_port);
            if (conn && !conn->user_child) {
                conn->state = TCP_STATE_LISTEN;
                /* Stash accept_callback so it can be fired on ESTABLISHED */
                conn->connect_callback = listener->accept_callback;
            }
        } else if (!(rx_flags & TCP_FLAG_RST)) {
            // Segment for a non-existent connection: reply with RST
            tcp_send_rst_raw(dst_ip, dst_port, src_ip, src_port,
                             rx_ack, 0, TCP_FLAG_RST);
            return;
        }
    }

    if (!conn) {
        tcp_dbg("TCP: No connection found, ignoring packet\n");
        return;
    }

    // Update connection activity timestamp
    tcp_update_activity(conn);

    tcp_header_t tcp_header;
    memcpy(&tcp_header, frame + tcp_start, sizeof(tcp_header_t));

    // Convert from network byte order
    tcp_header.src_port = ((tcp_header.src_port >> 8) & 0xFF) | ((tcp_header.src_port & 0xFF) << 8);
    tcp_header.dst_port = ((tcp_header.dst_port >> 8) & 0xFF) | ((tcp_header.dst_port & 0xFF) << 8);
    tcp_header.seq_num = ((tcp_header.seq_num >> 24) & 0xFF) | ((tcp_header.seq_num >> 8) & 0xFF00) |
                        ((tcp_header.seq_num & 0xFF00) << 8) | ((tcp_header.seq_num & 0xFF) << 24);
    tcp_header.ack_num = ((tcp_header.ack_num >> 24) & 0xFF) | ((tcp_header.ack_num >> 8) & 0xFF00) |
                        ((tcp_header.ack_num & 0xFF00) << 8) | ((tcp_header.ack_num & 0xFF) << 24);
    tcp_header.window = ((tcp_header.window >> 8) & 0xFF) | ((tcp_header.window & 0xFF) << 8);

    uint8_t data_offset = (tcp_header.data_offset >> 4) * 4;
    if (ip_header_len < 20 || data_offset < 20) {
        tcp_dbg("TCP: Invalid header sizes (ip=%u tcp=%u)\n", ip_header_len, data_offset);
        return;
    }
    if (tcp_start + data_offset > len) {
        tcp_dbg("TCP: Truncated frame for header lengths\n");
        return;
    }
    uint32_t tcp_payload_len = (tcp_seg_len >= data_offset) ? (tcp_seg_len - data_offset) : 0;

    tcp_dbg("TCP: Payload length: %u bytes\n", tcp_payload_len);

    /*
     * State machine runs under tcp_lock: the rx path (net thread), the
     * retransmit timer, and syscall-context send/close all mutate connection
     * state. User callbacks are deferred until after the lock is dropped so
     * they can safely call tcp_send()/tcp_close() without deadlocking.
     */
    void (*deferred_connect_cb)(tcp_connection_t *conn) = NULL;
    bool deferred_data_cb = false;
    uint32_t deliver_len = 0;
    const uint8_t *payload_ptr = frame + tcp_start + data_offset;

    TCP_LOCK();

    // Handle different TCP states and flags
    if (tcp_header.flags & TCP_FLAG_RST) {
        /* Leave CLOSED in place so poll()/SO_ERROR can observe failure.
         * Destroying here left tcp_socket_t->connection dangling (UAF) and
         * poll never reported POLLOUT|POLLERR, so nonblocking connect hung. */
        serial_printf("TCP: Received RST, closing connection\n");
        conn->state = TCP_STATE_CLOSED;
        tcp_retransmit_flush(conn);
        if (conn->disowned || (conn->user_child && !conn->accepted))
            tcp_destroy_connection_locked(conn);   /* no owner left to look */
        TCP_UNLOCK();
        return;
    }

    // Update remote window from every incoming segment (flow control)
    conn->remote_window = tcp_header.window;

    // Process ACK: remove acknowledged segments from retransmit queue
    if (tcp_header.flags & TCP_FLAG_ACK) {
        tcp_retransmit_ack(conn, tcp_header.ack_num);
    }

    // Parse TCP options (for MSS in SYN/SYN-ACK)
    if (tcp_header.flags & TCP_FLAG_SYN) {
        uint8_t hdr_len = (tcp_header.data_offset >> 4) * 4;
        if (hdr_len > sizeof(tcp_header_t)) {
            uint32_t opts_len = hdr_len - sizeof(tcp_header_t);
            const uint8_t *opts = frame + tcp_start + sizeof(tcp_header_t);
            if (tcp_start + hdr_len <= len) {
                tcp_parse_options(conn, opts, opts_len);
            }
        }
    }

    switch (conn->state) {
        case TCP_STATE_LISTEN:
            if (tcp_header.flags & TCP_FLAG_SYN) {
                tcp_handle_syn(conn, &tcp_header, tcp_payload_len);
            }
            break;

        case TCP_STATE_SYN_SENT:
            if ((tcp_header.flags & (TCP_FLAG_SYN | TCP_FLAG_ACK)) == (TCP_FLAG_SYN | TCP_FLAG_ACK)) {
                // Received SYN-ACK, send ACK
                serial_printf("TCP: Received SYN-ACK, sending ACK\n");
                conn->ack_num = tcp_header.seq_num + 1;
                conn->snd_una = tcp_header.ack_num;
                tcp_retransmit_ack(conn, tcp_header.ack_num);  // ACK our SYN
                tcp_send_packet(conn, TCP_FLAG_ACK, NULL, 0);
                conn->state = TCP_STATE_ESTABLISHED;
                conn->connected = true;

                // RFC 5681: IW = min(4*SMSS, max(2*SMSS, 4380))
                {
                    uint32_t smss = conn->remote_mss;
                    if (smss == 0) smss = TCP_DEFAULT_MSS;
                    uint32_t iw = 2 * smss;
                    if (iw < 4380) iw = 4380;
                    if (iw > 4 * smss) iw = 4 * smss;
                    conn->cwnd = iw;
                    conn->last_ack_num = conn->snd_una;
                }

                deferred_connect_cb = conn->connect_callback;
            }
            break;

        case TCP_STATE_SYN_RECEIVED:
            if ((tcp_header.flags & TCP_FLAG_ACK) == 0 &&
                (tcp_header.flags & TCP_FLAG_SYN)) {
                /* Retransmitted SYN – re-send our original SYN-ACK.
                 * Must use the initial_seq so the peer recognises it;
                 * tcp_send_packet_with_options would otherwise use the
                 * already-incremented seq_num and the peer will ignore it. */
                serial_printf("TCP: Re-sending SYN-ACK (retransmitted SYN)\n");
                uint32_t saved_seq = conn->seq_num;
                conn->seq_num = conn->initial_seq;
                uint16_t mss = conn->local_mss ? conn->local_mss : TCP_DEFAULT_MSS;
                uint8_t mss_opt[4] = { 2, 4, (uint8_t)(mss >> 8), (uint8_t)(mss & 0xFF) };
                tcp_send_packet_with_options(conn, TCP_FLAG_SYN | TCP_FLAG_ACK, NULL, 0, mss_opt, 4);
                conn->seq_num = saved_seq;
                break;
            }
            if (tcp_header.flags & TCP_FLAG_ACK) {
                // Received ACK for our SYN-ACK – connection is now ESTABLISHED
                serial_printf("TCP: Connection established (server side)\n");
                conn->state = TCP_STATE_ESTABLISHED;
                conn->connected = true;
                // Set initial congestion window (same as client-side)
                {
                    uint32_t smss = conn->remote_mss;
                    if (smss == 0) smss = TCP_DEFAULT_MSS;
                    uint32_t iw = 2 * smss;
                    if (iw < 4380) iw = 4380;
                    if (iw > 4 * smss) iw = 4 * smss;
                    conn->cwnd = iw;
                    conn->last_ack_num = conn->snd_una;
                }
                /* Fire the accept callback (registered by tcp_listen) */
                deferred_connect_cb = conn->connect_callback;
                conn->connect_callback = NULL;  /* don't fire again */
            }
            break;

        case TCP_STATE_ESTABLISHED:
        {
            /* RFC 793: process the segment text first, then the FIN. A server
             * that closes right after its last write often sends data+FIN in a
             * single segment; handling FIN first used to drop that data. */
            bool in_order = (tcp_header.seq_num == conn->ack_num);
            if (tcp_payload_len > 0) {
                if (!in_order) {
                    /* Old retransmit or out-of-order: re-ACK what we have. */
                    tcp_send_packet(conn, TCP_FLAG_ACK, NULL, 0);
                    break;
                }
                uint32_t taken = tcp_handle_data(conn, payload_ptr, tcp_payload_len);
                if (taken > 0) {
                    deferred_data_cb = true;
                    deliver_len = taken;
                }
                /* Only ACK what fit in the buffer; the peer retransmits the rest
                 * once our advertised window opens again. */
                conn->ack_num = tcp_header.seq_num + taken;
                if (taken < tcp_payload_len) {
                    tcp_send_packet(conn, TCP_FLAG_ACK, NULL, 0);
                    break;  /* FIN (if any) sits after unaccepted data */
                }
            }
            if (tcp_header.flags & TCP_FLAG_FIN) {
                uint32_t fin_seq = tcp_header.seq_num + tcp_payload_len;
                if (fin_seq != conn->ack_num) {
                    tcp_send_packet(conn, TCP_FLAG_ACK, NULL, 0);
                    break;
                }
                serial_printf("TCP: Received FIN, closing connection\n");
                conn->ack_num = fin_seq + 1;
                tcp_send_packet(conn, TCP_FLAG_ACK, NULL, 0);
                conn->state = TCP_STATE_CLOSE_WAIT;
                if (!conn->half_close) {
                    tcp_send_packet(conn, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0);
                    conn->state = TCP_STATE_LAST_ACK;
                }   /* socket: the owner may still write; FIN on close/shutdown */
            } else if (tcp_payload_len > 0) {
                tcp_send_packet(conn, TCP_FLAG_ACK, NULL, 0);
            } else if (tcp_header.flags & TCP_FLAG_ACK) {
                tcp_handle_ack(conn, &tcp_header);
            }
            break;
        }

        case TCP_STATE_FIN_WAIT_1:
            /* Peer may still send data after our FIN (half-close). */
            if (tcp_payload_len > 0 && tcp_header.seq_num == conn->ack_num) {
                uint32_t taken = tcp_handle_data(conn, payload_ptr, tcp_payload_len);
                if (taken > 0) { deferred_data_cb = true; deliver_len = taken; }
                conn->ack_num += taken;
                if (taken < tcp_payload_len || !(tcp_header.flags & TCP_FLAG_FIN)) {
                    tcp_send_packet(conn, TCP_FLAG_ACK, NULL, 0);
                    break;
                }
            } else if (tcp_payload_len > 0) {
                tcp_send_packet(conn, TCP_FLAG_ACK, NULL, 0);
                break;
            }
            if ((tcp_header.flags & (TCP_FLAG_FIN | TCP_FLAG_ACK)) == (TCP_FLAG_FIN | TCP_FLAG_ACK)) {
                // Received FIN-ACK, send ACK
                serial_printf("TCP: Received FIN-ACK, sending final ACK\n");
                conn->ack_num = tcp_header.seq_num + tcp_payload_len + 1;
                tcp_send_packet(conn, TCP_FLAG_ACK, NULL, 0);
                conn->state = TCP_STATE_TIME_WAIT;
                tcp_retransmit_flush(conn);
                if (conn->owned) break;   /* after shutdown(SHUT_WR): owner frees on close */
                tcp_destroy_connection_locked(conn);
                TCP_UNLOCK();
                return;
            } else if (tcp_header.flags & TCP_FLAG_FIN) {
                // Received FIN, send ACK
                conn->ack_num = tcp_header.seq_num + tcp_payload_len + 1;
                tcp_send_packet(conn, TCP_FLAG_ACK, NULL, 0);
                conn->state = TCP_STATE_CLOSING;
            }
            break;

        case TCP_STATE_LAST_ACK:
            if (tcp_header.flags & TCP_FLAG_ACK) {
                // Received final ACK. Do not free here: the owner (curl,
                // a socket, telnet) may still hold the pointer and read
                // recv_buffer; it frees the connection with tcp_close(), and
                // tcp_cleanup_stale_connections() reaps unowned CLOSED ones.
                // (Freeing here caused a use-after-free + double free when the
                // server's FIN and the final ACK arrived before curl looked.)
                serial_printf("TCP: Connection closed\n");
                tcp_retransmit_flush(conn);
                conn->state = TCP_STATE_CLOSED;
                if (conn->disowned) {          /* socket already closed it */
                    tcp_destroy_connection_locked(conn);
                    TCP_UNLOCK();
                    return;
                }
                break;
            }
            break;

        default:
            serial_printf("TCP: Unhandled state %d\n", conn->state);
            break;
    }

    TCP_UNLOCK();

    /* Run user callbacks outside the lock so they may call tcp_send() etc. */
    if (deferred_connect_cb) {
        deferred_connect_cb(conn);
    }
    if (deferred_data_cb && conn->data_callback) {
        conn->data_callback(conn, payload_ptr, deliver_len);
    }
}

/* Append in-order payload to the connection's receive buffer so that
 * tcp_socket_recv() actually returns data (previously rx data was only ever
 * handed to data_callback and recv() always saw an empty buffer).
 * Caller must hold tcp_lock. */
static uint32_t tcp_handle_data(tcp_connection_t *conn, const uint8_t *data, uint32_t len) {
    tcp_dbg("TCP: Received %u bytes of data\n", len);

    if (!conn->recv_buffer || len == 0) return len;  /* callback-only conn */

    uint32_t space = conn->recv_buffer_size - conn->recv_buffer_used;
    uint32_t to_copy = (len < space) ? len : space;
    if (to_copy < len) {
        serial_printf("TCP: recv buffer full, dropping %u bytes\n", len - to_copy);
    }
    if (to_copy > 0) {
        memcpy(conn->recv_buffer + conn->recv_buffer_used, data, to_copy);
        conn->recv_buffer_used += to_copy;
    }
    return to_copy;
}

static void tcp_handle_ack(tcp_connection_t *conn, const tcp_header_t *tcp_header) {
    // Update our sequence number based on acknowledgment
    if (tcp_header->ack_num > conn->seq_num) {
        NET_RX_LOG("TCP: ACK for sequence number %u\n", tcp_header->ack_num);
    }
}

static void tcp_handle_syn(tcp_connection_t *conn, const tcp_header_t *tcp_header, uint32_t payload_len) {
    NET_RX_LOG("TCP: Received SYN packet\n");

    if (conn->state == TCP_STATE_LISTEN) {
        // Send SYN-ACK with MSS option (many clients expect it; improves interoperability)
        conn->ack_num = tcp_header->seq_num + 1;
        conn->seq_num = tcp_isn_counter++;
        conn->initial_seq = conn->seq_num;

        uint16_t mss = conn->local_mss;
        if (mss == 0) mss = TCP_DEFAULT_MSS;
        uint8_t mss_opt[4] = { 2, 4, (uint8_t)(mss >> 8), (uint8_t)(mss & 0xFF) };
        tcp_send_packet_with_options(conn, TCP_FLAG_SYN | TCP_FLAG_ACK, NULL, 0, mss_opt, 4);
        conn->state = TCP_STATE_SYN_RECEIVED;
    }
}

static void tcp_handle_fin(tcp_connection_t *conn, const tcp_header_t *tcp_header) {
    NET_RX_LOG("TCP: Received FIN packet\n");

    if (conn->state == TCP_STATE_ESTABLISHED) {
        // Send ACK and move to CLOSE_WAIT
        conn->ack_num = tcp_header->seq_num + 1;
        tcp_send_packet(conn, TCP_FLAG_ACK, NULL, 0);
        conn->state = TCP_STATE_CLOSE_WAIT;
    }
}

// Socket API implementation
tcp_socket_t* tcp_socket_create(void) {
    tcp_socket_t *sock = kalloc(sizeof(tcp_socket_t));
    if (!sock) return NULL;

    memset(sock, 0, sizeof(tcp_socket_t));
    sock->socket_fd = -1; // Not bound yet

    return sock;
}

int tcp_socket_bind(tcp_socket_t *sock, uint16_t port) {
    if (!sock) return -EINVAL;

    uint8_t local_ip[4];
    net_get_ip_address(local_ip);

    // Create a listening connection
    tcp_connection_t *conn = tcp_create_connection(local_ip, port, (uint8_t[]){0,0,0,0}, 0);
    if (!conn) return -ENOBUFS;

    conn->state = TCP_STATE_LISTEN;
    conn->owned = true;
    sock->connection = conn;
    sock->socket_fd = 0; // Dummy file descriptor

    return 0;
}

int tcp_socket_listen(tcp_socket_t *sock, int backlog) {
    if (!sock || !sock->connection) return -EINVAL;

    // For now, we don't implement backlog, just mark as listening
    sock->connection->state = TCP_STATE_LISTEN;

    return 0;
}

tcp_socket_t* tcp_socket_accept(tcp_socket_t *sock) {
    if (!sock || !sock->connection) return NULL;

    // Find a connection in SYN_RECEIVED state (ready to accept)
    TCP_LOCK();
    tcp_connection_t *conn = tcp_connections;
    /* Oldest first is not tracked; any completed, not yet accepted child of
     * this port will do.  The listener itself (remote port 0) never matches. */
    while (conn) {
        if (conn != sock->connection && conn->user_child && !conn->accepted &&
            conn->remote_port != 0 &&
            (conn->state == TCP_STATE_ESTABLISHED || conn->state == TCP_STATE_CLOSE_WAIT) &&
            conn->local_port == sock->connection->local_port) {
            break;
        }
        conn = conn->next;
    }
    if (conn) { conn->accepted = true; conn->owned = true; }
    TCP_UNLOCK();

    if (!conn) return NULL;

    // Create new socket for the accepted connection
    tcp_socket_t *new_sock = tcp_socket_create();
    if (!new_sock) { tcp_close(conn); return NULL; }

    new_sock->connection = conn;
    new_sock->socket_fd = 1; // Dummy file descriptor

    return new_sock;
}

int tcp_socket_connect(tcp_socket_t *sock, const uint8_t *remote_ip, uint16_t remote_port) {
    if (!sock) return -EINVAL;

    tcp_connection_t *conn = tcp_connect(remote_ip, remote_port, NULL);
    if (!conn) return -ECONNREFUSED;
    TCP_LOCK();
    conn->owned = true;
    conn->half_close = true;
    TCP_UNLOCK();

    sock->connection = conn;
    sock->socket_fd = 2; // Dummy file descriptor

    return 0;
}

int tcp_socket_send(tcp_socket_t *sock, const uint8_t *data, uint32_t len) {
    if (!sock || !sock->connection) return -ENOTCONN;

    return tcp_send(sock->connection, data, len);
}

int tcp_socket_recv(tcp_socket_t *sock, uint8_t *buffer, uint32_t max_len) {
    if (!sock || !sock->connection || !sock->connection->recv_buffer) return -ENOTCONN;

    TCP_LOCK();
    uint32_t available = sock->connection->recv_buffer_used;
    if (available == 0) {
        TCP_UNLOCK();
        return 0;
    }

    uint32_t to_copy = (available < max_len) ? available : max_len;
    memcpy(buffer, sock->connection->recv_buffer, to_copy);

    // Remove copied data from buffer
    uint32_t remaining = available - to_copy;
    if (remaining > 0) {
        memmove(sock->connection->recv_buffer,
                sock->connection->recv_buffer + to_copy, remaining);
    }
    sock->connection->recv_buffer_used = remaining;
    /* Window update: if the window we last advertised was nearly closed
     * (less than one MSS), tell the peer that space opened up. Without it a
     * full buffer stalls the transfer until the peer's persist timer fires. */
    {
        tcp_connection_t *c = sock->connection;
        uint32_t before = c->recv_buffer_size - available;
        uint32_t now = c->recv_buffer_size - remaining;
        if (before < 1460 && now >= 1460 &&
            (c->state == TCP_STATE_ESTABLISHED || c->state == TCP_STATE_FIN_WAIT_1))
            tcp_send_packet(c, TCP_FLAG_ACK, NULL, 0);
    }
    TCP_UNLOCK();

    return to_copy;
}

int tcp_socket_close(tcp_socket_t *sock) {
    if (!sock) return -EINVAL;

    if (sock->connection) {
        tcp_close(sock->connection);
    }

    kfree(sock);
    return 0;
}

int tcp_listen(uint16_t port, void (*accept_callback)(tcp_connection_t *conn)) {
    if (!accept_callback) return -EINVAL;
    if (tcp_listener_count >= TCP_MAX_LISTENERS) {
        printf("TCP: listener table full\n");
        return -ENOMEM;
    }
    /* If already registered, update the callback */
    tcp_listener_t *existing = tcp_find_listener(port);
    if (existing) {
        existing->accept_callback = accept_callback;
        serial_printf("TCP: updated listener on port %u\n", port);
        return 0;
    }
    tcp_listeners[tcp_listener_count].port            = port;
    tcp_listeners[tcp_listener_count].accept_callback = accept_callback;
    tcp_listener_count++;
    serial_printf("TCP: listening on port %u\n", port);
    return 0;
}


/* Socket-layer listener on @port: the LISTEN pseudo-connection created by
 * tcp_socket_bind()+listen (remote 0.0.0.0:0, any local address). */
static tcp_connection_t *tcp_find_user_listen(uint16_t port) {
    TCP_LOCK();
    tcp_connection_t *c = tcp_connections;
    while (c) {
        if (c->state == TCP_STATE_LISTEN && c->owned && c->local_port == port &&
            c->remote_port == 0 && !c->user_child)
            break;
        c = c->next;
    }
    TCP_UNLOCK();
    return c;
}

/* poll(POLLIN) on a listening socket: a completed child awaits accept(). */
int tcp_socket_accept_ready(tcp_socket_t *sock) {
    if (!sock || !sock->connection) return 0;
    int ready = 0;
    TCP_LOCK();
    for (tcp_connection_t *c = tcp_connections; c; c = c->next) {
        if (c != sock->connection && c->user_child && !c->accepted &&
            c->local_port == sock->connection->local_port &&
            (c->state == TCP_STATE_ESTABLISHED || c->state == TCP_STATE_CLOSE_WAIT)) {
            ready = 1;
            break;
        }
    }
    TCP_UNLOCK();
    return ready;
}

/* shutdown(SHUT_WR): send our FIN but keep the connection (and its receive
 * buffer) owned by the socket so the program can still read. */
int tcp_socket_shutdown_wr(tcp_socket_t *sock) {
    if (!sock || !sock->connection) return -ENOTCONN;
    tcp_connection_t *conn = sock->connection;
    TCP_LOCK();
    if (conn->state == TCP_STATE_ESTABLISHED || conn->state == TCP_STATE_CLOSE_WAIT) {
        conn->state = (conn->state == TCP_STATE_ESTABLISHED) ? TCP_STATE_FIN_WAIT_1
                                                             : TCP_STATE_LAST_ACK;
        uint32_t fin_seq = conn->seq_num;
        tcp_retransmit_enqueue(conn, fin_seq, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0);
        tcp_send_packet(conn, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0);
    }
    TCP_UNLOCK();
    return 0;
}
