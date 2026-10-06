#include "net_dhcp.h"
#include "net_udp.h"
#include "net_device.h"
#include "net_ethernet.h"
#include "net_thread.h"
#include "debug_config.h"
#include "hpet.h"
#include "string.h"
#include "stdio.h"

// DHCPv4 client (RFC 2131/2132) for inDevOS.
//
// Key goals for this rewrite:
// - Don't rely on the cooperative task scheduler being functional (DHCP pumps RX itself)
// - Correctly parse options with bounds checks
// - Basic retransmit with backoff and sane timeouts
// - Apply IP/mask/gw/dns on ACK

#define DHCP_CLIENT_PORT 68
#define DHCP_SERVER_PORT 67

#define DHCP_MAGIC_COOKIE_0 99
#define DHCP_MAGIC_COOKIE_1 130
#define DHCP_MAGIC_COOKIE_2 83
#define DHCP_MAGIC_COOKIE_3 99

// DHCP message types (Option 53)
#define DHCPDISCOVER 1
#define DHCPOFFER    2
#define DHCPREQUEST  3
#define DHCPDECLINE  4
#define DHCPACK      5
#define DHCPNAK      6
#define DHCPRELEASE  7
#define DHCPINFORM   8

typedef struct __attribute__((packed)) {
    uint8_t  op;
    uint8_t  htype;
    uint8_t  hlen;
    uint8_t  hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint8_t  ciaddr[4];
    uint8_t  yiaddr[4];
    uint8_t  siaddr[4];
    uint8_t  giaddr[4];
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
} dhcp_fixed_t;

typedef struct {
    uint8_t msg_type;
    uint8_t yiaddr[4];
    uint8_t subnet_mask[4];
    uint8_t router[4];
    uint8_t dns[4];
    uint8_t server_id[4];
    uint32_t lease_time_s;
    uint32_t t1_renew_s;
    uint32_t t2_rebind_s;
} dhcp_offer_t;

typedef struct {
    uint32_t xid;
    uint8_t got_offer;
    uint8_t got_ack;
    dhcp_offer_t offer;
} dhcp_client_state_t;

// Lease tracking state
typedef struct {
    uint8_t  active;          // 1 if we hold a valid lease
    uint8_t  server_ip[4];    // DHCP server that granted the lease
    uint8_t  our_ip[4];       // IP address assigned
    uint32_t lease_time_s;    // Full lease duration
    uint32_t t1_s;            // Renewal time (default: lease/2)
    uint32_t t2_s;            // Rebind time (default: 7/8 * lease)
    uint64_t lease_start_ms;  // hpet_get_ticks when lease was acquired
} dhcp_lease_t;

static dhcp_client_state_t g_dhcp = {0};
static dhcp_lease_t g_lease = {0};
static volatile uint32_t g_dhcp_rx_frames = 0;
static volatile uint32_t g_dhcp_rx_ipv4 = 0;
static volatile uint32_t g_dhcp_rx_ipv6 = 0;
static volatile uint32_t g_dhcp_rx_arp  = 0;

static inline uint16_t bswap16(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
static inline uint32_t bswap32(uint32_t v) {
    return ((v & 0x000000FFU) << 24) |
           ((v & 0x0000FF00U) <<  8) |
           ((v & 0x00FF0000U) >>  8) |
           ((v & 0xFF000000U) >> 24);
}

static uint32_t dhcp_next_xid(void) {
    // Simple non-crypto XID generator: mix MAC + time.
    // Good enough to avoid collisions in typical VM usage.
    uint8_t mac[6] = {0};
    net_get_mac_address(mac);
    uint32_t t = (uint32_t)hpet_get_ticks();
    uint32_t x = 0xA5A5A5A5u ^ t;
    x ^= ((uint32_t)mac[0] << 24) | ((uint32_t)mac[1] << 16) | ((uint32_t)mac[2] << 8) | (uint32_t)mac[3];
    x ^= ((uint32_t)mac[4] << 8) | (uint32_t)mac[5];
    // xorshift
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x ? x : 0x12345678u;
}

static void dhcp_pump_rx(int max_packets) {
    // Make sure interrupts are enabled so NIC/HPET interrupts can fire while we're polling.
    __asm__ volatile ("sti");

    // Avoid hammering the NIC receive path when nothing is pending.
    if (!net_has_pending_receive()) return;

    for (int i = 0; i < max_packets; i++) {
        uint8_t buf[2048];
        int len = net_receive_packet(buf, sizeof(buf));
        if (len <= 0) break;

        // Lightweight traffic classification for diagnostics.
        if (len >= 14) {
            uint16_t ethertype = ((uint16_t)buf[12] << 8) | (uint16_t)buf[13];
            g_dhcp_rx_frames++;
            if (ethertype == 0x0800) g_dhcp_rx_ipv4++;
            else if (ethertype == 0x86DD) g_dhcp_rx_ipv6++;
            else if (ethertype == 0x0806) g_dhcp_rx_arp++;
        }

        ethernet_process_frame(buf, (uint32_t)len);
    }
}

static void dhcp_reset_offer(dhcp_offer_t *o) {
    memset(o, 0, sizeof(*o));
}

static void dhcp_parse_options(dhcp_offer_t *out, const uint8_t *opts, uint16_t len) {
    uint16_t i = 0;
    while (i < len) {
        uint8_t code = opts[i++];
        if (code == 0xFF) break;          // END
        if (code == 0x00) continue;       // PAD
        if (i >= len) break;
        uint8_t oplen = opts[i++];
        if ((uint16_t)(i + oplen) > len) break;

        switch (code) {
            case 1: // Subnet Mask
                if (oplen == 4) memcpy(out->subnet_mask, &opts[i], 4);
                break;
            case 3: // Router (take first)
                if (oplen >= 4) memcpy(out->router, &opts[i], 4);
                break;
            case 6: // DNS (take first)
                if (oplen >= 4) memcpy(out->dns, &opts[i], 4);
                break;
            case 51: // Lease Time
                if (oplen == 4) {
                    uint32_t v = ((uint32_t)opts[i] << 24) | ((uint32_t)opts[i+1] << 16) | ((uint32_t)opts[i+2] << 8) | (uint32_t)opts[i+3];
                    out->lease_time_s = v;
                }
                break;
            case 53: // Message Type
                if (oplen >= 1) out->msg_type = opts[i];
                break;
            case 54: // Server Identifier
                if (oplen == 4) memcpy(out->server_id, &opts[i], 4);
                break;
            case 58: // Renewal (T1)
                if (oplen == 4) {
                    uint32_t v = ((uint32_t)opts[i] << 24) | ((uint32_t)opts[i+1] << 16) | ((uint32_t)opts[i+2] << 8) | (uint32_t)opts[i+3];
                    out->t1_renew_s = v;
                }
                break;
            case 59: // Rebinding (T2)
                if (oplen == 4) {
                    uint32_t v = ((uint32_t)opts[i] << 24) | ((uint32_t)opts[i+1] << 16) | ((uint32_t)opts[i+2] << 8) | (uint32_t)opts[i+3];
                    out->t2_rebind_s = v;
                }
                break;
            default:
                break;
        }

        i = (uint16_t)(i + oplen);
    }
}

static int dhcp_decode_offer(dhcp_offer_t *out, uint32_t expect_xid, const uint8_t *payload, uint16_t payload_len) {
    if (payload_len < (uint16_t)(sizeof(dhcp_fixed_t) + 4)) return -1;

    const dhcp_fixed_t *f = (const dhcp_fixed_t*)payload;
    if (f->op != 2) return -1;     // BOOTREPLY
    if (f->htype != 1) return -1;  // Ethernet
    if (f->hlen != 6) return -1;   // MAC length

    uint32_t xid = bswap32(f->xid);
    if (xid != expect_xid) return -1;

    const uint8_t *cookie = payload + sizeof(dhcp_fixed_t);
    if (cookie[0] != DHCP_MAGIC_COOKIE_0 || cookie[1] != DHCP_MAGIC_COOKIE_1 ||
        cookie[2] != DHCP_MAGIC_COOKIE_2 || cookie[3] != DHCP_MAGIC_COOKIE_3) {
        return -1;
    }

    dhcp_reset_offer(out);
    memcpy(out->yiaddr, f->yiaddr, 4);

    const uint8_t *opts = payload + sizeof(dhcp_fixed_t) + 4;
    uint16_t opts_len = (uint16_t)(payload_len - (uint16_t)(sizeof(dhcp_fixed_t) + 4));
    dhcp_parse_options(out, opts, opts_len);

    return (out->msg_type != 0) ? 0 : -1;
}

static void dhcp_udp_handler(const uint8_t *src_ip, uint16_t src_port, const uint8_t *payload, uint16_t payload_len) {
    (void)src_ip;
    (void)src_port;

    dhcp_offer_t offer;
    if (dhcp_decode_offer(&offer, g_dhcp.xid, payload, payload_len) != 0) return;

    // Capture first valid offer and first valid ack.
    if (offer.msg_type == DHCPOFFER && !g_dhcp.got_offer) {
        g_dhcp.offer = offer;
        g_dhcp.got_offer = 1;
    } else if (offer.msg_type == DHCPACK) {
        g_dhcp.offer = offer; // ACK may include fuller config; keep newest
        g_dhcp.got_ack = 1;
    }
}

static int dhcp_build_common(uint8_t *pkt, uint16_t pkt_cap, uint32_t xid) {
    if (pkt_cap < (uint16_t)(sizeof(dhcp_fixed_t) + 4 + 4)) return -1;
    memset(pkt, 0, pkt_cap);

    dhcp_fixed_t *f = (dhcp_fixed_t*)pkt;
    f->op = 1;      // BOOTREQUEST
    f->htype = 1;   // Ethernet
    f->hlen = 6;    // MAC length
    f->hops = 0;
    f->xid = bswap32(xid);
    f->secs = 0;
    f->flags = bswap16(0x8000); // broadcast flag

    uint8_t mac[6] = {0};
    net_get_mac_address(mac);
    memcpy(f->chaddr, mac, 6);

    uint8_t *cookie = pkt + sizeof(dhcp_fixed_t);
    cookie[0] = DHCP_MAGIC_COOKIE_0;
    cookie[1] = DHCP_MAGIC_COOKIE_1;
    cookie[2] = DHCP_MAGIC_COOKIE_2;
    cookie[3] = DHCP_MAGIC_COOKIE_3;

    return 0;
}

static uint16_t dhcp_add_option(uint8_t *pkt, uint16_t idx, uint16_t cap, uint8_t code, const void *data, uint8_t len) {
    if ((uint16_t)(idx + 2 + len) > cap) return idx;
    pkt[idx++] = code;
    pkt[idx++] = len;
    if (len && data) memcpy(&pkt[idx], data, len);
    idx = (uint16_t)(idx + len);
    return idx;
}

static uint16_t dhcp_add_u8_option(uint8_t *pkt, uint16_t idx, uint16_t cap, uint8_t code, uint8_t v) {
    return dhcp_add_option(pkt, idx, cap, code, &v, 1);
}

static int dhcp_send_discover(uint32_t xid) {
    uint8_t pkt[548];
    if (dhcp_build_common(pkt, sizeof(pkt), xid) != 0) return -1;

    uint16_t i = (uint16_t)(sizeof(dhcp_fixed_t) + 4);
    i = dhcp_add_u8_option(pkt, i, sizeof(pkt), 53, DHCPDISCOVER);

    // Client Identifier (Option 61): type=1 (Ethernet) + MAC
    uint8_t client_id[7] = {0};
    client_id[0] = 1;
    net_get_mac_address(&client_id[1]);
    i = dhcp_add_option(pkt, i, sizeof(pkt), 61, client_id, sizeof(client_id));

    // Parameter Request List (Option 55)
    uint8_t prl[] = {1, 3, 6, 51, 54, 58, 59};
    i = dhcp_add_option(pkt, i, sizeof(pkt), 55, prl, sizeof(prl));

    // END
    if (i < sizeof(pkt)) pkt[i++] = 255;

    uint8_t bcast_ip[4] = {255, 255, 255, 255};
    return udp_send(bcast_ip, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, pkt, i);
}

static int dhcp_send_request(uint32_t xid, const dhcp_offer_t *offer) {
    uint8_t pkt[548];
    if (dhcp_build_common(pkt, sizeof(pkt), xid) != 0) return -1;

    uint16_t i = (uint16_t)(sizeof(dhcp_fixed_t) + 4);
    i = dhcp_add_u8_option(pkt, i, sizeof(pkt), 53, DHCPREQUEST);

    // Requested IP Address (Option 50)
    i = dhcp_add_option(pkt, i, sizeof(pkt), 50, offer->yiaddr, 4);
    // Server Identifier (Option 54)
    i = dhcp_add_option(pkt, i, sizeof(pkt), 54, offer->server_id, 4);

    // Parameter Request List (Option 55)
    uint8_t prl[] = {1, 3, 6, 51, 54, 58, 59};
    i = dhcp_add_option(pkt, i, sizeof(pkt), 55, prl, sizeof(prl));

    if (i < sizeof(pkt)) pkt[i++] = 255;

    uint8_t bcast_ip[4] = {255, 255, 255, 255};
    return udp_send(bcast_ip, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, pkt, i);
}

void dhcp_start(void) {
    printf("DHCP: Starting client\n");
    __asm__ volatile ("sti");

    // Store current config to restore if DHCP fails.
    uint8_t old_ip[4], old_mask[4], old_gw[4], old_dns[4];
    net_get_ip_address(old_ip);
    net_get_subnet_mask(old_mask);
    net_get_gateway(old_gw);
    net_get_dns_server(old_dns);

    // Clear address while acquiring.
    uint8_t zero_ip[4] = {0, 0, 0, 0};
    net_set_ip_address(zero_ip);

    // Reset state and install handler.
    memset(&g_dhcp, 0, sizeof(g_dhcp));
    g_dhcp_rx_frames = g_dhcp_rx_ipv4 = g_dhcp_rx_ipv6 = g_dhcp_rx_arp = 0;
    g_dhcp.xid = dhcp_next_xid();
    dhcp_reset_offer(&g_dhcp.offer);

    if (udp_register_handler(DHCP_CLIENT_PORT, dhcp_udp_handler) != 0) {
        printf("DHCP: Failed to register UDP handler on port %u\n", DHCP_CLIENT_PORT);
        return;
    }

    if (dhcp_send_discover(g_dhcp.xid) != 0) {
        printf("DHCP: Failed to send DISCOVER\n");
        udp_unregister_handler(DHCP_CLIENT_PORT);
        net_set_ip_address(old_ip);
        return;
    }

    uint32_t start_ms = (uint32_t)hpet_get_ticks();
    printf("DHCP: Waiting for OFFER...\n");

    // Wait up to 10 seconds for an OFFER, retransmitting DISCOVER.
    if (!g_dhcp.got_offer) {
        uint32_t deadline = start_ms + 10000;
        uint32_t tx_delay = 1000;
        uint32_t tx_at = start_ms + tx_delay;
        uint32_t last_dbg = start_ms;
        while ((uint32_t)hpet_get_ticks() < deadline && !g_dhcp.got_offer) {
            dhcp_pump_rx(8);
            uint32_t now = (uint32_t)hpet_get_ticks();
            if (DEBUG_NET_DHCP && now - last_dbg >= 1000) {
                extern void rtl8139_print_debug(void);
                rtl8139_print_debug();
                printf("DHCP: waiting OFFER... t=%u pending=%d\n", now, net_has_pending_receive());
                last_dbg = now;
            }
            if (now >= tx_at) {
                (void)dhcp_send_discover(g_dhcp.xid);
                if (tx_delay < 8000) tx_delay *= 2;
                tx_at = now + tx_delay;
            }
            if (!g_dhcp.got_offer) net_wait_idle();
        }
    }

    if (!g_dhcp.got_offer) {
        printf("DHCP: No OFFER received (timeout)\n");
        printf("DHCP: RX during wait: frames=%u ipv4=%u ipv6=%u arp=%u\n",
               (unsigned)g_dhcp_rx_frames, (unsigned)g_dhcp_rx_ipv4,
               (unsigned)g_dhcp_rx_ipv6, (unsigned)g_dhcp_rx_arp);
        printf("DHCP: Hint: this usually means there is NO DHCPv4 server on the network you're attached to,\n");
        printf("      or DHCP broadcasts are blocked. If you're using QEMU user networking, use `make run`.\n");
        printf("      If you're using TAP/bridge, run a DHCP server (e.g. dnsmasq) on that LAN/tap.\n");
        udp_unregister_handler(DHCP_CLIENT_PORT);
        net_set_ip_address(old_ip);
        net_set_subnet_mask(old_mask);
        net_set_gateway(old_gw);
        net_set_dns_server(old_dns);
        return;
    }

    printf("DHCP: OFFER %d.%d.%d.%d from server %d.%d.%d.%d\n",
           g_dhcp.offer.yiaddr[0], g_dhcp.offer.yiaddr[1], g_dhcp.offer.yiaddr[2], g_dhcp.offer.yiaddr[3],
           g_dhcp.offer.server_id[0], g_dhcp.offer.server_id[1], g_dhcp.offer.server_id[2], g_dhcp.offer.server_id[3]);

    if (dhcp_send_request(g_dhcp.xid, &g_dhcp.offer) != 0) {
        printf("DHCP: Failed to send REQUEST\n");
        udp_unregister_handler(DHCP_CLIENT_PORT);
        net_set_ip_address(old_ip);
        return;
    }

    printf("DHCP: Waiting for ACK...\n");
    uint32_t ack_start = (uint32_t)hpet_get_ticks();
    uint32_t ack_deadline = ack_start + 10000;
    uint32_t tx_delay = 1000;
    uint32_t tx_at = ack_start + tx_delay;
    uint32_t last_dbg = ack_start;
    while ((uint32_t)hpet_get_ticks() < ack_deadline && !g_dhcp.got_ack) {
        dhcp_pump_rx(8);
        uint32_t now = (uint32_t)hpet_get_ticks();
        if (DEBUG_NET_DHCP && now - last_dbg >= 1000) {
            extern void rtl8139_print_debug(void);
            rtl8139_print_debug();
            printf("DHCP: waiting ACK... t=%u pending=%d\n", now, net_has_pending_receive());
            last_dbg = now;
        }
        if (now >= tx_at) {
            (void)dhcp_send_request(g_dhcp.xid, &g_dhcp.offer);
            if (tx_delay < 8000) tx_delay *= 2;
            tx_at = now + tx_delay;
        }
        if (!g_dhcp.got_ack) net_wait_idle();
    }

    udp_unregister_handler(DHCP_CLIENT_PORT);

    if (!g_dhcp.got_ack) {
        printf("DHCP: No ACK received (timeout)\n");
        net_set_ip_address(old_ip);
        net_set_subnet_mask(old_mask);
        net_set_gateway(old_gw);
        net_set_dns_server(old_dns);
        return;
    }

    // Apply configuration from ACK.
    net_set_ip_address(g_dhcp.offer.yiaddr);
    net_set_subnet_mask(g_dhcp.offer.subnet_mask);
    net_set_gateway(g_dhcp.offer.router);
    net_set_dns_server(g_dhcp.offer.dns);

    printf("DHCP: Configured IP %d.%d.%d.%d mask %d.%d.%d.%d gw %d.%d.%d.%d dns %d.%d.%d.%d\n",
           g_dhcp.offer.yiaddr[0], g_dhcp.offer.yiaddr[1], g_dhcp.offer.yiaddr[2], g_dhcp.offer.yiaddr[3],
           g_dhcp.offer.subnet_mask[0], g_dhcp.offer.subnet_mask[1], g_dhcp.offer.subnet_mask[2], g_dhcp.offer.subnet_mask[3],
           g_dhcp.offer.router[0], g_dhcp.offer.router[1], g_dhcp.offer.router[2], g_dhcp.offer.router[3],
           g_dhcp.offer.dns[0], g_dhcp.offer.dns[1], g_dhcp.offer.dns[2], g_dhcp.offer.dns[3]);

    // Store lease information for renewal
    g_lease.active = 1;
    memcpy(g_lease.server_ip, g_dhcp.offer.server_id, 4);
    memcpy(g_lease.our_ip, g_dhcp.offer.yiaddr, 4);
    g_lease.lease_time_s = g_dhcp.offer.lease_time_s;
    g_lease.lease_start_ms = (uint64_t)hpet_get_ticks();

    // Set T1 and T2 (RFC 2131: T1 = lease/2, T2 = 7/8 * lease)
    if (g_dhcp.offer.t1_renew_s > 0) {
        g_lease.t1_s = g_dhcp.offer.t1_renew_s;
    } else {
        g_lease.t1_s = g_lease.lease_time_s / 2;
    }
    if (g_dhcp.offer.t2_rebind_s > 0) {
        g_lease.t2_s = g_dhcp.offer.t2_rebind_s;
    } else {
        g_lease.t2_s = (g_lease.lease_time_s * 7) / 8;
    }

    printf("DHCP: Lease time %u s, T1 (renew) %u s, T2 (rebind) %u s\n",
           g_lease.lease_time_s, g_lease.t1_s, g_lease.t2_s);
}

// ---- DHCP Renewal -----------------------------------------------------------

/**
 * Send a DHCP REQUEST for renewal (unicast to server) or rebinding (broadcast).
 * ciaddr is set to our current IP (per RFC 2131 section 4.3.2).
 */
static int dhcp_send_renew_request(uint32_t xid, int broadcast) {
    uint8_t pkt[548];
    if (dhcp_build_common(pkt, sizeof(pkt), xid) != 0) return -1;

    dhcp_fixed_t *f = (dhcp_fixed_t*)pkt;
    // For RENEWING/REBINDING, ciaddr = our current IP, no broadcast flag
    memcpy(f->ciaddr, g_lease.our_ip, 4);
    if (!broadcast) {
        f->flags = 0;  // Unicast – clear broadcast flag
    }

    uint16_t i = (uint16_t)(sizeof(dhcp_fixed_t) + 4);
    i = dhcp_add_u8_option(pkt, i, sizeof(pkt), 53, DHCPREQUEST);

    // Parameter Request List
    uint8_t prl[] = {1, 3, 6, 51, 54, 58, 59};
    i = dhcp_add_option(pkt, i, sizeof(pkt), 55, prl, sizeof(prl));

    if (i < sizeof(pkt)) pkt[i++] = 255;  // END

    if (broadcast) {
        uint8_t bcast_ip[4] = {255, 255, 255, 255};
        return udp_send(bcast_ip, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, pkt, i);
    } else {
        return udp_send(g_lease.server_ip, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, pkt, i);
    }
}

uint32_t dhcp_get_lease_remaining_s(void) {
    if (!g_lease.active || g_lease.lease_time_s == 0) return 0;
    uint64_t now = (uint64_t)hpet_get_ticks();
    uint64_t elapsed_ms = now - g_lease.lease_start_ms;
    uint64_t elapsed_s = elapsed_ms / 1000;
    if (elapsed_s >= g_lease.lease_time_s) return 0;
    return (uint32_t)(g_lease.lease_time_s - elapsed_s);
}

int dhcp_has_lease(void) {
    return g_lease.active && dhcp_get_lease_remaining_s() > 0;
}

int dhcp_renew_check(void) {
    if (!g_lease.active) return 0;  // No lease to renew
    if (g_lease.lease_time_s == 0) return 0;  // Infinite lease

    uint64_t now = (uint64_t)hpet_get_ticks();
    uint64_t elapsed_ms = now - g_lease.lease_start_ms;
    uint64_t elapsed_s = elapsed_ms / 1000;

    if (elapsed_s < g_lease.t1_s) {
        return 0;  // Not time to renew yet
    }

    // Determine whether to renew (unicast) or rebind (broadcast)
    int broadcast = (elapsed_s >= g_lease.t2_s);

    if (elapsed_s >= g_lease.lease_time_s) {
        // Lease expired – fall back to full re-acquire
        printf("DHCP: Lease expired, performing full re-acquire\n");
        g_lease.active = 0;
        dhcp_start();
        return g_lease.active ? 1 : -1;
    }

    printf("DHCP: %s at T+%llu s (T1=%u, T2=%u, lease=%u)\n",
           broadcast ? "Rebinding" : "Renewing",
           (unsigned long long)elapsed_s, g_lease.t1_s, g_lease.t2_s, g_lease.lease_time_s);

    // Prepare for renewal response
    memset(&g_dhcp, 0, sizeof(g_dhcp));
    g_dhcp.xid = dhcp_next_xid();
    dhcp_reset_offer(&g_dhcp.offer);

    if (udp_register_handler(DHCP_CLIENT_PORT, dhcp_udp_handler) != 0) {
        printf("DHCP: Failed to register UDP handler for renewal\n");
        return 0;
    }

    // Send renewal/rebind request
    if (dhcp_send_renew_request(g_dhcp.xid, broadcast) != 0) {
        printf("DHCP: Failed to send renewal REQUEST\n");
        udp_unregister_handler(DHCP_CLIENT_PORT);
        return 0;
    }

    // Wait up to 5 seconds for ACK
    uint32_t start = (uint32_t)hpet_get_ticks();
    uint32_t deadline = start + 5000;
    while ((uint32_t)hpet_get_ticks() < deadline && !g_dhcp.got_ack) {
        dhcp_pump_rx(8);
        if (!g_dhcp.got_ack) net_wait_idle();
    }

    udp_unregister_handler(DHCP_CLIENT_PORT);

    if (!g_dhcp.got_ack) {
        printf("DHCP: Renewal ACK not received\n");
        // If we're past T2 and still no response, mark lease as failing
        if (broadcast) {
            printf("DHCP: Rebind failed, lease may be lost soon\n");
        }
        return 0;
    }

    // Apply renewed configuration
    net_set_ip_address(g_dhcp.offer.yiaddr);
    net_set_subnet_mask(g_dhcp.offer.subnet_mask);
    net_set_gateway(g_dhcp.offer.router);
    net_set_dns_server(g_dhcp.offer.dns);

    // Update lease info
    memcpy(g_lease.server_ip, g_dhcp.offer.server_id, 4);
    memcpy(g_lease.our_ip, g_dhcp.offer.yiaddr, 4);
    g_lease.lease_time_s = g_dhcp.offer.lease_time_s;
    g_lease.lease_start_ms = (uint64_t)hpet_get_ticks();

    if (g_dhcp.offer.t1_renew_s > 0) g_lease.t1_s = g_dhcp.offer.t1_renew_s;
    else g_lease.t1_s = g_lease.lease_time_s / 2;
    if (g_dhcp.offer.t2_rebind_s > 0) g_lease.t2_s = g_dhcp.offer.t2_rebind_s;
    else g_lease.t2_s = (g_lease.lease_time_s * 7) / 8;

    printf("DHCP: Lease renewed, new lease %u s\n", g_lease.lease_time_s);
    return 1;
}


















