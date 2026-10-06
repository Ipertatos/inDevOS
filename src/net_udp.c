/**
 * @file net_udp.c
 * @brief UDP (User Datagram Protocol) implementation for inDevOS
 *
 * This module provides UDP networking functionality including:
 * - Packet transmission and reception
 * - Checksum calculation for data integrity
 * - Handler registration for different UDP ports
 * - Security validations and bounds checking
 *
 * @author inDevOS development team
 * @version 1.0
 * @date 2024
 */

#include "net_udp.h"
#include "../include/indevos_errno.h"
#include "net_ip.h"
#include "mem.h"
#include "string.h"
#include "stdio.h"
#include "net_device.h"
#include "hpet.h"
#include "serial.h"
#include "debug_config.h"
#include "atomic.h"

static volatile int g_udp_verbose = 0;
static spinlock_t udp_lock;

static int udp_pcb_deliver(uint16_t src_port, uint16_t dst_port,
                           const uint8_t src_ip[4], const uint8_t *payload,
                           uint16_t payload_len);

void udp_set_verbose(int enabled) {
    g_udp_verbose = enabled ? 1 : 0;
}

int udp_get_verbose(void) {
    return g_udp_verbose ? 1 : 0;
}

/**
 * @brief Pseudo header structure for UDP checksum calculation
 *
 * Used internally for computing UDP checksums as per RFC 768
 */
typedef struct {
    uint8_t src_ip[4];      /**< Source IP address */
    uint8_t dst_ip[4];      /**< Destination IP address */
    uint8_t zero;           /**< Reserved, must be zero */
    uint8_t protocol;       /**< Protocol number (17 for UDP) */
    uint16_t udp_len;       /**< UDP packet length */
} __attribute__((packed)) udp_pseudo_header_t;

/**
 * @brief Calculate UDP checksum with pseudo-header
 *
 * Implements RFC 768 UDP checksum calculation including the pseudo-header
 * for proper error detection. Returns 0 if checksum is disabled.
 *
 * @param src_ip Source IP address (4 bytes)
 * @param dst_ip Destination IP address (4 bytes)
 * @param udp Pointer to UDP packet (header + data)
 * @param len Length of UDP packet in bytes
 * @return 16-bit checksum value
 */
static uint16_t udp_checksum(const uint8_t *src_ip, const uint8_t *dst_ip, const uint8_t *udp, uint16_t len) {
    uint32_t sum = 0;

    // Add pseudo-header fields to checksum
    sum += ((uint16_t)src_ip[0] << 8) | src_ip[1];
    sum += ((uint16_t)src_ip[2] << 8) | src_ip[3];
    sum += ((uint16_t)dst_ip[0] << 8) | dst_ip[1];
    sum += ((uint16_t)dst_ip[2] << 8) | dst_ip[3];
    sum += 0x0011; // UDP protocol number
    sum += len;

    // Add UDP header and data (16-bit words)
    for (uint16_t i = 0; i + 1 < len; i += 2) {
        sum += ((uint16_t)udp[i] << 8) | udp[i + 1];
    }

    // Handle odd-length packets
    if (len & 1) {
        sum += ((uint16_t)udp[len - 1]) << 8;
    }

    // Fold 32-bit sum to 16 bits with carries
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return (uint16_t)(~sum);
}

/**
 * @brief Initialize UDP subsystem
 *
 * Sets up UDP protocol support and prepares handler registration table.
 * This function should be called during network stack initialization.
 */
void udp_init(void) {
    printf("UDP: Initialized\n");
}

/**
 * @brief Send UDP packet to specified destination
 *
 * Creates and transmits a UDP packet with proper header construction,
 * checksum calculation, and security validations.
 *
 * @param dst_ip Destination IPv4 address (4 bytes, network order)
 * @param src_port Source UDP port (host order)
 * @param dst_port Destination UDP port (host order)
 * @param payload Pointer to data payload (can be NULL for empty packets)
 * @param len Length of payload in bytes (0 for empty packets)
 * @return 0 on success, -1 on error
 */
int udp_send(const uint8_t *dst_ip, uint16_t src_port, uint16_t dst_port, const uint8_t *payload, uint16_t len) {
    // Input validation for security
    if (!dst_ip) {
        printf("UDP: Invalid destination IP address\n");
        return -EINVAL;
    }

    // Prevent integer overflow in packet length calculation
    if (len > 65527) { // Max UDP payload (65535 - 8 byte header)
        printf("UDP: Payload too large (%u bytes)\n", len);
        return -EMSGSIZE;
    }

    uint16_t udp_len = (uint16_t)(8 + len);

    uint8_t *packet = kalloc(udp_len);
    if (!packet) {
        printf("UDP: Failed to allocate packet buffer\n");
        return -ENOMEM;
    }

    udp_header_t *hdr = (udp_header_t*)packet;
    hdr->src_port = (uint16_t)(((src_port & 0xFF) << 8) | (src_port >> 8));
    hdr->dst_port = (uint16_t)(((dst_port & 0xFF) << 8) | (dst_port >> 8));
    hdr->length = (uint16_t)(((udp_len & 0xFF) << 8) | (udp_len >> 8));
    hdr->checksum = 0;
    if (len > 0 && payload) memcpy(packet + 8, payload, len);

    uint8_t src_ip[4];
    extern void net_get_ip_address(uint8_t *ip);
    net_get_ip_address(src_ip);
    /* Checksum must be stored in network byte order (same swap as tcp_checksum in net_tcp.c). */
    uint16_t cs = udp_checksum(src_ip, dst_ip, packet, udp_len);
    if (cs == 0)
        cs = 0xFFFF; /* RFC 768: zero checksum means unused; transmit as all-ones */
    hdr->checksum = (uint16_t)(((cs & 0xFF) << 8) | (cs >> 8));

    int rc = ip_send_packet(dst_ip, 17, packet, udp_len);
    kfree(packet);
    return rc;
}

/**
 * @brief Handle incoming UDP packet
 *
 * Processes received UDP packets by validating headers, extracting payload,
 * and dispatching to registered port handlers.
 *
 * @param frame Pointer to complete Ethernet frame
 * @param len Total frame length in bytes
 */
void udp_handle_packet(const uint8_t *frame, uint32_t len) {
    // Validate minimum packet size (Ethernet + IPv4 + UDP headers)
    if (len < 14 + 20 + 8) {
        printf("UDP: Frame too short (%u bytes, minimum %u)\n", len, 14 + 20 + 8);
        return;
    }
    const uint8_t *ip = frame + 14;
    uint8_t ihl = (ip[0] & 0x0F) * 4;
    if (len < 14 + ihl + 8) { printf("UDP: Invalid IHL\n"); return; }
    const uint8_t *udp = frame + 14 + ihl;
    uint16_t total_len = ((uint16_t)udp[4] << 8) | udp[5];
    if (total_len < 8) { printf("UDP: Invalid UDP length %u\n", total_len); return; }
    if (14 + ihl + total_len > len) { printf("UDP: Truncated packet\n"); return; }
    uint16_t src_port = ((uint16_t)udp[0] << 8) | udp[1];
    uint16_t dst_port = ((uint16_t)udp[2] << 8) | udp[3];

    /* RFC 768: checksum 0 means the sender did not compute one.
     * A valid checksum makes the ones-complement sum of pseudo-header plus
     * UDP (checksum field included) come out 0. Cap the sum at the IPv4
     * payload so Ethernet padding is not treated as UDP data.
     * DHCP offers (67 -> 68) from QEMU user-net and some hypervisors carry
     * a checksum that does not match. Dropping them means no address, so
     * accept that pair after the IP checksum has already passed. */
    uint16_t rx_csum = ((uint16_t)udp[6] << 8) | udp[7];
    if (rx_csum != 0) {
        uint8_t src_ip_c[4] = { ip[12], ip[13], ip[14], ip[15] };
        uint8_t dst_ip_c[4] = { ip[16], ip[17], ip[18], ip[19] };
        uint16_t ip_total = ((uint16_t)ip[2] << 8) | ip[3];
        uint16_t csum_len = total_len;
        if (ip_total >= (uint16_t)(ihl + 8)) {
            uint16_t ip_payload = (uint16_t)(ip_total - ihl);
            if (ip_payload < csum_len)
                csum_len = ip_payload;
        }
        if (udp_checksum(src_ip_c, dst_ip_c, udp, csum_len) != 0) {
            int dhcp = (src_port == 67 && dst_port == 68);
            if (!dhcp) {
                static uint32_t bad_csum_n = 0;
                static uint64_t bad_csum_ms = 0;
                static int bad_csum_printed = 0;
                bad_csum_n++;
                uint64_t now = hpet_get_ticks();
                if (!bad_csum_printed || now - bad_csum_ms >= 1000) {
                    printf("UDP: Bad rx checksum %u->%u, dropping (%u)\n",
                           src_port, dst_port, (unsigned)bad_csum_n);
                    bad_csum_n = 0;
                    bad_csum_ms = now;
                    bad_csum_printed = 1;
                }
                return;
            }
        }
    }

    // Demux to registered handlers, then bound SOCK_DGRAM sockets
    extern udp_handler_t _udp_handlers[65536];
    uint8_t src_ip[4] = { ip[12], ip[13], ip[14], ip[15] };
    uint16_t payload_len = (uint16_t)(total_len - 8);
    klock(&udp_lock);
    udp_handler_t handler = _udp_handlers[dst_port];
    kunlock(&udp_lock);
    if (handler) {
        if (g_udp_verbose) {
            printf("UDP: Received packet %u->%u, %u bytes payload\n", src_port, dst_port, (unsigned)payload_len);
        }
        handler(src_ip, src_port, udp + 8, payload_len);
    } else if (udp_pcb_deliver(src_port, dst_port, src_ip, udp + 8, payload_len)) {
        if (g_udp_verbose) {
            printf("UDP: Socket queued %u->%u, %u bytes\n",
                   src_port, dst_port, (unsigned)payload_len);
        }
    } else {
        // Quiet by default; rate-limit unhandled-port spam.
        if (g_udp_verbose) {
            printf("UDP: No handler for port %u\n", dst_port);
        } else {
            static uint64_t last_print_ms = 0;
            static uint32_t suppressed = 0;
            suppressed++;
            uint64_t now = hpet_get_ticks();
            if (now - last_print_ms >= 1000) {
                SERIAL_DEBUG_PRINT(NET_UDP, "UDP: Suppressed %u unhandled UDP packets (last dst_port=%u)\n",
                                   (unsigned)suppressed, (unsigned)dst_port);
                suppressed = 0;
                last_print_ms = now;
            }
        }
    }
}

/** @brief Global UDP port handler table */
udp_handler_t _udp_handlers[65536] = {0};

/**
 * @brief Register UDP port handler
 *
 * Associates a callback function with a specific UDP port number.
 * Only non-privileged ports (>= 1024) can be registered by user processes.
 * System services (DHCP port 68, DNS port 53) are allowed on privileged ports.
 *
 * @param dst_port UDP port number to register (1024-65535 for user processes, 68/53 for system)
 * @param handler Function pointer to handle packets for this port
 * @return 0 on success, -1 on error
 */
int udp_register_handler(uint16_t dst_port, udp_handler_t handler) {
    // Allow DHCP (port 68) and DNS (port 53) for system services
    // Prevent registration on other privileged ports (< 1024) except for root processes
    if (dst_port < 1024 && dst_port != 68 && dst_port != 53) {
        printf("UDP: Cannot register handler on privileged port %u\n", dst_port);
        return -EACCES;
    }

    // Validate handler function pointer
    if (!handler) {
        printf("UDP: Invalid handler function pointer\n");
        return -EINVAL;
    }

    klock(&udp_lock);
    if (_udp_handlers[dst_port]) {
        kunlock(&udp_lock);
        printf("UDP: Port %u already has a registered handler\n", dst_port);
        return -EADDRINUSE;
    }

    _udp_handlers[dst_port] = handler;
    kunlock(&udp_lock);
    DEBUG_PRINT(NET_UDP, "UDP: Registered handler for port %u\n", dst_port);
    return 0;
}

/**
 * @brief Unregister UDP port handler
 *
 * Removes the handler associated with a specific UDP port.
 * Only non-privileged ports can be unregistered by user processes.
 * System services (DHCP port 68, DNS port 53) can be unregistered.
 *
 * @param dst_port UDP port number to unregister
 */
void udp_unregister_handler(uint16_t dst_port) {
    // Allow unregistering DHCP (port 68) and DNS (port 53) for system services
    if (dst_port < 1024 && dst_port != 68 && dst_port != 53) {
        printf("UDP: Cannot unregister privileged port %u\n", dst_port);
        return;
    }

    klock(&udp_lock);
    int had = _udp_handlers[dst_port] != NULL;
    _udp_handlers[dst_port] = NULL;
    kunlock(&udp_lock);
    if (had)
        DEBUG_PRINT(NET_UDP, "UDP: Unregistered handler for port %u\n", dst_port);
    else
        DEBUG_PRINT(NET_UDP, "UDP: No handler registered for port %u\n", dst_port);
}

/* ── SOCK_DGRAM protocol control blocks ────────────────────────────────── */

#define UDP_PCB_MAX     32
#define UDP_RX_MAX       8
#define UDP_RX_PAYLOAD  1472
#define UDP_EPHEMERAL_LO 49152

typedef struct {
    uint8_t  src_ip[4];
    uint16_t src_port;
    uint16_t len;
    uint8_t  data[UDP_RX_PAYLOAD];
} udp_rx_pkt_t;

struct udp_pcb {
    int      in_use;
    int      bound;
    int      connected;
    uint16_t local_port;
    uint8_t  dest_ip[4];
    uint16_t dest_port;
    udp_rx_pkt_t rx[UDP_RX_MAX];
    int      rx_head;
    int      rx_tail;
    int      rx_count;
};

static udp_pcb_t udp_pcbs[UDP_PCB_MAX];

static int udp_port_taken_unlocked(uint16_t port) {
    extern udp_handler_t _udp_handlers[65536];
    if (_udp_handlers[port]) return 1;
    for (int i = 0; i < UDP_PCB_MAX; i++) {
        if (udp_pcbs[i].in_use && udp_pcbs[i].bound &&
            udp_pcbs[i].local_port == port)
            return 1;
    }
    return 0;
}

static uint16_t udp_alloc_ephemeral_unlocked(void) {
    static uint16_t next = UDP_EPHEMERAL_LO;
    for (int n = 0; n < 16384; n++) {
        uint16_t p = next++;
        if (next < UDP_EPHEMERAL_LO) next = UDP_EPHEMERAL_LO;
        if (!udp_port_taken_unlocked(p)) return p;
    }
    return 0;
}

static int udp_pcb_deliver(uint16_t src_port, uint16_t dst_port,
                           const uint8_t src_ip[4], const uint8_t *payload,
                           uint16_t payload_len) {
    klock(&udp_lock);
    udp_pcb_t *s = NULL;
    for (int i = 0; i < UDP_PCB_MAX; i++) {
        if (udp_pcbs[i].in_use && udp_pcbs[i].bound &&
            udp_pcbs[i].local_port == dst_port) {
            s = &udp_pcbs[i];
            break;
        }
    }
    if (!s) {
        kunlock(&udp_lock);
        return 0;
    }
    if (s->rx_count >= UDP_RX_MAX) {
        /* Drop oldest */
        s->rx_head = (s->rx_head + 1) % UDP_RX_MAX;
        s->rx_count--;
    }
    udp_rx_pkt_t *pkt = &s->rx[s->rx_tail];
    memcpy(pkt->src_ip, src_ip, 4);
    pkt->src_port = src_port;
    pkt->len = payload_len > UDP_RX_PAYLOAD ? UDP_RX_PAYLOAD : payload_len;
    if (pkt->len && payload)
        memcpy(pkt->data, payload, pkt->len);
    s->rx_tail = (s->rx_tail + 1) % UDP_RX_MAX;
    s->rx_count++;
    kunlock(&udp_lock);
    return 1;
}

udp_pcb_t *udp_pcb_create(void) {
    klock(&udp_lock);
    for (int i = 0; i < UDP_PCB_MAX; i++) {
        if (udp_pcbs[i].in_use) continue;
        memset(&udp_pcbs[i], 0, sizeof(udp_pcbs[i]));
        udp_pcbs[i].in_use = 1;
        kunlock(&udp_lock);
        return &udp_pcbs[i];
    }
    kunlock(&udp_lock);
    return NULL;
}

int udp_pcb_bind(udp_pcb_t *s, uint16_t port) {
    if (!s || !s->in_use) return -EINVAL;
    klock(&udp_lock);
    if (s->bound) {
        kunlock(&udp_lock);
        return -EINVAL;
    }
    if (port == 0) {
        port = udp_alloc_ephemeral_unlocked();
        if (!port) {
            kunlock(&udp_lock);
            return -EADDRINUSE;
        }
    } else if (udp_port_taken_unlocked(port)) {
        kunlock(&udp_lock);
        return -EADDRINUSE;
    }
    s->local_port = port;
    s->bound = 1;
    kunlock(&udp_lock);
    return 0;
}

int udp_pcb_connect(udp_pcb_t *s, const uint8_t ip[4], uint16_t port) {
    if (!s || !s->in_use || !ip || !port) return -EINVAL;
    klock(&udp_lock);
    if (!s->bound) {
        uint16_t eph = udp_alloc_ephemeral_unlocked();
        if (!eph) {
            kunlock(&udp_lock);
            return -EADDRINUSE;
        }
        s->local_port = eph;
        s->bound = 1;
    }
    memcpy(s->dest_ip, ip, 4);
    s->dest_port = port;
    s->connected = 1;
    kunlock(&udp_lock);
    return 0;
}

int udp_pcb_sendto(udp_pcb_t *s, const uint8_t *data, uint16_t len,
                   const uint8_t ip[4], uint16_t port) {
    if (!s || !s->in_use) return -EINVAL;
    if (len > 0 && !data) return -EINVAL;
    if (!ip || !port) return -EDESTADDRREQ;
    uint16_t sport;
    klock(&udp_lock);
    if (!s->bound) {
        uint16_t eph = udp_alloc_ephemeral_unlocked();
        if (!eph) {
            kunlock(&udp_lock);
            return -EADDRINUSE;
        }
        s->local_port = eph;
        s->bound = 1;
    }
    sport = s->local_port;
    kunlock(&udp_lock);
    int rc = udp_send(ip, sport, port, data, len);
    return rc < 0 ? rc : (int)len;
}

int udp_pcb_recvfrom(udp_pcb_t *s, uint8_t *buf, uint16_t len,
                     uint8_t src_ip[4], uint16_t *src_port) {
    if (!s || !s->in_use) return -EINVAL;
    klock(&udp_lock);
    if (s->rx_count <= 0) {
        kunlock(&udp_lock);
        return 0;
    }
    udp_rx_pkt_t *pkt = &s->rx[s->rx_head];
    uint16_t n = pkt->len;
    if (n > len) n = len;
    if (n && buf) memcpy(buf, pkt->data, n);
    if (src_ip) memcpy(src_ip, pkt->src_ip, 4);
    if (src_port) *src_port = pkt->src_port;
    s->rx_head = (s->rx_head + 1) % UDP_RX_MAX;
    s->rx_count--;
    kunlock(&udp_lock);
    return (int)n;
}

uint16_t udp_pcb_local_port(udp_pcb_t *s) {
    if (!s || !s->in_use) return 0;
    return s->local_port;
}

int udp_pcb_rx_ready(udp_pcb_t *s) {
    if (!s || !s->in_use) return 0;
    klock(&udp_lock);
    int n = s->rx_count;
    kunlock(&udp_lock);
    return n;
}

void udp_pcb_close(udp_pcb_t *s) {
    if (!s) return;
    klock(&udp_lock);
    memset(s, 0, sizeof(*s));
    kunlock(&udp_lock);
}


