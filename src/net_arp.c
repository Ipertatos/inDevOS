#include "net_ethernet.h"
#include "net_arp.h"
#include "string.h"
#include "net_device.h"
#include "net_ip.h"
#include "hpet.h"
#include "ktime.h"
#include "utils.h"
#include "debug_config.h"

static struct arp_entry arp_cache[ARP_CACHE_SIZE];
static struct arp_pending arp_pending_requests[ARP_CACHE_SIZE];

void arp_init() {
    memset(arp_cache, 0, sizeof(arp_cache));
    memset(arp_pending_requests, 0, sizeof(arp_pending_requests));
    printf("ARP: Initialized with cache size %d\n", ARP_CACHE_SIZE);
}

void arp_handle_packet(const uint8_t *frame, uint32_t len) {
    if (len < 42) { // Ethernet header (14) + ARP header (28)
        NET_RX_LOG("ARP: Packet too short (%d bytes)\n", len);
        return;
    }
    
    struct arp_packet *arp = (struct arp_packet*)(frame + 14);
    
    // Check opcode in host order
    uint16_t opcode = ntohs(arp->opcode);
    
    DEBUG_PRINT(NET_ARP, "ARP: Processing packet - opcode: 0x%04x\n", opcode);
    DEBUG_PRINT(NET_ARP, "ARP: Sender IP: %d.%d.%d.%d, Target IP: %d.%d.%d.%d\n",
           arp->sender_ip[0], arp->sender_ip[1], arp->sender_ip[2], arp->sender_ip[3],
           arp->target_ip[0], arp->target_ip[1], arp->target_ip[2], arp->target_ip[3]);
    
    // Check if this is for us (target IP matches our IP)
    uint8_t our_ip[4];
    net_get_ip_address(our_ip);
    if (memcmp(arp->target_ip, our_ip, 4) == 0) {
        DEBUG_PRINT(NET_ARP, "ARP: Packet is for us!\n");
        
        if (opcode == ARP_REQUEST) { // ARP request
            DEBUG_PRINT(NET_ARP, "ARP: Received request, sending reply\n");
            
            // Update cache with sender's info
            arp_insert(arp->sender_ip, arp->sender_mac);
            
            // Send ARP reply
            uint8_t reply[64];
            memset(reply, 0, sizeof(reply));
            
            // Ethernet header
            memcpy(&reply[0], arp->sender_mac, 6); // To sender
            uint8_t our_mac[6];
            net_get_mac_address(our_mac);
            memcpy(&reply[6], our_mac, 6); // From us
            reply[12] = 0x08; reply[13] = 0x06; // ARP
            
            // ARP header
            struct arp_packet *reply_arp = (struct arp_packet*)&reply[14];
            reply_arp->htype = htons(0x0001);   // Ethernet (1)
            reply_arp->ptype = htons(0x0800);   // IPv4 (0x0800)
            reply_arp->hlen = 6;
            reply_arp->plen = 4;
            reply_arp->opcode = htons(ARP_REPLY); // ARP reply (2)
            
            // Sender (us)
            memcpy(reply_arp->sender_mac, our_mac, 6);
            memcpy(reply_arp->sender_ip, our_ip, 4);
            
            // Target (original sender)
            memcpy(reply_arp->target_mac, arp->sender_mac, 6);
            memcpy(reply_arp->target_ip, arp->sender_ip, 4);
            
            DEBUG_PRINT(NET_ARP, "ARP: Sending reply to %02x:%02x:%02x:%02x:%02x:%02x\n",
                   arp->sender_mac[0], arp->sender_mac[1], arp->sender_mac[2],
                   arp->sender_mac[3], arp->sender_mac[4], arp->sender_mac[5]);
            
            net_send_packet(reply, sizeof(reply));
            
        } else if (ntohs(arp->opcode) == ARP_REPLY) { // ARP reply
            DEBUG_PRINT(NET_ARP, "ARP: Received reply\n");
            // Clear the pending request first so the queue flush triggered by
            // arp_insert() sees a resolved entry.
            for (int i = 0; i < ARP_CACHE_SIZE; i++) {
                if (arp_pending_requests[i].valid && 
                    memcmp(arp_pending_requests[i].target_ip, arp->sender_ip, 4) == 0) {
                    DEBUG_PRINT(NET_ARP, "ARP: Resolved pending request for %d.%d.%d.%d\n",
                                arp->sender_ip[0], arp->sender_ip[1], arp->sender_ip[2], arp->sender_ip[3]);
                    arp_pending_requests[i].valid = 0;
                    break;
                }
            }
            arp_insert(arp->sender_ip, arp->sender_mac);
        }
    } else {
        DEBUG_PRINT(NET_ARP, "ARP: Packet not for us (target: %d.%d.%d.%d, our IP: %d.%d.%d.%d)\n",
                    arp->target_ip[0], arp->target_ip[1], arp->target_ip[2], arp->target_ip[3],
                    our_ip[0], our_ip[1], our_ip[2], our_ip[3]);
    }
}

void arp_send_request(const uint8_t *target_ip) {
    // Build ARP request packet
    uint8_t packet[64]; // Ethernet + ARP packet
    memset(packet, 0, sizeof(packet));
    
    // Ethernet header (14 bytes)
    // Destination: broadcast (FF:FF:FF:FF:FF:FF)
    packet[0] = 0xFF; packet[1] = 0xFF; packet[2] = 0xFF;
    packet[3] = 0xFF; packet[4] = 0xFF; packet[5] = 0xFF;
    
    // Source: our MAC address
    uint8_t our_mac[6];
    net_get_mac_address(our_mac);
    memcpy(&packet[6], our_mac, 6);
    
    // EtherType: ARP (0x0806)
    packet[12] = 0x08; packet[13] = 0x06;
    
    // ARP header (28 bytes)
    struct arp_packet *arp = (struct arp_packet*)&packet[14];
    arp->htype = htons(0x0001); // Ethernet
    arp->ptype = htons(0x0800); // IPv4
    arp->hlen = 6;       // MAC address length
    arp->plen = 4;       // IP address length
    arp->opcode = htons(ARP_REQUEST); // ARP request
    
    // Sender MAC and IP
    memcpy(arp->sender_mac, our_mac, 6);
    uint8_t our_ip[4];
    net_get_ip_address(our_ip);
    memcpy(arp->sender_ip, our_ip, 4);
    
            // Target MAC: 00:00:00:00:00:00 (unknown)
    memset(arp->target_mac, 0, 6);
    
    // Target IP
    memcpy(arp->target_ip, target_ip, 4);
    
    // Send the packet
    DEBUG_PRINT(NET_ARP, "ARP: Sending request for %d.%d.%d.%d\n",
                target_ip[0], target_ip[1], target_ip[2], target_ip[3]);
    
    // Ensure packet is at least 60 bytes (padding)
    // Ethernet (14) + ARP (28) = 42 bytes. Minimum ethernet payload is 46 bytes.
    // Total frame size minimum is 64 bytes (incl FCS) -> payload 60 bytes?
    // Standard practice: Pad payload to 46 bytes.
    // Current packet size: 14 + 28 = 42. Needs 4 bytes padding.
    // However, the buffer is 64 bytes, so we just need to send enough length.
    
    // Send 60 bytes (42 data + 18 padding) to be safe and meet minimum frame size requirements
    net_send_packet(packet, 60);
}

int arp_lookup(const uint8_t *ip, uint8_t *mac_out) {
    uint64_t current_time = ktime_now_ms();
    
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && memcmp(arp_cache[i].ip, ip, 4) == 0) {
            // Check if entry has expired (unless it's permanent)
            if (!arp_cache[i].permanent) {
                uint64_t age_ms = current_time - arp_cache[i].timestamp;
                if (age_ms > ARP_TIMEOUT_MS) {
                    DEBUG_PRINT(NET_ARP, "ARP: Cache entry expired for %d.%d.%d.%d\n",
                                ip[0], ip[1], ip[2], ip[3]);
                    arp_cache[i].valid = 0;
                    continue;
                }
            }
            
            memcpy(mac_out, arp_cache[i].mac, 6);
            return 1;
        }
    }
    return 0;
}

static void arp_cache_store(const uint8_t *ip, const uint8_t *mac);

void arp_insert(const uint8_t *ip, const uint8_t *mac) {
    arp_cache_store(ip, mac);
    ip_arp_resolved(ip);
}

static void arp_cache_store(const uint8_t *ip, const uint8_t *mac) {
    uint64_t current_time = ktime_now_ms();
    
    // First, try to update existing entry
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && memcmp(arp_cache[i].ip, ip, 4) == 0) {
            memcpy(arp_cache[i].mac, mac, 6);
            arp_cache[i].timestamp = current_time;
            DEBUG_PRINT(NET_ARP, "ARP: Updated cache entry for %d.%d.%d.%d\n",
                   ip[0], ip[1], ip[2], ip[3]);
            return;
        }
    }
    
    // Find empty slot
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!arp_cache[i].valid) {
            memcpy(arp_cache[i].ip, ip, 4);
            memcpy(arp_cache[i].mac, mac, 6);
            arp_cache[i].timestamp = current_time;
            arp_cache[i].valid = 1;
            arp_cache[i].permanent = 0;
            DEBUG_PRINT(NET_ARP, "ARP: Added cache entry for %d.%d.%d.%d\n",
                   ip[0], ip[1], ip[2], ip[3]);
            return;
        }
    }
    
    // No empty slot, replace oldest entry
    int oldest_idx = 0;
    uint64_t oldest_time = arp_cache[0].timestamp;
    
    for (int i = 1; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].timestamp < oldest_time) {
            oldest_time = arp_cache[i].timestamp;
            oldest_idx = i;
        }
    }
    
    memcpy(arp_cache[oldest_idx].ip, ip, 4);
    memcpy(arp_cache[oldest_idx].mac, mac, 6);
    arp_cache[oldest_idx].timestamp = current_time;
    arp_cache[oldest_idx].valid = 1;
    arp_cache[oldest_idx].permanent = 0;
    DEBUG_PRINT(NET_ARP, "ARP: Replaced cache entry for %d.%d.%d.%d\n",
                ip[0], ip[1], ip[2], ip[3]);
}

void arp_cleanup_expired() {
    uint64_t current_time = ktime_now_ms();
    int cleaned = 0;
    
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && !arp_cache[i].permanent) {
            uint64_t age_ms = current_time - arp_cache[i].timestamp;
            if (age_ms > ARP_TIMEOUT_MS) {
                arp_cache[i].valid = 0;
                cleaned++;
            }
        }
    }
    
    if (cleaned > 0) {
        DEBUG_PRINT(NET_ARP, "ARP: Cleaned up %d expired cache entries\n", cleaned);
    }
}

int arp_resolve(const uint8_t *ip, uint8_t *mac_out) {
    // First, try to find in cache
    if (arp_lookup(ip, mac_out)) {
        return 1;
    }
    
    // Check if we already have a pending request
    uint64_t current_time = ktime_now_ms();
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_pending_requests[i].valid && 
            memcmp(arp_pending_requests[i].target_ip, ip, 4) == 0) {
            
            // Check if we should retry
            uint64_t age_ms = current_time - arp_pending_requests[i].timestamp;
            if (age_ms > ARP_RETRY_TIMEOUT_MS) {
                if (arp_pending_requests[i].retry_count < ARP_RETRY_COUNT) {
                    arp_pending_requests[i].retry_count++;
                    arp_pending_requests[i].timestamp = current_time;
                    arp_send_request(ip);
                    DEBUG_PRINT(NET_ARP, "ARP: Retry %d for %d.%d.%d.%d\n",
                                arp_pending_requests[i].retry_count, ip[0], ip[1], ip[2], ip[3]);
                } else {
                    // Give up - timeout reached
                    arp_pending_requests[i].valid = 0;
                    printf("ARP: Failed to resolve %d.%d.%d.%d after %d retries (timeout)\n",
                           ip[0], ip[1], ip[2], ip[3], ARP_RETRY_COUNT);
                    return 0; // Failed
                }
            }
            return 0; // Still pending
        }
    }
    
    // Start new request
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!arp_pending_requests[i].valid) {
            memcpy(arp_pending_requests[i].target_ip, ip, 4);
            arp_pending_requests[i].timestamp = current_time;
            arp_pending_requests[i].retry_count = 0;
            arp_pending_requests[i].valid = 1;
            
            arp_send_request(ip);
            DEBUG_PRINT(NET_ARP, "ARP: Started resolution for %d.%d.%d.%d\n",
                        ip[0], ip[1], ip[2], ip[3]);
            return 0; // Pending
        }
    }
    
    printf("ARP: No space for pending request for %d.%d.%d.%d\n",
           ip[0], ip[1], ip[2], ip[3]);
    return 0;
}

void arp_add_static(const uint8_t *ip, const uint8_t *mac) {
    uint64_t current_time = ktime_now_ms();
    
    // Try to update existing entry
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && memcmp(arp_cache[i].ip, ip, 4) == 0) {
            memcpy(arp_cache[i].mac, mac, 6);
            arp_cache[i].timestamp = current_time;
            arp_cache[i].permanent = 1;
            printf("ARP: Updated static entry for %d.%d.%d.%d\n",
                   ip[0], ip[1], ip[2], ip[3]);
            return;
        }
    }
    
    // Add new static entry
    arp_insert(ip, mac);
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && memcmp(arp_cache[i].ip, ip, 4) == 0) {
            arp_cache[i].permanent = 1;
            break;
        }
    }
    printf("ARP: Added static entry for %d.%d.%d.%d\n",
           ip[0], ip[1], ip[2], ip[3]);
}

void arp_get_cache_info(int *total_entries, int *valid_entries) {
    *total_entries = ARP_CACHE_SIZE;
    *valid_entries = 0;
    
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid) {
            (*valid_entries)++;
        }
    }
}

int arp_is_pending(const uint8_t *ip) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_pending_requests[i].valid && 
            memcmp(arp_pending_requests[i].target_ip, ip, 4) == 0) {
            return 1; // Still pending
        }
    }
    return 0; // Not pending
}

void arp_get_pending_info(int *total_pending) {
    *total_pending = 0;
    
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_pending_requests[i].valid) {
            (*total_pending)++;
            printf("  Pending: %d.%d.%d.%d (retry %d)\n",
                   arp_pending_requests[i].target_ip[0], arp_pending_requests[i].target_ip[1],
                   arp_pending_requests[i].target_ip[2], arp_pending_requests[i].target_ip[3],
                   arp_pending_requests[i].retry_count);
        }
    }
} 