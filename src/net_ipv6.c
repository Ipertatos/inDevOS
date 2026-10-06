/**
 * @file net_ipv6.c
 * @brief IPv6 protocol implementation (basic)
 *
 * Implements basic IPv6 packet handling, ICMPv6 (Echo, Neighbor Discovery),
 * and SLAAC (Stateless Address Autoconfiguration) with EUI-64.
 */

#include "net_ipv6.h"
#include "net_device.h"
#include "net_ethernet.h"
#include "serial.h"
#include "term.h"
#include "mem.h"
#include "hpet.h"
#include "string.h"

extern int snprintf(char *str, size_t size, const char *fmt, ...);

/* Byte order helpers */
static inline uint16_t htons16(uint16_t v) { return (v << 8) | (v >> 8); }
static inline uint16_t ntohs16(uint16_t v) { return (v << 8) | (v >> 8); }
static inline uint32_t htonl32(uint32_t v) {
    return (v << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00) | (v >> 24);
}

/* Well-known addresses */
const ipv6_addr_t IPV6_ADDR_UNSPECIFIED = {{ 0 }};
const ipv6_addr_t IPV6_ADDR_LOOPBACK = {{ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1 }};
const ipv6_addr_t IPV6_ADDR_ALL_NODES = {{ 0xff,0x02,0,0,0,0,0,0,0,0,0,0,0,0,0,0x01 }};
const ipv6_addr_t IPV6_ADDR_ALL_ROUTERS = {{ 0xff,0x02,0,0,0,0,0,0,0,0,0,0,0,0,0,0x02 }};

/* Global IPv6 interface state */
static ipv6_interface_t ipv6_iface = {0};

/* ICMPv6 echo sequence number */
static uint16_t icmp6_seq = 0;

/* ============================================================================
 * Address Utilities
 * ============================================================================ */

bool ipv6_addr_is_unspecified(const ipv6_addr_t *addr) {
    for (int i = 0; i < 16; i++) if (addr->addr[i] != 0) return false;
    return true;
}

bool ipv6_addr_is_loopback(const ipv6_addr_t *addr) {
    return ipv6_addr_equal(addr, &IPV6_ADDR_LOOPBACK);
}

bool ipv6_addr_is_multicast(const ipv6_addr_t *addr) {
    return addr->addr[0] == 0xFF;
}

bool ipv6_addr_is_link_local(const ipv6_addr_t *addr) {
    return addr->addr[0] == 0xFE && (addr->addr[1] & 0xC0) == 0x80;
}

bool ipv6_addr_equal(const ipv6_addr_t *a, const ipv6_addr_t *b) {
    return memcmp(a->addr, b->addr, 16) == 0;
}

void ipv6_addr_to_string(const ipv6_addr_t *addr, char *buf, size_t buflen) {
    if (!addr || !buf || buflen < 40) {
        if (buf && buflen > 0) buf[0] = '\0';
        return;
    }
    /* Simple format: full form with colons, no zero compression */
    snprintf(buf, buflen, "%x:%x:%x:%x:%x:%x:%x:%x",
             (addr->addr[0] << 8) | addr->addr[1],
             (addr->addr[2] << 8) | addr->addr[3],
             (addr->addr[4] << 8) | addr->addr[5],
             (addr->addr[6] << 8) | addr->addr[7],
             (addr->addr[8] << 8) | addr->addr[9],
             (addr->addr[10] << 8) | addr->addr[11],
             (addr->addr[12] << 8) | addr->addr[13],
             (addr->addr[14] << 8) | addr->addr[15]);
}

int ipv6_addr_from_string(const char *str, ipv6_addr_t *addr) {
    if (!str || !addr) return -1;
    memset(addr, 0, sizeof(ipv6_addr_t));

    /* Simple parser: supports full form xxxx:xxxx:...:xxxx and :: for zero groups */
    uint16_t groups[8] = {0};
    int group_count = 0;
    int double_colon_pos = -1;
    uint16_t val = 0;
    bool has_val = false;

    for (const char *p = str; ; p++) {
        if (*p >= '0' && *p <= '9') {
            val = (val << 4) | (*p - '0');
            has_val = true;
        } else if (*p >= 'a' && *p <= 'f') {
            val = (val << 4) | (*p - 'a' + 10);
            has_val = true;
        } else if (*p >= 'A' && *p <= 'F') {
            val = (val << 4) | (*p - 'A' + 10);
            has_val = true;
        } else if (*p == ':' || *p == '\0') {
            if (has_val && group_count < 8) {
                groups[group_count++] = val;
                val = 0;
                has_val = false;
            }
            if (*p == ':' && *(p + 1) == ':') {
                double_colon_pos = group_count;
                p++;  /* Skip second colon */
            }
            if (*p == '\0') break;
        } else {
            return -1;
        }
    }

    /* Expand :: if present */
    if (double_colon_pos >= 0) {
        int zeros_needed = 8 - group_count;
        if (zeros_needed < 0) return -1;
        /* Shift groups after :: to the end */
        for (int i = group_count - 1; i >= double_colon_pos; i--) {
            groups[i + zeros_needed] = groups[i];
        }
        /* Fill zeros */
        for (int i = 0; i < zeros_needed; i++) {
            groups[double_colon_pos + i] = 0;
        }
    }

    /* Convert to byte form */
    for (int i = 0; i < 8; i++) {
        addr->addr[i * 2]     = (uint8_t)(groups[i] >> 8);
        addr->addr[i * 2 + 1] = (uint8_t)(groups[i] & 0xFF);
    }

    return 0;
}

void ipv6_solicited_node_addr(const ipv6_addr_t *unicast, ipv6_addr_t *solicited) {
    /* ff02::1:ffXX:XXXX where XX:XXXX is the last 24 bits of the unicast address */
    memset(solicited, 0, sizeof(ipv6_addr_t));
    solicited->addr[0]  = 0xFF;
    solicited->addr[1]  = 0x02;
    solicited->addr[11] = 0x01;
    solicited->addr[12] = 0xFF;
    solicited->addr[13] = unicast->addr[13];
    solicited->addr[14] = unicast->addr[14];
    solicited->addr[15] = unicast->addr[15];
}

void ipv6_eui64_from_mac(const uint8_t *mac, uint8_t *iid) {
    /* EUI-64: insert FF:FE in the middle, flip the U/L bit */
    iid[0] = mac[0] ^ 0x02;  /* Flip universal/local bit */
    iid[1] = mac[1];
    iid[2] = mac[2];
    iid[3] = 0xFF;
    iid[4] = 0xFE;
    iid[5] = mac[3];
    iid[6] = mac[4];
    iid[7] = mac[5];
}

/* ============================================================================
 * ICMPv6 Checksum
 * ============================================================================ */

static uint16_t ipv6_icmp_checksum(const ipv6_addr_t *src, const ipv6_addr_t *dst,
                                    const uint8_t *payload, uint16_t payload_len) {
    uint32_t sum = 0;

    /* Pseudo-header: src + dst + payload length + next header (58 = ICMPv6) */
    for (int i = 0; i < 16; i += 2)
        sum += (src->addr[i] << 8) | src->addr[i + 1];
    for (int i = 0; i < 16; i += 2)
        sum += (dst->addr[i] << 8) | dst->addr[i + 1];
    sum += payload_len;
    sum += 58;  /* ICMPv6 next header */

    /* Payload */
    for (uint16_t i = 0; i + 1 < payload_len; i += 2)
        sum += (payload[i] << 8) | payload[i + 1];
    if (payload_len & 1)
        sum += payload[payload_len - 1] << 8;

    /* Fold carry */
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);

    return ~((uint16_t)sum);
}

/* ============================================================================
 * Neighbor Cache
 * ============================================================================ */

static ndp_neighbor_entry_t* ndp_cache_find(const ipv6_addr_t *ip) {
    for (int i = 0; i < IPV6_NEIGHBOR_CACHE_SIZE; i++) {
        if (ipv6_iface.neighbor_cache[i].state != NDP_STATE_INCOMPLETE &&
            ipv6_addr_equal(&ipv6_iface.neighbor_cache[i].ip, ip)) {
            return &ipv6_iface.neighbor_cache[i];
        }
    }
    return NULL;
}

static void ndp_cache_add(const ipv6_addr_t *ip, const uint8_t *mac) {
    /* Find existing or free slot */
    ndp_neighbor_entry_t *entry = ndp_cache_find(ip);
    if (!entry) {
        for (int i = 0; i < IPV6_NEIGHBOR_CACHE_SIZE; i++) {
            if (ipv6_iface.neighbor_cache[i].state == NDP_STATE_INCOMPLETE) {
                entry = &ipv6_iface.neighbor_cache[i];
                break;
            }
        }
    }
    if (!entry) {
        /* Evict oldest STALE entry */
        entry = &ipv6_iface.neighbor_cache[0];
    }

    entry->ip = *ip;
    memcpy(entry->mac, mac, 6);
    entry->state = NDP_STATE_REACHABLE;
    entry->timestamp = (uint32_t)hpet_get_ticks();
}

/* ============================================================================
 * ICMPv6 Handling
 * ============================================================================ */

static void icmpv6_handle_echo_request(const ipv6_header_t *ip6,
                                        const uint8_t *payload, uint16_t payload_len) {
    if (payload_len < 8) return;  /* ICMPv6 header + id + seq */

    serial_puts("ipv6: received ICMPv6 Echo Request, sending reply\n");

    /* Build Echo Reply */
    uint8_t reply[128];
    uint16_t reply_len = payload_len;
    if (reply_len > sizeof(reply)) reply_len = sizeof(reply);

    memcpy(reply, payload, reply_len);
    reply[0] = ICMPV6_ECHO_REPLY;  /* Change type to reply */
    reply[2] = 0; reply[3] = 0;    /* Clear checksum */

    /* Compute checksum */
    const ipv6_addr_t *src_addr = &ipv6_iface.link_local;
    if (!ipv6_addr_is_unspecified(&ipv6_iface.addresses[0].address))
        src_addr = &ipv6_iface.addresses[0].address;

    uint16_t cksum = ipv6_icmp_checksum(src_addr, &ip6->src, reply, reply_len);
    reply[2] = (uint8_t)(cksum >> 8);
    reply[3] = (uint8_t)(cksum & 0xFF);

    ipv6_send_packet(&ip6->src, 58, reply, reply_len);
}

static void icmpv6_handle_neighbor_solicit(const ipv6_header_t *ip6,
                                            const uint8_t *payload, uint16_t payload_len) {
    if (payload_len < 4 + 20) return;  /* icmpv6 hdr(4) + reserved(4) + target(16) */

    /* Parse NS */
    const nd_neighbor_solicit_t *ns = (const nd_neighbor_solicit_t *)(payload + 4);

    /* Check if target is one of our addresses */
    bool is_us = ipv6_addr_equal(&ns->target, &ipv6_iface.link_local);
    for (int i = 0; i < IPV6_MAX_ADDRESSES && !is_us; i++) {
        if (ipv6_iface.addresses[i].valid &&
            ipv6_addr_equal(&ns->target, &ipv6_iface.addresses[i].address)) {
            is_us = true;
        }
    }

    if (!is_us) return;

    /* Extract source link-layer address option if present */
    uint16_t opt_offset = 4 + sizeof(nd_neighbor_solicit_t);
    while (opt_offset + 2 <= payload_len) {
        uint8_t opt_type = payload[opt_offset];
        uint8_t opt_len = payload[opt_offset + 1];
        if (opt_len == 0) break;
        if (opt_type == ND_OPT_SOURCE_LINK_ADDR && opt_len >= 1) {
            ndp_cache_add(&ip6->src, payload + opt_offset + 2);
        }
        opt_offset += opt_len * 8;
    }

    serial_puts("ipv6: received NS for our address, sending NA\n");

    /* Build Neighbor Advertisement */
    uint8_t reply[64];
    memset(reply, 0, sizeof(reply));

    /* ICMPv6 header */
    reply[0] = ICMPV6_NEIGHBOR_ADVERTISEMENT;
    reply[1] = 0;
    reply[2] = 0; reply[3] = 0;  /* ICMPv6 cksum: computed below */

    /* NA body: flags + target */
    uint32_t flags = htonl32(ND_NA_FLAG_SOLICITED | ND_NA_FLAG_OVERRIDE);
    memcpy(reply + 4, &flags, 4);
    memcpy(reply + 8, ns->target.addr, 16);

    /* Target Link-Layer Address option */
    reply[24] = ND_OPT_TARGET_LINK_ADDR;
    reply[25] = 1;  /* Length in 8-byte units */
    memcpy(reply + 26, ipv6_iface.mac, 6);

    uint16_t reply_len = 32;

    /* Checksum */
    const ipv6_addr_t *src_addr = &ns->target;
    const ipv6_addr_t *dst_addr = ipv6_addr_is_unspecified(&ip6->src) ?
                                  &IPV6_ADDR_ALL_NODES : &ip6->src;

    uint16_t cksum = ipv6_icmp_checksum(src_addr, dst_addr, reply, reply_len);
    reply[2] = (uint8_t)(cksum >> 8);
    reply[3] = (uint8_t)(cksum & 0xFF);

    ipv6_send_packet(dst_addr, 58, reply, reply_len);
}

static void icmpv6_handle_neighbor_advert(const ipv6_header_t *ip6,
                                           const uint8_t *payload, uint16_t payload_len) {
    if (payload_len < 4 + 20) return;

    const nd_neighbor_advert_t *na = (const nd_neighbor_advert_t *)(payload + 4);

    /* Extract target link-layer address */
    uint16_t opt_offset = 4 + sizeof(nd_neighbor_advert_t);
    while (opt_offset + 2 <= payload_len) {
        uint8_t opt_type = payload[opt_offset];
        uint8_t opt_len = payload[opt_offset + 1];
        if (opt_len == 0) break;
        if (opt_type == ND_OPT_TARGET_LINK_ADDR && opt_len >= 1) {
            ndp_cache_add(&na->target, payload + opt_offset + 2);
            serial_puts("ipv6: learned neighbor MAC from NA\n");
        }
        opt_offset += opt_len * 8;
    }
}

static void icmpv6_handle_router_advert(const ipv6_header_t *ip6,
                                         const uint8_t *payload, uint16_t payload_len) {
    if (payload_len < 4 + sizeof(nd_router_advert_t)) return;

    const nd_router_advert_t *ra = (const nd_router_advert_t *)(payload + 4);
    serial_printf("ipv6: received Router Advertisement (hop_limit=%d, lifetime=%d)\n",
                  ra->cur_hop_limit, ntohs16(ra->router_lifetime));

    if (ra->cur_hop_limit > 0)
        ipv6_iface.hop_limit = ra->cur_hop_limit;

    /* Store router as gateway */
    ipv6_iface.gateway = ip6->src;

    /* Cache router's MAC */
    uint16_t opt_offset = 4 + sizeof(nd_router_advert_t);
    while (opt_offset + 2 <= payload_len) {
        uint8_t opt_type = payload[opt_offset];
        uint8_t opt_len = payload[opt_offset + 1];
        if (opt_len == 0) break;

        if (opt_type == ND_OPT_SOURCE_LINK_ADDR && opt_len >= 1) {
            ndp_cache_add(&ip6->src, payload + opt_offset + 2);
        } else if (opt_type == ND_OPT_PREFIX_INFO && opt_len == 4) {
            const nd_prefix_info_t *pi = (const nd_prefix_info_t *)(payload + opt_offset);
            if ((pi->flags & 0x40) && pi->prefix_length <= 128) {
                /* Autonomous flag set - configure address via SLAAC */
                serial_printf("ipv6: SLAAC prefix /%d\n", pi->prefix_length);

                /* Generate global address: prefix + EUI-64 */
                ipv6_addr_t global;
                memcpy(global.addr, pi->prefix.addr, 8);  /* Network prefix */
                ipv6_eui64_from_mac(ipv6_iface.mac, global.addr + 8);

                /* Store as first global address */
                for (int i = 0; i < IPV6_MAX_ADDRESSES; i++) {
                    if (!ipv6_iface.addresses[i].valid) {
                        ipv6_iface.addresses[i].address = global;
                        ipv6_iface.addresses[i].prefix_length = pi->prefix_length;
                        ipv6_iface.addresses[i].valid = true;

                        char buf[48];
                        ipv6_addr_to_string(&global, buf, sizeof(buf));
                        printf("IPv6: SLAAC configured address %s/%d\n", buf, pi->prefix_length);
                        serial_printf("ipv6: SLAAC global address = %s/%d\n", buf, pi->prefix_length);
                        break;
                    }
                }
                ipv6_iface.slaac_complete = true;
            }
        }
        opt_offset += opt_len * 8;
    }
}

static void icmpv6_handle(const ipv6_header_t *ip6, const uint8_t *payload, uint16_t len) {
    if (len < 4) return;

    uint8_t type = payload[0];
    switch (type) {
        case ICMPV6_ECHO_REQUEST:
            icmpv6_handle_echo_request(ip6, payload, len);
            break;
        case ICMPV6_ECHO_REPLY: {
            uint16_t id  = (payload[4] << 8) | payload[5];
            uint16_t seq = (payload[6] << 8) | payload[7];
            char buf[48];
            ipv6_addr_to_string(&ip6->src, buf, sizeof(buf));
            printf("ICMPv6 Echo Reply from %s: id=%d seq=%d\n", buf, id, seq);
            break;
        }
        case ICMPV6_ROUTER_ADVERTISEMENT:
            icmpv6_handle_router_advert(ip6, payload, len);
            break;
        case ICMPV6_NEIGHBOR_SOLICITATION:
            icmpv6_handle_neighbor_solicit(ip6, payload, len);
            break;
        case ICMPV6_NEIGHBOR_ADVERTISEMENT:
            icmpv6_handle_neighbor_advert(ip6, payload, len);
            break;
        default:
            serial_printf("ipv6: unhandled ICMPv6 type %d\n", type);
            break;
    }
}

/* ============================================================================
 * IPv6 Packet Handling
 * ============================================================================ */

int ipv6_init(void) {
    memset(&ipv6_iface, 0, sizeof(ipv6_iface));
    ipv6_iface.hop_limit = 64;

    /* Get MAC from network device */
    net_device_t *dev = net_device_get_current();
    if (dev && dev->initialized) {
        memcpy(ipv6_iface.mac, dev->mac_address, 6);
    }

    /* Generate link-local address: fe80:: + EUI-64 */
    ipv6_iface.link_local.addr[0] = 0xFE;
    ipv6_iface.link_local.addr[1] = 0x80;
    ipv6_eui64_from_mac(ipv6_iface.mac, ipv6_iface.link_local.addr + 8);

    ipv6_iface.enabled = true;

    char buf[48];
    ipv6_addr_to_string(&ipv6_iface.link_local, buf, sizeof(buf));
    serial_printf("ipv6: link-local address = %s\n", buf);
    printf("IPv6: Link-local %s\n", buf);

    return 0;
}

void ipv6_handle_packet(const uint8_t *frame, uint32_t len) {
    if (!ipv6_iface.enabled) return;
    if (len < sizeof(ipv6_header_t)) return;

    const ipv6_header_t *ip6 = (const ipv6_header_t *)frame;

    /* Verify version = 6 */
    uint8_t version = (uint8_t)((htonl32(ip6->version_tc_flow) >> 28) & 0xF);
    if (version != 6) {
        serial_puts("ipv6: invalid version\n");
        return;
    }

    uint16_t payload_len = ntohs16(ip6->payload_length);
    if (sizeof(ipv6_header_t) + payload_len > len) return;

    const uint8_t *payload = frame + sizeof(ipv6_header_t);

    /* Check if packet is for us */
    bool for_us = ipv6_addr_equal(&ip6->dst, &ipv6_iface.link_local) ||
                  ipv6_addr_is_multicast(&ip6->dst);
    for (int i = 0; i < IPV6_MAX_ADDRESSES && !for_us; i++) {
        if (ipv6_iface.addresses[i].valid &&
            ipv6_addr_equal(&ip6->dst, &ipv6_iface.addresses[i].address)) {
            for_us = true;
        }
    }

    /* Also accept solicited-node multicast for our addresses */
    if (!for_us && ipv6_addr_is_multicast(&ip6->dst)) {
        /* Check solicited-node multicast */
        if (ip6->dst.addr[0] == 0xFF && ip6->dst.addr[1] == 0x02 &&
            ip6->dst.addr[11] == 0x01 && ip6->dst.addr[12] == 0xFF) {
            /* Compare last 3 bytes with our link-local */
            if (ip6->dst.addr[13] == ipv6_iface.link_local.addr[13] &&
                ip6->dst.addr[14] == ipv6_iface.link_local.addr[14] &&
                ip6->dst.addr[15] == ipv6_iface.link_local.addr[15]) {
                for_us = true;
            }
        }
    }

    if (!for_us) return;

    /* Dispatch based on Next Header */
    switch (ip6->next_header) {
        case 58:  /* ICMPv6 */
            icmpv6_handle(ip6, payload, payload_len);
            break;
        case 6:   /* TCP */
            serial_puts("ipv6: TCP over IPv6 not yet supported\n");
            break;
        case 17:  /* UDP */
            serial_puts("ipv6: UDP over IPv6 not yet supported\n");
            break;
        default:
            serial_printf("ipv6: unhandled next header %d\n", ip6->next_header);
            break;
    }
}

int ipv6_send_packet(const ipv6_addr_t *dst, uint8_t next_header,
                     const uint8_t *payload, uint16_t payload_len) {
    if (!ipv6_iface.enabled || !dst || !payload) return -1;

    net_device_t *dev = net_device_get_current();
    if (!dev || !dev->initialized) return -1;

    /* Build ethernet + IPv6 packet */
    uint32_t total_len = 14 + sizeof(ipv6_header_t) + payload_len;
    uint8_t *pkt = (uint8_t *)kalloc(total_len);
    if (!pkt) return -1;

    /* Determine destination MAC */
    uint8_t dst_mac[6];
    if (ipv6_addr_is_multicast(dst)) {
        /* Multicast: derive MAC from IPv6 multicast address */
        dst_mac[0] = 0x33;
        dst_mac[1] = 0x33;
        dst_mac[2] = dst->addr[12];
        dst_mac[3] = dst->addr[13];
        dst_mac[4] = dst->addr[14];
        dst_mac[5] = dst->addr[15];
    } else {
        /* Unicast: lookup in neighbor cache */
        if (ipv6_neighbor_lookup(dst, dst_mac) != 0) {
            /* Not in cache - try to resolve */
            ipv6_neighbor_resolve(dst);
            kfree(pkt);
            return -1;  /* Packet dropped; will be retried after resolution */
        }
    }

    /* Ethernet header */
    memcpy(pkt, dst_mac, 6);
    memcpy(pkt + 6, ipv6_iface.mac, 6);
    pkt[12] = 0x86; pkt[13] = 0xDD;  /* EtherType: IPv6 */

    /* IPv6 header */
    ipv6_header_t *ip6 = (ipv6_header_t *)(pkt + 14);
    ip6->version_tc_flow = htonl32(0x60000000);  /* Version 6 */
    ip6->payload_length = htons16(payload_len);
    ip6->next_header = next_header;
    ip6->hop_limit = ipv6_iface.hop_limit;

    /* Source address: prefer global, fall back to link-local */
    if (!ipv6_addr_is_unspecified(&ipv6_iface.addresses[0].address) &&
        ipv6_iface.addresses[0].valid) {
        ip6->src = ipv6_iface.addresses[0].address;
    } else {
        ip6->src = ipv6_iface.link_local;
    }
    ip6->dst = *dst;

    /* Payload */
    memcpy(pkt + 14 + sizeof(ipv6_header_t), payload, payload_len);

    /* Send via NIC */
    int ret = -1;
    if (dev->ops.send_packet) {
        ret = dev->ops.send_packet(pkt, total_len);
    }

    kfree(pkt);
    return ret >= 0 ? 0 : -1;
}

int ipv6_ping(const ipv6_addr_t *dst) {
    if (!ipv6_iface.enabled || !dst) return -1;

    uint8_t icmp[16];
    memset(icmp, 0, sizeof(icmp));

    icmp[0] = ICMPV6_ECHO_REQUEST;
    icmp[1] = 0;
    icmp[2] = 0; icmp[3] = 0;  /* filled by ipv6_icmp_checksum below */
    /* Identifier */
    icmp[4] = 0x00; icmp[5] = 0x01;
    /* Sequence */
    icmp6_seq++;
    icmp[6] = (uint8_t)(icmp6_seq >> 8);
    icmp[7] = (uint8_t)(icmp6_seq & 0xFF);

    /* Source address */
    const ipv6_addr_t *src = &ipv6_iface.link_local;
    if (ipv6_iface.addresses[0].valid)
        src = &ipv6_iface.addresses[0].address;

    uint16_t cksum = ipv6_icmp_checksum(src, dst, icmp, 16);
    icmp[2] = (uint8_t)(cksum >> 8);
    icmp[3] = (uint8_t)(cksum & 0xFF);

    char buf[48];
    ipv6_addr_to_string(dst, buf, sizeof(buf));
    printf("PING6 %s: seq=%d\n", buf, icmp6_seq);

    return ipv6_send_packet(dst, 58, icmp, 16);
}

void ipv6_start_slaac(void) {
    if (!ipv6_iface.enabled) return;

    char buf[48];
    ipv6_addr_to_string(&ipv6_iface.link_local, buf, sizeof(buf));
    printf("IPv6: Starting SLAAC (link-local: %s)\n", buf);

    /* Send Router Solicitation to ff02::2 */
    uint8_t rs[16];
    memset(rs, 0, sizeof(rs));
    rs[0] = ICMPV6_ROUTER_SOLICITATION;
    rs[1] = 0;

    /* Source link-layer address option */
    rs[8] = ND_OPT_SOURCE_LINK_ADDR;
    rs[9] = 1;
    memcpy(rs + 10, ipv6_iface.mac, 6);

    uint16_t cksum = ipv6_icmp_checksum(&ipv6_iface.link_local,
                                         &IPV6_ADDR_ALL_ROUTERS, rs, 16);
    rs[2] = (uint8_t)(cksum >> 8);
    rs[3] = (uint8_t)(cksum & 0xFF);

    ipv6_send_packet(&IPV6_ADDR_ALL_ROUTERS, 58, rs, 16);
    serial_puts("ipv6: sent Router Solicitation\n");
}

int ipv6_neighbor_resolve(const ipv6_addr_t *target) {
    if (!target) return -1;

    /* Check cache first */
    ndp_neighbor_entry_t *entry = ndp_cache_find(target);
    if (entry && entry->state == NDP_STATE_REACHABLE) return 0;

    /* Send Neighbor Solicitation */
    uint8_t ns[32];
    memset(ns, 0, sizeof(ns));
    ns[0] = ICMPV6_NEIGHBOR_SOLICITATION;
    ns[1] = 0;
    /* reserved(4) at offset 4 */
    memcpy(ns + 8, target->addr, 16);  /* Target address */

    /* Source link-layer address option */
    ns[24] = ND_OPT_SOURCE_LINK_ADDR;
    ns[25] = 1;
    memcpy(ns + 26, ipv6_iface.mac, 6);

    /* Send to solicited-node multicast */
    ipv6_addr_t sol_addr;
    ipv6_solicited_node_addr(target, &sol_addr);

    uint16_t cksum = ipv6_icmp_checksum(&ipv6_iface.link_local, &sol_addr, ns, 32);
    ns[2] = (uint8_t)(cksum >> 8);
    ns[3] = (uint8_t)(cksum & 0xFF);

    ipv6_send_packet(&sol_addr, 58, ns, 32);
    return 1;
}

int ipv6_neighbor_lookup(const ipv6_addr_t *ip, uint8_t *mac) {
    ndp_neighbor_entry_t *entry = ndp_cache_find(ip);
    if (entry && entry->state >= NDP_STATE_REACHABLE) {
        memcpy(mac, entry->mac, 6);
        return 0;
    }
    return -1;
}

const ipv6_addr_t* ipv6_get_link_local(void) {
    return &ipv6_iface.link_local;
}

const ipv6_addr_t* ipv6_get_global_address(void) {
    for (int i = 0; i < IPV6_MAX_ADDRESSES; i++) {
        if (ipv6_iface.addresses[i].valid)
            return &ipv6_iface.addresses[i].address;
    }
    return NULL;
}

bool ipv6_is_enabled(void) {
    return ipv6_iface.enabled;
}

void ipv6_print_status(void) {
    char buf[48];

    printf("=== IPv6 Status ===\n");
    printf("  Enabled: %s\n", ipv6_iface.enabled ? "yes" : "no");
    printf("  MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
           ipv6_iface.mac[0], ipv6_iface.mac[1], ipv6_iface.mac[2],
           ipv6_iface.mac[3], ipv6_iface.mac[4], ipv6_iface.mac[5]);

    ipv6_addr_to_string(&ipv6_iface.link_local, buf, sizeof(buf));
    printf("  Link-local: %s\n", buf);

    for (int i = 0; i < IPV6_MAX_ADDRESSES; i++) {
        if (ipv6_iface.addresses[i].valid) {
            ipv6_addr_to_string(&ipv6_iface.addresses[i].address, buf, sizeof(buf));
            printf("  Global[%d]: %s/%d\n", i, buf, ipv6_iface.addresses[i].prefix_length);
        }
    }

    if (!ipv6_addr_is_unspecified(&ipv6_iface.gateway)) {
        ipv6_addr_to_string(&ipv6_iface.gateway, buf, sizeof(buf));
        printf("  Gateway: %s\n", buf);
    }

    printf("  SLAAC: %s\n", ipv6_iface.slaac_complete ? "complete" : "pending");
    printf("  Hop limit: %d\n", ipv6_iface.hop_limit);

    /* Neighbor cache */
    int cached = 0;
    for (int i = 0; i < IPV6_NEIGHBOR_CACHE_SIZE; i++) {
        if (ipv6_iface.neighbor_cache[i].state >= NDP_STATE_REACHABLE) cached++;
    }
    printf("  Neighbor cache: %d entries\n", cached);
}
