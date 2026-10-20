#include "net_ethernet.h"
#include "net_arp.h"
#include "net_ip.h"
#include "serial.h"
#include "hpet.h"
#include "string.h"

static volatile int g_eth_verbose = 0;

/* Drop counters: frames the stack ignores on purpose (LLDP, RRCP, IGMP...). */
static volatile uint32_t g_drop_unknown_l2, g_drop_igmp, g_drop_unknown_ip, g_drop_malformed;
static volatile uint32_t g_rx_log_suppressed;
static volatile uint16_t g_last_ethertype;
static volatile uint8_t  g_last_ip_proto;

void net_rx_count_unknown_l2(uint16_t ethertype) { g_drop_unknown_l2++; g_last_ethertype = ethertype; }
void net_rx_count_ip_proto(uint8_t proto) {
    if (proto == 2) g_drop_igmp++;
    else { g_drop_unknown_ip++; g_last_ip_proto = proto; }
}
void net_rx_count_malformed(void) { g_drop_malformed++; }

int net_rx_log_allow(void) {
    if (g_eth_verbose) return 2;
    static uint64_t last_ms = 0;
    static int primed = 0;
    uint64_t now = hpet_get_ticks();
    if (!primed || now - last_ms >= 1000) { primed = 1; last_ms = now; return 1; }
    g_rx_log_suppressed++;
    return 0;
}

void net_rx_print_drop_stats(const char *indent) {
    printf("%sdropped: igmp %u, unknown-ip %u", indent ? indent : "",
           (unsigned)g_drop_igmp, (unsigned)g_drop_unknown_ip);
    if (g_drop_unknown_ip) printf(" (last proto %u)", (unsigned)g_last_ip_proto);
    printf(", unknown-l2 %u", (unsigned)g_drop_unknown_l2);
    if (g_drop_unknown_l2) printf(" (last ethertype 0x%04x)", (unsigned)g_last_ethertype);
    printf(", malformed %u, rate-limited log lines %u\n",
           (unsigned)g_drop_malformed, (unsigned)g_rx_log_suppressed);
}

void ethernet_set_verbose(int enabled) {
    g_eth_verbose = enabled ? 1 : 0;
}

int ethernet_get_verbose(void) {
    return g_eth_verbose ? 1 : 0;
}

// Process an ethernet frame - centralized entry point for the network stack
void ethernet_process_frame(const uint8_t *frame, uint32_t length) {
    if (length < 14) { // Minimum Ethernet frame size
        if (g_eth_verbose) {
            printf("Ethernet: Frame too short (%d bytes)\n", length);
        }
        return;
    }
    
    // Extract ethernet header fields
    uint8_t dst_mac[6], src_mac[6];
    uint16_t ethertype;
    
    memcpy(dst_mac, frame, 6);
    memcpy(src_mac, frame + 6, 6);
    ethertype = (frame[12] << 8) | frame[13];

    if (g_eth_verbose) {
        printf("Ethernet: Frame from %02x:%02x:%02x:%02x:%02x:%02x to %02x:%02x:%02x:%02x:%02x:%02x, type 0x%04x, length %d\n",
               src_mac[0], src_mac[1], src_mac[2], src_mac[3], src_mac[4], src_mac[5],
               dst_mac[0], dst_mac[1], dst_mac[2], dst_mac[3], dst_mac[4], dst_mac[5],
               ethertype, length);
    }
    
    // Dispatch to appropriate protocol handler
    switch (ethertype) {
        case 0x0806: // ARP
            if (g_eth_verbose) {
                printf("Ethernet: Processing ARP packet (%d bytes)\n", length);
            }
            arp_handle_packet(frame, length);
            break;
            
        case 0x0800: // IPv4
            if (g_eth_verbose) {
                printf("Ethernet: Processing IPv4 packet (%d bytes)\n", length);
            }
            ip_handle_packet(frame, length);
            break;
            
        case 0x86dd: { // IPv6
            extern void ipv6_handle_packet(const uint8_t *frame, uint32_t len);
            extern bool ipv6_is_enabled(void);
            if (ipv6_is_enabled()) {
                ipv6_handle_packet(frame + 14, length - 14);
            } else if (g_eth_verbose) {
                printf("Ethernet: IPv6 packet received (%d bytes) - IPv6 not enabled\n", length);
            }
            break;
        }
            
        default:
            /* LLDP 0x88cc, Realtek RRCP 0x8899, 0x8ffd, ...: count, never print
             * unless 'netverbose on'. */
            net_rx_count_unknown_l2(ethertype);
            if (g_eth_verbose) {
                printf("Ethernet: Unknown ethertype 0x%04x (%d bytes)\n", ethertype, length);
            }
            break;
    }
}