#include "net_ethernet.h"
#include "net_icmp.h"
#include "string.h"
#include "stdio.h"
#include "net_ip.h"
#include "mem.h"
#include "debug_config.h"

// ICMP types
#define ICMP_TYPE_ECHO_REQUEST 8
#define ICMP_TYPE_ECHO_REPLY   0
#define ICMP_TYPE_DEST_UNREACH 3
#define ICMP_TYPE_TIME_EXCEEDED 11

// ICMP codes
#define ICMP_CODE_NET_UNREACH 0
#define ICMP_CODE_HOST_UNREACH 1
#define ICMP_CODE_PROTOCOL_UNREACH 2
#define ICMP_CODE_PORT_UNREACH 3

static uint16_t icmp_id_counter = 0;
static volatile uint16_t expect_id = 0xFFFF;
static volatile uint16_t expect_seq = 0xFFFF;
static volatile int expect_hit = 0;

void icmp_init() {
    icmp_id_counter = 0;
    printf("ICMP: Initialized\n");
}

// Calculate ICMP checksum
static uint16_t icmp_checksum(const uint8_t *data, uint16_t len) {
    uint32_t sum = 0;
    uint16_t *ptr = (uint16_t*)data;
    
    // Sum all 16-bit words
    for (int i = 0; i < len / 2; i++) {
        sum += ptr[i];
    }
    
    // Handle odd byte
    if (len % 2) {
        sum += data[len - 1];
    }
    
    // Add carry
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    
    return ~sum;
}

void icmp_handle_packet(const uint8_t *data, uint32_t len) {
    if (len < 8) { // Minimum ICMP header size
        NET_RX_LOG("ICMP: Packet too short (%d bytes)\n", len);
        return;
    }
    
    struct icmp_header *icmp = (struct icmp_header*)data;
    
    DEBUG_PRINT(NET_ICMP, "ICMP: Received type %d, code %d, id %d, seq %d\n",
                icmp->type, icmp->code,
                (icmp->identifier >> 8) | ((icmp->identifier & 0xFF) << 8),
                (icmp->sequence >> 8) | ((icmp->sequence & 0xFF) << 8));
    
    switch (icmp->type) {
        // Echo requests are handled directly by the IP layer to preserve source IP
        case ICMP_TYPE_ECHO_REQUEST:
            DEBUG_PRINT(NET_ICMP, "ICMP: Echo request received\n");
            // Extract source IP from the packet to reply to
            if (len >= 8) {
                // The IP layer doesn't pass the source IP to us in the current architecture,
                // it handles packet dispatch. We need to modify the architecture to support replies
                // or rely on the IP layer to handle this specific case.
                
                // Note: Current IP layer implementation calls icmp_send_echo_reply_to directly
                // when it sees an ECHO_REQUEST, so we shouldn't get here for those unless
                // the IP layer failed to handle it.
                DEBUG_PRINT(NET_ICMP, "ICMP: Echo request should have been handled by IP layer\n");
            }
            break;
            
        case ICMP_TYPE_ECHO_REPLY:
            DEBUG_PRINT(NET_ICMP, "ICMP: Echo reply received\n");
            if ((((icmp->identifier >> 8) | ((icmp->identifier & 0xFF) << 8)) == expect_id) &&
                (((icmp->sequence >> 8) | ((icmp->sequence & 0xFF) << 8)) == expect_seq)) {
                expect_hit = 1;
            }
            break;
            
        case ICMP_TYPE_DEST_UNREACH:
            NET_RX_LOG("ICMP: Destination unreachable (code %d)\n", icmp->code);
            break;
            
        case ICMP_TYPE_TIME_EXCEEDED:
            NET_RX_LOG("ICMP: Time exceeded\n");
            break;
            
        default:
            NET_RX_LOG("ICMP: Unknown type %d\n", icmp->type);
            break;
    }
}

int icmp_send_echo_request(const uint8_t *dst_ip, uint16_t id, uint16_t seq, const uint8_t *data, uint16_t data_len) {
    // Build ICMP echo request
    uint16_t total_len = 8 + data_len; // ICMP header + data
    uint8_t *icmp_packet = kalloc(total_len);
    if (!icmp_packet) {
        printf("ICMP: Failed to allocate packet buffer\n");
        return -1;
    }
    
    struct icmp_header *icmp = (struct icmp_header*)icmp_packet;
    icmp->type = ICMP_TYPE_ECHO_REQUEST;
    icmp->code = 0;
    icmp->checksum = 0;
    icmp->identifier = ((id & 0xFF) << 8) | ((id >> 8) & 0xFF);
    icmp->sequence = ((seq & 0xFF) << 8) | ((seq >> 8) & 0xFF);
    
    // Copy data
    if (data && data_len > 0) {
        memcpy(&icmp_packet[8], data, data_len);
    }
    
    // Calculate checksum
    icmp->checksum = icmp_checksum(icmp_packet, total_len);
    
    // Send via IP layer
    int result = ip_send_packet(dst_ip, 1, icmp_packet, total_len); // Protocol 1 = ICMP
    kfree(icmp_packet);
    
    if (result == 0) {
        DEBUG_PRINT(NET_ICMP, "ICMP: Sent echo request to %d.%d.%d.%d (id %d, seq %d)\n",
                    dst_ip[0], dst_ip[1], dst_ip[2], dst_ip[3], id, seq);
    } else {
        printf("ICMP: Failed to send echo request\n");
    }
    
    return result;
}

int icmp_send_echo_reply_to(const uint8_t *dst_ip, const uint8_t *original_packet, uint32_t len) {
    DEBUG_PRINT(NET_ICMP, "ICMP: send_echo_reply_to called for %d.%d.%d.%d, len=%d\n",
           dst_ip[0], dst_ip[1], dst_ip[2], dst_ip[3], len);
    
    if (len < 8) {
        printf("ICMP: Cannot send reply - packet too short\n");
        return -1;
    }
    
    // Create reply packet
    uint8_t *reply_packet = kalloc(len);
    if (!reply_packet) {
        printf("ICMP: Failed to allocate reply buffer\n");
        return -1;
    }
    
    // Copy original packet
    memcpy(reply_packet, original_packet, len);
    
    // Modify header for reply
    struct icmp_header *icmp = (struct icmp_header*)reply_packet;
    DEBUG_PRINT(NET_ICMP, "ICMP: Original type=%d, changing to type=%d\n", icmp->type, ICMP_TYPE_ECHO_REPLY);
    icmp->type = ICMP_TYPE_ECHO_REPLY;
    icmp->checksum = 0; // Will be recalculated
    
    // Calculate new checksum
    icmp->checksum = icmp_checksum(reply_packet, len);
    DEBUG_PRINT(NET_ICMP, "ICMP: Calculated checksum=0x%04x\n", icmp->checksum);
    
    // Send via IP layer
    DEBUG_PRINT(NET_ICMP, "ICMP: Calling ip_send_packet for echo reply\n");
    int result = ip_send_packet(dst_ip, 1, reply_packet, len); // Protocol 1 = ICMP
    kfree(reply_packet);
    
    if (result == 0) {
        DEBUG_PRINT(NET_ICMP, "ICMP: Successfully sent echo reply to %d.%d.%d.%d\n",
               dst_ip[0], dst_ip[1], dst_ip[2], dst_ip[3]);
    } else {
        printf("ICMP: Failed to send echo reply (result=%d)\n", result);
    }
    
    return result;
} 

// Helper function to send echo request with auto-generated ID and sequence
int icmp_ping(const uint8_t *dst_ip, const uint8_t *data, uint16_t data_len) {
    uint16_t id = icmp_id_counter++;
    uint16_t seq = 1; // For simple ping, always use sequence 1
    
    icmp_set_echo_expect(id, seq);
    return icmp_send_echo_request(dst_ip, id, seq, data, data_len);
} 

void icmp_set_echo_expect(uint16_t id, uint16_t seq) {
    expect_id = id;
    expect_seq = seq;
    expect_hit = 0;
}

int icmp_check_echo_reply(void) {
    if (expect_hit) {
        expect_hit = 0;
        return 1;
    }
    return 0;
}