#pragma once
#include <stdint.h>

#define ARP_CACHE_SIZE 16
#define ARP_TIMEOUT_MS 30000  // 30 seconds
#define ARP_RETRY_COUNT 5     // More retries for reliability
#define ARP_RETRY_TIMEOUT_MS 100  // 100ms - much faster retries for low latency

// ARP opcodes
#define ARP_REQUEST 1
#define ARP_REPLY   2

// ARP packet structure
struct arp_packet {
    uint16_t htype;
    uint16_t ptype;
    uint8_t hlen;
    uint8_t plen;
    uint16_t opcode;
    uint8_t sender_mac[6];
    uint8_t sender_ip[4];
    uint8_t target_mac[6];
    uint8_t target_ip[4];
} __attribute__((packed));

// ARP cache entry
struct arp_entry {
    uint8_t ip[4];
    uint8_t mac[6];
    uint64_t timestamp;  // When this entry was created/updated
    int valid;
    int permanent;  // For static entries
};

// Pending ARP request
struct arp_pending {
    uint8_t target_ip[4];
    uint64_t timestamp;
    int retry_count;
    int valid;
};

void arp_init();
void arp_handle_packet(const uint8_t *frame, uint32_t len);
void arp_send_request(const uint8_t *target_ip);
int arp_lookup(const uint8_t *ip, uint8_t *mac_out);
void arp_insert(const uint8_t *ip, const uint8_t *mac);
void arp_cleanup_expired();
int arp_resolve(const uint8_t *ip, uint8_t *mac_out);
void arp_add_static(const uint8_t *ip, const uint8_t *mac);
void arp_get_cache_info(int *total_entries, int *valid_entries);
int arp_is_pending(const uint8_t *ip);
void arp_get_pending_info(int *total_pending); 