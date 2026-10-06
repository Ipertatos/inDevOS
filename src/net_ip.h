#pragma once
#include <stdint.h>

// IPv4 header structure
struct ipv4_header {
    uint8_t  version_ihl;
    uint8_t  dscp_ecn;
    uint16_t total_length;
    uint16_t identification;
    uint16_t flags_fragment;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t header_checksum;
    uint8_t  src_ip[4];
    uint8_t  dst_ip[4];
} __attribute__((packed));

void ip_init(void);
void ip_handle_packet(const uint8_t *frame, uint32_t len);
int ip_send_packet(const uint8_t *dst_ip, uint8_t protocol, const uint8_t *payload, uint16_t payload_len);

// Fragment reassembly – call periodically to expire stale fragments
void ip_fragment_cleanup(void);

// ARP-pending transmit queue: flush datagrams waiting on `ip` (called by
// arp_insert) and drive retries/expiry (called from net_rx_service).
void ip_arp_resolved(const uint8_t *ip);
void ip_arpq_tick(void);