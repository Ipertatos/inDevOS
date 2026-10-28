#pragma once

#include <stdint.h>
#include <stdbool.h>

// TCP Header Structure (RFC 793)
typedef struct {
    uint16_t src_port;        // Source port
    uint16_t dst_port;        // Destination port
    uint32_t seq_num;         // Sequence number
    uint32_t ack_num;         // Acknowledgment number
    uint8_t data_offset;      // Data offset (4 bits) + reserved (4 bits)
    uint8_t flags;            // Control flags
    uint16_t window;          // Window size
    uint16_t checksum;        // Checksum
    uint16_t urgent_ptr;      // Urgent pointer
} __attribute__((packed)) tcp_header_t;

// TCP Flags
#define TCP_FLAG_FIN 0x01    // Finish
#define TCP_FLAG_SYN 0x02    // Synchronize
#define TCP_FLAG_RST 0x04    // Reset
#define TCP_FLAG_PSH 0x08    // Push
#define TCP_FLAG_ACK 0x10    // Acknowledgment
#define TCP_FLAG_URG 0x20    // Urgent
#define TCP_FLAG_ECE 0x40    // ECN Echo
#define TCP_FLAG_CWR 0x80    // Congestion Window Reduced

// TCP States
typedef enum {
    TCP_STATE_CLOSED,
    TCP_STATE_LISTEN,
    TCP_STATE_SYN_SENT,
    TCP_STATE_SYN_RECEIVED,
    TCP_STATE_ESTABLISHED,
    TCP_STATE_FIN_WAIT_1,
    TCP_STATE_FIN_WAIT_2,
    TCP_STATE_CLOSE_WAIT,
    TCP_STATE_CLOSING,
    TCP_STATE_LAST_ACK,
    TCP_STATE_TIME_WAIT
} tcp_state_t;

// Maximum Segment Size defaults
#define TCP_DEFAULT_MSS       536   // RFC 879 – minimum for IPv4
#define TCP_MAX_MSS           1460  // Typical for Ethernet (1500 - 20 IP - 20 TCP)

// Retransmission constants (RFC 6298)
#define TCP_RTO_MIN_MS        1000  // Minimum RTO (1 second)
#define TCP_RTO_MAX_MS        64000 // Maximum RTO (64 seconds)
#define TCP_MAX_RETRIES       8     // Max retransmission attempts before giving up

// Retransmission queue entry
typedef struct tcp_retransmit_entry {
    uint32_t seq_start;       // Starting sequence number of this segment
    uint32_t seq_end;         // Ending sequence number (exclusive)
    uint8_t *data;            // Copy of the segment payload (NULL for SYN/FIN)
    uint32_t data_len;        // Length of payload
    uint8_t  flags;           // TCP flags used to send this segment
    uint64_t sent_at;         // Tick count when last (re)transmitted
    uint32_t rto_ms;          // Current RTO for this entry (doubles on each retry)
    uint8_t  retries;         // Number of retransmissions so far
    struct tcp_retransmit_entry *next;
} tcp_retransmit_entry_t;

// TCP Connection Structure
typedef struct tcp_connection {
    // Connection identifiers
    uint8_t local_ip[4];
    uint8_t remote_ip[4];
    uint16_t local_port;
    uint16_t remote_port;

    // Sequence numbers
    uint32_t seq_num;         // Our next sequence number
    uint32_t ack_num;         // Expected remote sequence number
    uint32_t initial_seq;     // Initial sequence number
    uint32_t snd_una;         // Oldest unacknowledged sequence number

    // Window management (RFC 793)
    uint16_t window_size;     // Our advertised receive window
    uint16_t remote_window;   // Peer's advertised receive window
    uint16_t local_mss;       // MSS we advertise
    uint16_t remote_mss;      // MSS the peer advertised

    // Congestion control (RFC 5681 – TCP Reno)
    uint32_t cwnd;            // Congestion window (bytes)
    uint32_t ssthresh;        // Slow-start threshold (bytes)
    uint32_t dup_ack_count;   // Duplicate ACK counter (for fast retransmit)
    uint32_t last_ack_num;    // Last ACK number received (to detect duplicates)
    bool     in_fast_recovery; // Currently in fast-recovery phase

    // Connection state
    tcp_state_t state;
    bool connected;

    // Retransmission
    tcp_retransmit_entry_t *retransmit_queue;      // Head of retransmit queue
    tcp_retransmit_entry_t *retransmit_queue_tail;  // Tail for O(1) append
    uint32_t rto_ms;          // Smoothed RTO (exponential backoff base)
    uint32_t srtt_ms;         // Smoothed round-trip time
    uint32_t rttvar_ms;       // RTT variance

    // Timers
    uint64_t last_activity;
    uint32_t retransmit_timeout;

    // User callbacks
    void (*data_callback)(struct tcp_connection *conn, const uint8_t *data, uint32_t len);
    void (*connect_callback)(struct tcp_connection *conn);
    void (*close_callback)(struct tcp_connection *conn);

    // Buffer management
    uint8_t *send_buffer;
    uint32_t send_buffer_size;
    uint32_t send_buffer_used;

    uint8_t *recv_buffer;
    uint32_t recv_buffer_size;
    uint32_t recv_buffer_used;

    // Next connection in list
    struct tcp_connection *next;

    // Socket-layer ownership (appended: keeps earlier offsets stable)
    bool owned;        // a tcp_socket_t points here: never reap/free under it
    bool half_close;   // socket semantics: peer FIN -> CLOSE_WAIT, no auto-FIN
    bool accepted;     // already handed out by accept()
    bool user_child;   // created by a SYN to a socket-layer listener
    bool disowned;     // socket closed it: free as soon as the close handshake ends

    // Passive statistics ([tcpstat] serial line once a receive burst goes idle)
    uint64_t st_t_open;       // ms: connect()/SYN seen
    uint64_t st_t_first;      // ms: first payload byte received
    uint64_t st_t_last;       // ms: last payload byte received
    uint32_t st_rx;           // payload bytes accepted
    uint32_t st_ooo;          // out-of-order segments seen
    uint32_t st_rexmit;       // RTO retransmissions
    uint32_t st_fast;         // fast retransmissions
    bool     st_reported;     // current burst already reported

    // Receive ring + out-of-order queue + window scaling (batch 13)
    uint32_t recv_head;       // read offset into recv_buffer (ring); recv_buffer_used = in-order bytes
    uint32_t snd_wnd;         // peer's receive window in bytes (scaled)
    uint32_t last_wnd_adv;    // window (bytes) we last advertised
    uint8_t  rcv_wscale;      // shift applied to windows we advertise (0 unless WS agreed)
    uint8_t  snd_wscale;      // shift the peer applies to its windows
    bool     ws_peer;         // peer's SYN carried the window-scale option
    bool     ws_ok;           // window scaling in effect (both SYNs carried it)
    uint8_t  ooo_n;           // number of held out-of-order ranges
    struct { uint32_t start, end; } ooo[8];   // absolute seq ranges held in the ring past rcv_nxt
} tcp_connection_t;

// TCP Socket Structure (for user applications)
typedef struct {
    int socket_fd;
    tcp_connection_t *connection;
} tcp_socket_t;

// Function declarations
void tcp_init(void);
int tcp_listen(uint16_t port, void (*accept_callback)(tcp_connection_t *conn));
tcp_connection_t* tcp_connect(const uint8_t *remote_ip, uint16_t remote_port,
                             void (*connect_callback)(tcp_connection_t *conn));
int tcp_send(tcp_connection_t *conn, const uint8_t *data, uint32_t len);
int tcp_close(tcp_connection_t *conn);
void tcp_handle_packet(const uint8_t *frame, uint32_t len);

// Socket API (for applications)
tcp_socket_t* tcp_socket_create(void);
int tcp_socket_bind(tcp_socket_t *sock, uint16_t port);
int tcp_socket_listen(tcp_socket_t *sock, int backlog);
tcp_socket_t* tcp_socket_accept(tcp_socket_t *sock);
int tcp_socket_connect(tcp_socket_t *sock, const uint8_t *remote_ip, uint16_t remote_port);
int tcp_socket_send(tcp_socket_t *sock, const uint8_t *data, uint32_t len);
int tcp_socket_recv(tcp_socket_t *sock, uint8_t *buffer, uint32_t max_len);
/* Copy up to len in-order received bytes out of conn's receive ring (takes tcp_lock). */
uint32_t tcp_conn_read(tcp_connection_t *conn, uint8_t *buf, uint32_t len);
int tcp_socket_close(tcp_socket_t *sock);
int tcp_socket_accept_ready(tcp_socket_t *sock);
int tcp_socket_shutdown_wr(tcp_socket_t *sock);

// Utility functions
uint16_t tcp_checksum(const uint8_t *src_ip, const uint8_t *dst_ip,
                     const uint8_t *tcp_packet, uint16_t len);
void tcp_print_packet(const tcp_header_t *header);

// Connection management
void tcp_cleanup_stale_connections(void);  // Call periodically to prevent memory leaks
int tcp_get_connection_count(void);        // Get number of active connections

// Retransmission – call periodically from a timer or network task
void tcp_retransmit_check(void);


















