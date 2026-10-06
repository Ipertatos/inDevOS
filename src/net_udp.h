#pragma once
#include <stdint.h>

typedef struct {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} __attribute__((packed)) udp_header_t;

void udp_init(void);
int udp_send(const uint8_t *dst_ip, uint16_t src_port, uint16_t dst_port, const uint8_t *payload, uint16_t len);
void udp_handle_packet(const uint8_t *frame, uint32_t len);

typedef void (*udp_handler_t)(const uint8_t *src_ip, uint16_t src_port, const uint8_t *payload, uint16_t payload_len);
int udp_register_handler(uint16_t dst_port, udp_handler_t handler);
void udp_unregister_handler(uint16_t dst_port);

// Control verbose UDP logging (0=quiet, 1=verbose).
void udp_set_verbose(int enabled);
int udp_get_verbose(void);

/* Userspace SOCK_DGRAM sockets. transport in kernel_socket_t is udp_pcb_t*. */
typedef struct udp_pcb udp_pcb_t;
udp_pcb_t *udp_pcb_create(void);
int udp_pcb_bind(udp_pcb_t *s, uint16_t port); /* 0 = ephemeral */
int udp_pcb_connect(udp_pcb_t *s, const uint8_t ip[4], uint16_t port);
int udp_pcb_sendto(udp_pcb_t *s, const uint8_t *data, uint16_t len,
                   const uint8_t ip[4], uint16_t port);
int udp_pcb_recvfrom(udp_pcb_t *s, uint8_t *buf, uint16_t len,
                     uint8_t src_ip[4], uint16_t *src_port);
int udp_pcb_rx_ready(udp_pcb_t *s);
uint16_t udp_pcb_local_port(udp_pcb_t *s);
void udp_pcb_close(udp_pcb_t *s);


