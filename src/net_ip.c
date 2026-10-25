#include "net_ip.h"
#include "string.h"
#include "serial.h"
#include "net_device.h"
#include "net_ethernet.h"
#include "net_arp.h"
#include "net_icmp.h"
#include "net_udp.h"
#include "net_tcp.h"
#include "mem.h"
#include "ktime.h"
#include "atomic.h"
#include "debug_config.h"

// IP protocol numbers
#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6
#define IP_PROTO_UDP  17

// IP header flags (host byte order, as used in flags_fragment field after byteswap)
#define IP_FLAG_DF 0x4000  // Don't Fragment
#define IP_FLAG_MF 0x2000  // More Fragments
#define IP_FRAG_OFFSET_MASK 0x1FFF // Fragment offset mask

// Fragment reassembly constants
#define IP_FRAG_MAX_SLOTS    8      // Maximum concurrent reassembly slots
#define IP_FRAG_MAX_SIZE     65535  // Max reassembled datagram size
#define IP_FRAG_TIMEOUT_MS   15000  // 15 seconds (RFC 791 recommends 15s)
#define IP_MTU               1500   // Ethernet MTU

static uint16_t ip_id_counter = 0;
static spinlock_t ip_lock;

/* ── Loopback (127.0.0.0/8 and our own address) ─────────────────────────────
 * Datagrams for a local destination never touch the NIC: ip_send_packet()
 * builds the full frame and parks it here; ip_loopback_drain() (called from
 * poll_network_once() and the main-loop RX service) feeds it back through
 * ip_handle_packet().  Delivery is deferred on purpose: TCP sends while
 * holding tcp_lock, so handling the segment synchronously would deadlock. */
#define IP_LO_MAX 512
static uint8_t  *ip_lo_frame[IP_LO_MAX];
static uint32_t  ip_lo_flen[IP_LO_MAX];
static uint32_t  ip_lo_head, ip_lo_tail, ip_lo_count;
static spinlock_t ip_lo_lock;
static volatile int ip_lo_draining;

int ip_is_local_dst(const uint8_t *dst) {
    if (!dst) return 0;
    if (dst[0] == 127) return 1;
    uint8_t our[4];
    net_get_ip_address(our);
    if ((our[0] | our[1] | our[2] | our[3]) == 0) return 0;
    return memcmp(dst, our, 4) == 0;
}

void ip_select_src(const uint8_t *dst, uint8_t *src) {
    if (dst && dst[0] == 127) { memcpy(src, dst, 4); return; }
    net_get_ip_address(src);
}

static uint16_t ip_header_checksum_bytes(const uint8_t *header_bytes, uint16_t header_len);

static int ip_lo_enqueue(const uint8_t *dst_ip, uint8_t protocol,
                         const uint8_t *payload, uint16_t payload_len) {
    uint32_t total = 20u + payload_len;
    if (total > 0xFFFF) return -1;
    uint8_t *f = kalloc(14 + total);
    if (!f) return -1;
    memset(f, 0, 34);
    uint8_t mac[6];
    net_get_mac_address(mac);
    memcpy(f, mac, 6); memcpy(f + 6, mac, 6);
    f[12] = 0x08; f[13] = 0x00;
    uint8_t *h = f + 14;
    h[0] = 0x45;
    h[2] = (uint8_t)(total >> 8); h[3] = (uint8_t)total;
    uint16_t id = __atomic_fetch_add(&ip_id_counter, 1, __ATOMIC_RELAXED);
    h[4] = (uint8_t)(id >> 8); h[5] = (uint8_t)id;
    h[6] = 0x40;                       /* DF */
    h[8] = 64; h[9] = protocol;
    ip_select_src(dst_ip, h + 12);
    memcpy(h + 16, dst_ip, 4);
    uint16_t cs = ip_header_checksum_bytes(h, 20);
    h[10] = (uint8_t)(cs >> 8); h[11] = (uint8_t)cs;
    if (payload && payload_len) memcpy(f + 34, payload, payload_len);
    klock(&ip_lo_lock);
    if (ip_lo_count >= IP_LO_MAX) {
        kunlock(&ip_lo_lock);
        kfree(f);
        return 0;                      /* dropped like a full NIC queue; TCP retransmits */
    }
    ip_lo_frame[ip_lo_tail] = f;
    ip_lo_flen[ip_lo_tail] = 14 + total;
    ip_lo_tail = (ip_lo_tail + 1) % IP_LO_MAX;
    ip_lo_count++;
    kunlock(&ip_lo_lock);
    return 0;
}

int ip_loopback_pending(void) { return ip_lo_count != 0; }

/* Deliver queued local datagrams; replies they trigger are delivered in the
 * same call (bounded so a ping-pong cannot spin forever). */
int ip_loopback_drain(void) {
    if (__atomic_exchange_n(&ip_lo_draining, 1, __ATOMIC_ACQUIRE))
        return 0;
    int n = 0;
    while (n < 1024) {
        klock(&ip_lo_lock);
        if (ip_lo_count == 0) { kunlock(&ip_lo_lock); break; }
        uint8_t *f = ip_lo_frame[ip_lo_head];
        uint32_t len = ip_lo_flen[ip_lo_head];
        ip_lo_frame[ip_lo_head] = NULL;
        ip_lo_head = (ip_lo_head + 1) % IP_LO_MAX;
        ip_lo_count--;
        kunlock(&ip_lo_lock);
        ip_handle_packet(f, len);
        kfree(f);
        n++;
    }
    __atomic_store_n(&ip_lo_draining, 0, __ATOMIC_RELEASE);
    return n;
}

// ---- ARP-pending transmit queue ---------------------------------------------
//
// Datagrams whose next hop has no ARP entry yet wait here.  arp_insert()
// flushes them via ip_arp_resolved(); ip_arpq_tick() drives ARP retries and
// drops entries once resolution gives up.

#define IP_ARPQ_SLOTS       16
#define IP_ARPQ_MAX_AGE_MS  3000

typedef struct {
    int      valid;
    uint8_t  next_hop[4];
    uint8_t  dst_ip[4];
    uint8_t  protocol;
    uint16_t len;
    uint8_t *payload;
    uint64_t queued_at;
} ip_arpq_entry_t;

static ip_arpq_entry_t ip_arpq[IP_ARPQ_SLOTS];

static int ip_arpq_enqueue(const uint8_t *next_hop, const uint8_t *dst_ip,
                           uint8_t protocol, const uint8_t *payload,
                           uint16_t payload_len) {
    uint8_t *copy = NULL;
    if (payload_len > 0) {
        copy = kalloc(payload_len);
        if (!copy) return -1;
        memcpy(copy, payload, payload_len);
    }

    uint64_t flags = irq_save();
    int slot = -1, oldest = 0;
    for (int i = 0; i < IP_ARPQ_SLOTS; i++) {
        if (!ip_arpq[i].valid) { slot = i; break; }
        if (ip_arpq[i].queued_at < ip_arpq[oldest].queued_at) oldest = i;
    }
    uint8_t *evicted = NULL;
    if (slot < 0) {
        slot = oldest;
        evicted = ip_arpq[slot].payload;
    }
    ip_arpq_entry_t *e = &ip_arpq[slot];
    memcpy(e->next_hop, next_hop, 4);
    memcpy(e->dst_ip, dst_ip, 4);
    e->protocol  = protocol;
    e->len       = payload_len;
    e->payload   = copy;
    e->queued_at = ktime_now_ms();
    e->valid     = 1;
    irq_restore(flags);

    if (evicted) kfree(evicted);
    return 0;
}

void ip_arp_resolved(const uint8_t *ip) {
    for (int i = 0; i < IP_ARPQ_SLOTS; i++) {
        uint64_t flags = irq_save();
        if (!ip_arpq[i].valid || memcmp(ip_arpq[i].next_hop, ip, 4) != 0) {
            irq_restore(flags);
            continue;
        }
        ip_arpq_entry_t e = ip_arpq[i];
        ip_arpq[i].valid = 0;
        ip_arpq[i].payload = NULL;
        irq_restore(flags);

        ip_send_packet(e.dst_ip, e.protocol, e.payload, e.len);
        if (e.payload) kfree(e.payload);
    }
}

void ip_arpq_tick(void) {
    uint64_t now = ktime_now_ms();
    for (int i = 0; i < IP_ARPQ_SLOTS; i++) {
        uint64_t flags = irq_save();
        if (!ip_arpq[i].valid) { irq_restore(flags); continue; }
        uint8_t hop[4];
        memcpy(hop, ip_arpq[i].next_hop, 4);
        int expired = now - ip_arpq[i].queued_at > IP_ARPQ_MAX_AGE_MS;
        uint8_t *drop = NULL;
        if (expired) {
            drop = ip_arpq[i].payload;
            ip_arpq[i].payload = NULL;
            ip_arpq[i].valid = 0;
        }
        irq_restore(flags);

        if (drop) {
            kfree(drop);
            SERIAL_DEBUG_PRINT(NET_IP, "IP: dropped datagram, no ARP reply from %d.%d.%d.%d\n",
                               hop[0], hop[1], hop[2], hop[3]);
            continue;
        }
        uint8_t mac[6];
        if (arp_resolve(hop, mac))
            ip_arp_resolved(hop);
    }
}

// ---- IP Fragment Reassembly -------------------------------------------------

// A single fragment hole/piece
typedef struct ip_frag_piece {
    uint16_t offset;           // Fragment offset (in bytes, not 8-byte units)
    uint16_t length;           // Fragment data length
    uint8_t *data;             // Fragment data copy
    struct ip_frag_piece *next;
} ip_frag_piece_t;

// Reassembly slot: collects fragments for one datagram
typedef struct {
    uint8_t  src_ip[4];
    uint8_t  dst_ip[4];
    uint16_t id;               // IP identification
    uint8_t  protocol;
    uint8_t  active;           // 1 = in use
    uint16_t total_len;        // Known total length (0 = not yet known)
    uint16_t received_bytes;   // Sum of fragment data received so far
    uint64_t first_seen_ms;    // Timestamp for timeout
    ip_frag_piece_t *pieces;   // Linked list of received fragments (sorted by offset)
} ip_frag_slot_t;

static ip_frag_slot_t ip_frag_slots[IP_FRAG_MAX_SLOTS];

static uint64_t ip_now_ms(void) {
    extern uint64_t hpet_get_ticks(void);
    return hpet_get_ticks();
}

/**
 * Free all pieces in a reassembly slot and mark it inactive.
 */
static void ip_frag_slot_free(ip_frag_slot_t *slot) {
    ip_frag_piece_t *p = slot->pieces;
    while (p) {
        ip_frag_piece_t *next = p->next;
        if (p->data) kfree(p->data);
        kfree(p);
        p = next;
    }
    slot->pieces = NULL;
    slot->active = 0;
    slot->total_len = 0;
    slot->received_bytes = 0;
}

/**
 * Find or allocate a reassembly slot for the given datagram identity.
 */
static ip_frag_slot_t *ip_frag_find_or_create(const uint8_t *src_ip, const uint8_t *dst_ip,
                                               uint16_t id, uint8_t protocol) {
    // Search existing
    for (int i = 0; i < IP_FRAG_MAX_SLOTS; i++) {
        if (!ip_frag_slots[i].active) continue;
        if (ip_frag_slots[i].id == id &&
            ip_frag_slots[i].protocol == protocol &&
            memcmp(ip_frag_slots[i].src_ip, src_ip, 4) == 0 &&
            memcmp(ip_frag_slots[i].dst_ip, dst_ip, 4) == 0) {
            return &ip_frag_slots[i];
        }
    }

    // Allocate new
    for (int i = 0; i < IP_FRAG_MAX_SLOTS; i++) {
        if (!ip_frag_slots[i].active) {
            ip_frag_slots[i].active = 1;
            memcpy(ip_frag_slots[i].src_ip, src_ip, 4);
            memcpy(ip_frag_slots[i].dst_ip, dst_ip, 4);
            ip_frag_slots[i].id = id;
            ip_frag_slots[i].protocol = protocol;
            ip_frag_slots[i].total_len = 0;
            ip_frag_slots[i].received_bytes = 0;
            ip_frag_slots[i].first_seen_ms = ip_now_ms();
            ip_frag_slots[i].pieces = NULL;
            return &ip_frag_slots[i];
        }
    }

    // No free slots – evict the oldest
    int oldest = 0;
    uint64_t oldest_time = ip_frag_slots[0].first_seen_ms;
    for (int i = 1; i < IP_FRAG_MAX_SLOTS; i++) {
        if (ip_frag_slots[i].first_seen_ms < oldest_time) {
            oldest_time = ip_frag_slots[i].first_seen_ms;
            oldest = i;
        }
    }
    ip_frag_slot_free(&ip_frag_slots[oldest]);
    ip_frag_slots[oldest].active = 1;
    memcpy(ip_frag_slots[oldest].src_ip, src_ip, 4);
    memcpy(ip_frag_slots[oldest].dst_ip, dst_ip, 4);
    ip_frag_slots[oldest].id = id;
    ip_frag_slots[oldest].protocol = protocol;
    ip_frag_slots[oldest].first_seen_ms = ip_now_ms();
    return &ip_frag_slots[oldest];
}

/**
 * Add a fragment to a reassembly slot. Returns 1 if reassembly is complete.
 */
static int ip_frag_add(ip_frag_slot_t *slot, uint16_t offset, const uint8_t *data,
                        uint16_t data_len, int more_fragments) {
    // Allocate piece
    ip_frag_piece_t *piece = kalloc(sizeof(ip_frag_piece_t));
    if (!piece) return 0;

    piece->offset = offset;
    piece->length = data_len;
    piece->data = kalloc(data_len);
    if (!piece->data) {
        kfree(piece);
        return 0;
    }
    memcpy(piece->data, data, data_len);

    // Insert sorted by offset
    ip_frag_piece_t **pp = &slot->pieces;
    while (*pp && (*pp)->offset < offset) {
        pp = &(*pp)->next;
    }
    piece->next = *pp;
    *pp = piece;

    slot->received_bytes += data_len;

    // If this is the last fragment, we know the total length
    if (!more_fragments) {
        slot->total_len = offset + data_len;
    }

    // Check if reassembly is complete
    if (slot->total_len > 0 && slot->received_bytes >= slot->total_len) {
        // Verify contiguity
        uint16_t expected_offset = 0;
        ip_frag_piece_t *p = slot->pieces;
        while (p) {
            if (p->offset != expected_offset) return 0;  // Gap
            expected_offset += p->length;
            p = p->next;
        }
        return (expected_offset == slot->total_len) ? 1 : 0;
    }

    return 0;
}

/**
 * Reassemble a completed datagram from its pieces.
 * Caller must kfree the returned buffer.
 */
static uint8_t *ip_frag_reassemble(ip_frag_slot_t *slot, uint16_t *out_len) {
    if (slot->total_len == 0) return NULL;

    uint8_t *buf = kalloc(slot->total_len);
    if (!buf) return NULL;

    ip_frag_piece_t *p = slot->pieces;
    while (p) {
        memcpy(buf + p->offset, p->data, p->length);
        p = p->next;
    }

    *out_len = slot->total_len;
    return buf;
}

void ip_fragment_cleanup(void) {
    uint64_t now = ip_now_ms();
    klock(&ip_lock);
    for (int i = 0; i < IP_FRAG_MAX_SLOTS; i++) {
        if (!ip_frag_slots[i].active) continue;
        if (now - ip_frag_slots[i].first_seen_ms > IP_FRAG_TIMEOUT_MS) {
            DEBUG_PRINT(NET_IP, "IP: Fragment reassembly timeout for id %u\n", ip_frag_slots[i].id);
            ip_frag_slot_free(&ip_frag_slots[i]);
        }
    }
    kunlock(&ip_lock);
}

void ip_init() {
    ip_id_counter = 0;
    printf("IP: Initialized\n");
}

// Calculate IPv4 header checksum over raw bytes (host-order return)
static uint16_t ip_header_checksum_bytes(const uint8_t *header_bytes, uint16_t header_len) {
    uint32_t sum = 0;
    // Sum 16-bit words in network order
    for (uint16_t i = 0; i + 1 < header_len; i += 2) {
        uint16_t word = ((uint16_t)header_bytes[i] << 8) | (uint16_t)header_bytes[i + 1];
        sum += word;
    }
    // Handle odd trailing byte
    if (header_len & 1) {
        sum += ((uint16_t)header_bytes[header_len - 1]) << 8;
    }
    // Fold carries
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return (uint16_t)(~sum);
}

void ip_handle_packet(const uint8_t *frame, uint32_t len) {
    if (len < 34) { // Ethernet header (14) + minimum IP header (20)
        net_rx_count_malformed();
        NET_RX_LOG("IP: Packet too short (%d bytes)\n", len);
        return;
    }
    
    struct ipv4_header *ip_header = (struct ipv4_header*)(frame + 14);
    
    // Read version and header length early for bounds checking and checksum
    uint8_t version = (ip_header->version_ihl >> 4) & 0xF;
    uint8_t header_len = (ip_header->version_ihl & 0xF) * 4;
    if (version != 4) {
        net_rx_count_malformed();
        NET_RX_LOG("IP: Unsupported version %d\n", version);
        return;
    }
    if (header_len < 20 || (uint32_t)(14 + header_len) > len) {
        net_rx_count_malformed();
        NET_RX_LOG("IP: Invalid header length %d for frame length %d\n", header_len, len);
        return;
    }
    
    // Check if this is for us or broadcast
    uint8_t our_ip[4];
    net_get_ip_address(our_ip);
    int is_broadcast = (ip_header->dst_ip[0] == 255 && ip_header->dst_ip[1] == 255 &&
                        ip_header->dst_ip[2] == 255 && ip_header->dst_ip[3] == 255);
    
    // Also accept packets if our IP is 0.0.0.0 (e.g. during DHCP configuration)
    int is_unconfigured = (our_ip[0] == 0 && our_ip[1] == 0 && our_ip[2] == 0 && our_ip[3] == 0);
    
    int is_loop = (ip_header->dst_ip[0] == 127);
    if (!is_broadcast && !is_unconfigured && !is_loop && memcmp(ip_header->dst_ip, our_ip, 4) != 0) {
        DEBUG_PRINT(NET_IP, "IP: Packet not for us (dst: %d.%d.%d.%d)\n",
               ip_header->dst_ip[0], ip_header->dst_ip[1], 
               ip_header->dst_ip[2], ip_header->dst_ip[3]);
        return;
    }
    
    // Verify checksum over raw header bytes
    uint8_t *raw_header = (uint8_t*)ip_header;
    uint16_t original_network_order = ip_header->header_checksum;
    // Convert original to host order for comparison
    uint16_t original_host = (uint16_t)((original_network_order >> 8) | ((original_network_order & 0xFF) << 8));
    // Temporarily zero checksum bytes for calculation
    raw_header[10] = 0;
    raw_header[11] = 0;
    uint16_t calculated_host = ip_header_checksum_bytes(raw_header, header_len);
    // Restore original header bytes
    raw_header[10] = (uint8_t)(original_network_order >> 8);
    raw_header[11] = (uint8_t)(original_network_order & 0xFF);
    if (original_host != calculated_host) {
        net_rx_count_malformed();
        NET_RX_LOG("IP: Invalid checksum (got 0x%04x, expected 0x%04x)\n", 
               original_host, calculated_host);
        return;
    }
    
    
    // Validate total length before any payload math
    uint16_t total_len = (ip_header->total_length >> 8) | ((ip_header->total_length & 0xFF) << 8);
    if (total_len < header_len) {
        net_rx_count_malformed();
        NET_RX_LOG("IP: Invalid total length %u < header length %u\n", total_len, header_len);
        return;
    }
    if ((uint32_t)(14 + total_len) > len) {
        net_rx_count_malformed();
        NET_RX_LOG("IP: Truncated frame (ip total %u, frame %u)\n", total_len, len);
        return;
    }
    uint16_t payload_len = (uint16_t)(total_len - header_len);
    
    DEBUG_PRINT(NET_IP, "IP: Received packet from %d.%d.%d.%d, protocol %d, length %d\n",
           ip_header->src_ip[0], ip_header->src_ip[1], 
           ip_header->src_ip[2], ip_header->src_ip[3],
           ip_header->protocol, payload_len);
    
    // Auto-add sender to ARP cache if not already present
    // Extract sender MAC from Ethernet header
    uint8_t sender_mac[6];
    memcpy(sender_mac, frame + 6, 6); // Source MAC is at offset 6 in Ethernet header
    if (!is_loop && ip_header->src_ip[0] != 127 && memcmp(ip_header->src_ip, our_ip, 4) != 0)
        arp_insert(ip_header->src_ip, sender_mac);

    // Check for fragmentation
    uint16_t flags_frag_raw = ((uint16_t)raw_header[6] << 8) | raw_header[7];
    int more_fragments = (flags_frag_raw & 0x2000) != 0;
    uint16_t frag_offset = (flags_frag_raw & 0x1FFF) * 8;  // Convert to byte offset
    uint16_t ip_id_raw = ((uint16_t)raw_header[4] << 8) | raw_header[5];

    if (more_fragments || frag_offset > 0) {
        // This is a fragment – hand off to reassembly
        DEBUG_PRINT(NET_IP, "IP: Fragment id=%u offset=%u len=%u MF=%d\n",
                    ip_id_raw, frag_offset, payload_len, more_fragments);

        klock(&ip_lock);
        ip_frag_slot_t *slot = ip_frag_find_or_create(
            ip_header->src_ip, ip_header->dst_ip, ip_id_raw, ip_header->protocol);
        if (!slot) { kunlock(&ip_lock); return; }

        // Validate reassembly bounds to avoid offset/length overflow.
        if ((uint32_t)frag_offset + (uint32_t)payload_len > IP_FRAG_MAX_SIZE) {
            DEBUG_PRINT(NET_IP, "IP: Fragment exceeds max datagram size (off=%u len=%u)\n",
                        frag_offset, payload_len);
            kunlock(&ip_lock);
            return;
        }

        const uint8_t *frag_data = frame + 14 + header_len;
        int complete = ip_frag_add(slot, frag_offset, frag_data, payload_len, more_fragments);

        if (!complete) { kunlock(&ip_lock); return; }

        // Reassembly complete – build a synthetic non-fragmented frame for dispatch
        uint16_t reassembled_len = 0;
        uint8_t *reassembled = ip_frag_reassemble(slot, &reassembled_len);
        ip_frag_slot_free(slot);
        kunlock(&ip_lock);

        if (!reassembled) return;

        DEBUG_PRINT(NET_IP, "IP: Reassembled %u bytes for protocol %d\n",
                    reassembled_len, ip_header->protocol);

        // Build a synthetic Ethernet + IP + payload frame for the protocol handler
        uint32_t synth_frame_len = 14 + 20 + reassembled_len;
        uint8_t *synth_frame = kalloc(synth_frame_len);
        if (!synth_frame) { kfree(reassembled); return; }

        // Copy Ethernet header from original frame
        memcpy(synth_frame, frame, 14);
        // Build a minimal IP header
        memcpy(synth_frame + 14, raw_header, 20 > header_len ? header_len : 20);
        // Fix total_length in the synthetic header
        uint16_t synth_total = 20 + reassembled_len;
        synth_frame[16] = (uint8_t)(synth_total >> 8);
        synth_frame[17] = (uint8_t)(synth_total & 0xFF);
        // Clear fragment flags/offset
        synth_frame[20] = 0; synth_frame[21] = 0;
        // Copy reassembled payload
        memcpy(synth_frame + 34, reassembled, reassembled_len);
        kfree(reassembled);

        // Dispatch the reassembled datagram (recurse with synthetic frame)
        // Update payload pointer and len for dispatch below
        payload_len = reassembled_len;
        // We'll dispatch directly instead of recursing to avoid overhead
        switch (ip_header->protocol) {
            case IP_PROTO_ICMP:
                if (payload_len >= 8) {
                    struct icmp_header *icmp = (struct icmp_header*)(synth_frame + 34);
                    if (icmp->type == 8) {
                        icmp_send_echo_reply_to(ip_header->src_ip, synth_frame + 34, payload_len);
                    } else {
                        icmp_handle_packet(synth_frame + 34, payload_len);
                    }
                }
                break;
            case IP_PROTO_TCP:
                tcp_handle_packet(synth_frame, synth_frame_len);
                break;
            case IP_PROTO_UDP:
                udp_handle_packet(synth_frame, synth_frame_len);
                break;
            default:
                break;
        }
        kfree(synth_frame);
        return;
    }
    
    // Non-fragmented packet – dispatch to protocol handler directly
    const uint8_t *payload = frame + 14 + header_len;
    
    switch (ip_header->protocol) {
        case IP_PROTO_ICMP:
            DEBUG_PRINT(NET_IP, "IP: Handling ICMP packet, payload_len=%d\n", payload_len);
            // For ICMP echo requests, we need to pass the source IP for replies
            if (payload_len >= 8) { // Minimum ICMP header
                struct icmp_header *icmp = (struct icmp_header*)payload;
                DEBUG_PRINT(NET_IP, "IP: ICMP type=%d, code=%d\n", icmp->type, icmp->code);
                
                if (icmp->type == 8) { // Echo request
                    DEBUG_PRINT(NET_IP, "IP: Echo request detected from %d.%d.%d.%d\n",
                           ip_header->src_ip[0], ip_header->src_ip[1], 
                           ip_header->src_ip[2], ip_header->src_ip[3]);
                           
                    // Send echo reply directly to the source IP
                    icmp_send_echo_reply_to(ip_header->src_ip, payload, payload_len);
                } else {
                    icmp_handle_packet(payload, payload_len);
                }
            } else {
                DEBUG_PRINT(NET_IP, "IP: ICMP packet too short (%d bytes)\n", payload_len);
            }
            break;
        case IP_PROTO_TCP:
            DEBUG_PRINT(NET_IP, "IP: Handling TCP packet\n");
            tcp_handle_packet(frame, len);
            break;
        case IP_PROTO_UDP:
            DEBUG_PRINT(NET_IP, "IP: Handling UDP packet\n");
            udp_handle_packet(frame, len);
            break;
        default:
            /* IGMP (2) and anything else unhandled: drop quietly. */
            net_rx_count_ip_proto(ip_header->protocol);
            if (ethernet_get_verbose())
                printf("IP: Unknown protocol %d\n", ip_header->protocol);
            break;
    }
}

/**
 * Helper: send a single IP frame (Ethernet + IP header + payload).
 * Used internally by ip_send_packet and ip_send_fragment.
 */
static int ip_send_frame(const uint8_t *dst_mac, const uint8_t *our_ip, const uint8_t *dst_ip,
                         uint8_t protocol, uint16_t ip_id, uint16_t flags_frag_nbo,
                         const uint8_t *payload, uint16_t payload_len) {
    uint16_t total_len = 20 + payload_len;
    uint8_t *packet = kalloc(14 + total_len);
    if (!packet) {
        printf("IP: Failed to allocate packet buffer\n");
        return -1;
    }

    // Ethernet header
    memcpy(&packet[0], dst_mac, 6);
    uint8_t our_mac[6];
    net_get_mac_address(our_mac);
    memcpy(&packet[6], our_mac, 6);
    packet[12] = 0x08; packet[13] = 0x00; // EtherType: IPv4

    // IP header
    struct ipv4_header *ip_header = (struct ipv4_header*)&packet[14];
    ip_header->version_ihl = 0x45;
    ip_header->dscp_ecn = 0;
    ip_header->total_length = ((total_len & 0xFF) << 8) | ((total_len >> 8) & 0xFF);
    ip_header->identification = ((ip_id & 0xFF) << 8) | ((ip_id >> 8) & 0xFF);
    ip_header->flags_fragment = flags_frag_nbo;
    ip_header->ttl = 64;
    ip_header->protocol = protocol;
    ip_header->header_checksum = 0;
    memcpy(ip_header->src_ip, our_ip, 4);
    memcpy(ip_header->dst_ip, dst_ip, 4);

    // Checksum
    uint16_t checksum_host = ip_header_checksum_bytes((uint8_t*)ip_header, 20);
    ip_header->header_checksum = (uint16_t)(((checksum_host & 0xFF) << 8) | ((checksum_host >> 8) & 0xFF));

    // Copy payload
    if (payload && payload_len > 0) {
        memcpy(&packet[34], payload, payload_len);
    }

    int result = net_send_packet(packet, 14 + total_len);
    kfree(packet);
    return result;
}

int ip_send_packet(const uint8_t *dst_ip, uint8_t protocol, const uint8_t *payload, uint16_t payload_len) {
    
    DEBUG_PRINT(NET_IP, "IP: send_packet called for %d.%d.%d.%d, protocol=%d, payload_len=%d\n",
           dst_ip[0], dst_ip[1], dst_ip[2], dst_ip[3], protocol, payload_len);
    
    if (ip_is_local_dst(dst_ip))
        return ip_lo_enqueue(dst_ip, protocol, payload, payload_len);

    // Determine if destination is on-link; if not, route via gateway
    uint8_t our_ip[4], mask[4], gw[4];
    net_get_ip_address(our_ip);
    net_get_subnet_mask(mask);
    net_get_gateway(gw);

    uint8_t next_hop_ip[4];
    int is_broadcast_ip = (dst_ip[0]==255 && dst_ip[1]==255 && dst_ip[2]==255 && dst_ip[3]==255);
    int mask_zero = ((mask[0]|mask[1]|mask[2]|mask[3]) == 0);
    int gw_set = ((gw[0]|gw[1]|gw[2]|gw[3]) != 0);
    int off_subnet = ((our_ip[0] & mask[0]) != (dst_ip[0] & mask[0])) ||
                     ((our_ip[1] & mask[1]) != (dst_ip[1] & mask[1])) ||
                     ((our_ip[2] & mask[2]) != (dst_ip[2] & mask[2])) ||
                     ((our_ip[3] & mask[3]) != (dst_ip[3] & mask[3]));
    if (!is_broadcast_ip && (mask_zero || off_subnet) && gw_set) {
        memcpy(next_hop_ip, gw, 4);
        SERIAL_DEBUG_PRINT(NET_IP, "IP: Routing via GW %d.%d.%d.%d (mask=%d.%d.%d.%d mask_zero=%d off_sub=%d)\n",
                           gw[0],gw[1],gw[2],gw[3],
                           mask[0],mask[1],mask[2],mask[3], mask_zero, off_subnet);
    } else {
        memcpy(next_hop_ip, dst_ip, 4);
        SERIAL_DEBUG_PRINT(NET_IP, "IP: Direct route to %d.%d.%d.%d\n", dst_ip[0],dst_ip[1],dst_ip[2],dst_ip[3]);
    }

    // Resolve destination MAC address (skip ARP for broadcast).  On a miss
    // the datagram is parked until the ARP reply arrives instead of spinning.
    uint8_t dst_mac[6];
    if (is_broadcast_ip) {
        memset(dst_mac, 0xFF, 6);
    } else if (!arp_resolve(next_hop_ip, dst_mac)) {
        SERIAL_DEBUG_PRINT(NET_IP, "IP: ARP miss for %d.%d.%d.%d, queueing\n",
                           next_hop_ip[0], next_hop_ip[1], next_hop_ip[2], next_hop_ip[3]);
        return ip_arpq_enqueue(next_hop_ip, dst_ip, protocol, payload, payload_len);
    }
    
    DEBUG_PRINT(NET_IP, "IP: Resolved MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
           dst_mac[0], dst_mac[1], dst_mac[2], dst_mac[3], dst_mac[4], dst_mac[5]);

    // Max payload per fragment: (MTU - IP header) rounded down to 8-byte boundary
    uint16_t max_frag_payload = ((IP_MTU - 20) / 8) * 8;  // 1480 bytes

    uint16_t current_id = __atomic_fetch_add(&ip_id_counter, 1, __ATOMIC_RELAXED);

    if (payload_len <= (IP_MTU - 20)) {
        // No fragmentation needed – send with DF flag
        uint16_t flags_frag = IP_FLAG_DF;  // host order
        uint16_t flags_frag_nbo = ((flags_frag & 0xFF) << 8) | ((flags_frag >> 8) & 0xFF);
        return ip_send_frame(dst_mac, our_ip, dst_ip, protocol, current_id,
                             flags_frag_nbo, payload, payload_len);
    }

    // Fragmentation required
    DEBUG_PRINT(NET_IP, "IP: Fragmenting %u bytes into %u-byte chunks\n",
                payload_len, max_frag_payload);

    uint16_t offset = 0;
    while (offset < payload_len) {
        uint16_t chunk = payload_len - offset;
        int last_fragment = 1;

        if (chunk > max_frag_payload) {
            chunk = max_frag_payload;
            last_fragment = 0;
        }

        // Build flags + fragment offset in host byte order
        // Fragment offset is in 8-byte units
        uint16_t flags_frag = (offset / 8);
        if (!last_fragment) flags_frag |= IP_FLAG_MF;

        // Convert to network byte order
        uint16_t flags_frag_nbo = ((flags_frag & 0xFF) << 8) | ((flags_frag >> 8) & 0xFF);

        int result = ip_send_frame(dst_mac, our_ip, dst_ip, protocol, current_id,
                                   flags_frag_nbo, payload + offset, chunk);
        if (result != 0) {
            printf("IP: Failed to send fragment at offset %u\n", offset);
            return -1;
        }

        offset += chunk;
    }

    DEBUG_PRINT(NET_IP, "IP: Sent %u bytes in fragments\n", payload_len);
    return 0;
} 