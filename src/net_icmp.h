#pragma once
#include <stdint.h>

// ICMP header structure
struct icmp_header {
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
    uint16_t identifier;
    uint16_t sequence;
    // Data follows
} __attribute__((packed));

void icmp_init();
void icmp_handle_packet(const uint8_t *frame, uint32_t len);
int icmp_send_echo_request(const uint8_t *dst_ip, uint16_t id, uint16_t seq, const uint8_t *data, uint16_t data_len);
int icmp_send_echo_reply(const uint8_t *original_packet, uint32_t len);
int icmp_send_echo_reply_to(const uint8_t *dst_ip, const uint8_t *original_packet, uint32_t len);
int icmp_ping(const uint8_t *dst_ip, const uint8_t *data, uint16_t data_len); 

// Simple ping tracking helpers
void icmp_set_echo_expect(uint16_t id, uint16_t seq);
int icmp_check_echo_reply(void); // returns 1 if expected reply received (and clears the flag)