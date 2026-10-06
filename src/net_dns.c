#include "net_dns.h"
#include "net_udp.h"
#include "net_device.h"
#include "net_thread.h"
#include "mem.h"
#include "string.h"
#include "stdio.h"

// DNS A resolver with response caching

static uint16_t dns_id_counter = 1;

static uint16_t htons16(uint16_t x) { return (uint16_t)(((x & 0xFF) << 8) | (x >> 8)); }

// ---- DNS Response Cache -----------------------------------------------------
#define DNS_CACHE_SIZE 16          // Max number of cached entries
#define DNS_CACHE_MAX_NAME 128     // Max hostname length in cache
#define DNS_CACHE_DEFAULT_TTL 300  // 5 minutes default TTL if not specified

typedef struct {
    char     hostname[DNS_CACHE_MAX_NAME];
    uint8_t  ip[4];
    uint32_t ttl_s;           // TTL in seconds (from DNS response)
    uint64_t cached_at_ms;    // hpet_get_ticks when entry was stored
    uint8_t  valid;
} dns_cache_entry_t;

static dns_cache_entry_t dns_cache[DNS_CACHE_SIZE];
static int dns_cache_initialized = 0;

static void dns_cache_init(void) {
    if (dns_cache_initialized) return;
    memset(dns_cache, 0, sizeof(dns_cache));
    dns_cache_initialized = 1;
}

static int dns_str_eq_ci(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
        a++; b++;
    }
    return (*a == *b);
}

static uint64_t dns_now_ms(void) {
    extern uint64_t hpet_get_ticks(void);
    return hpet_get_ticks();
}

/**
 * Look up a hostname in the cache. Returns 1 if found (and not expired), 0 otherwise.
 */
static int dns_cache_lookup(const char *hostname, uint8_t out_ip[4]) {
    dns_cache_init();
    uint64_t now = dns_now_ms();

    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        if (!dns_cache[i].valid) continue;
        if (!dns_str_eq_ci(dns_cache[i].hostname, hostname)) continue;

        // Check TTL
        uint64_t elapsed_ms = now - dns_cache[i].cached_at_ms;
        uint64_t ttl_ms = (uint64_t)dns_cache[i].ttl_s * 1000;
        if (elapsed_ms >= ttl_ms) {
            // Expired – invalidate
            dns_cache[i].valid = 0;
            return 0;
        }

        memcpy(out_ip, dns_cache[i].ip, 4);
        printf("DNS: Cache hit for %s -> %d.%d.%d.%d (TTL %u s remaining)\n",
               hostname, out_ip[0], out_ip[1], out_ip[2], out_ip[3],
               (uint32_t)((ttl_ms - elapsed_ms) / 1000));
        return 1;
    }
    return 0;
}

/**
 * Insert or update a cache entry.
 */
static void dns_cache_insert(const char *hostname, const uint8_t ip[4], uint32_t ttl_s) {
    dns_cache_init();

    if (ttl_s == 0) ttl_s = DNS_CACHE_DEFAULT_TTL;

    // Check if already cached (update in place)
    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        if (dns_cache[i].valid && dns_str_eq_ci(dns_cache[i].hostname, hostname)) {
            memcpy(dns_cache[i].ip, ip, 4);
            dns_cache[i].ttl_s = ttl_s;
            dns_cache[i].cached_at_ms = dns_now_ms();
            return;
        }
    }

    // Find a free slot or evict the oldest/expired entry
    int slot = -1;
    uint64_t oldest_time = 0xFFFFFFFFFFFFFFFFULL;

    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        if (!dns_cache[i].valid) {
            slot = i;
            break;
        }
        // Check if expired
        uint64_t elapsed = dns_now_ms() - dns_cache[i].cached_at_ms;
        if (elapsed >= (uint64_t)dns_cache[i].ttl_s * 1000) {
            slot = i;
            break;
        }
        // Track oldest for LRU-like eviction
        if (dns_cache[i].cached_at_ms < oldest_time) {
            oldest_time = dns_cache[i].cached_at_ms;
            slot = i;
        }
    }

    if (slot < 0) slot = 0;  // Fallback (shouldn't happen)

    // Copy hostname safely
    size_t name_len = 0;
    while (hostname[name_len] && name_len < DNS_CACHE_MAX_NAME - 1) name_len++;
    memcpy(dns_cache[slot].hostname, hostname, name_len);
    dns_cache[slot].hostname[name_len] = '\0';

    memcpy(dns_cache[slot].ip, ip, 4);
    dns_cache[slot].ttl_s = ttl_s;
    dns_cache[slot].cached_at_ms = dns_now_ms();
    dns_cache[slot].valid = 1;

    printf("DNS: Cached %s -> %d.%d.%d.%d (TTL %u s)\n",
           hostname, ip[0], ip[1], ip[2], ip[3], ttl_s);
}

void dns_cache_flush(void) {
    memset(dns_cache, 0, sizeof(dns_cache));
    printf("DNS: Cache flushed\n");
}

int dns_cache_get_count(void) {
    dns_cache_init();
    int count = 0;
    uint64_t now = dns_now_ms();
    for (int i = 0; i < DNS_CACHE_SIZE; i++) {
        if (!dns_cache[i].valid) continue;
        uint64_t elapsed = now - dns_cache[i].cached_at_ms;
        if (elapsed < (uint64_t)dns_cache[i].ttl_s * 1000) count++;
    }
    return count;
}

static volatile struct {
    uint16_t pending_id;
    int have_answer;
    uint8_t ip[4];
    uint32_t ttl;  // TTL from the answer record
} dns_state = {0};

static void dns_udp_handler(const uint8_t *src_ip, uint16_t src_port, const uint8_t *payload, uint16_t payload_len) {
    (void)src_ip; (void)src_port;
    if (payload_len < 12) return;
    uint16_t id = ((uint16_t)payload[0] << 8) | payload[1];
    if (id != dns_state.pending_id) return;
    // Parse header
    uint16_t flags = ((uint16_t)payload[2] << 8) | payload[3];
    uint16_t qd = ((uint16_t)payload[4] << 8) | payload[5];
    uint16_t an = ((uint16_t)payload[6] << 8) | payload[7];
    if ((flags & 0x8000) == 0) return; // not a response
    // Skip question section
    size_t i = 12;
    for (uint16_t qi = 0; qi < qd && i < payload_len; qi++) {
        while (i < payload_len && payload[i] != 0) { i += (size_t)payload[i] + 1; }
        i++; // zero label
        i += 4; // QTYPE+QCLASS
    }
    // Read first answer A record
    for (uint16_t ai = 0; ai < an && i + 10 <= payload_len; ai++) {
        // Name (could be compressed); skip name: if pointer, 2 bytes, else walk labels
        if ((payload[i] & 0xC0) == 0xC0) { i += 2; } else { while (i < payload_len && payload[i] != 0) { i += (size_t)payload[i] + 1; } i++; }
        if (i + 10 > payload_len) break;
        uint16_t type = ((uint16_t)payload[i] << 8) | payload[i+1];
        uint16_t _class = ((uint16_t)payload[i+2] << 8) | payload[i+3];
        uint32_t ttl = ((uint32_t)payload[i+4] << 24) | ((uint32_t)payload[i+5] << 16) |
                       ((uint32_t)payload[i+6] << 8)  | (uint32_t)payload[i+7];
        uint16_t rdlen = ((uint16_t)payload[i+8] << 8) | payload[i+9];
        i += 10;
        if (i + rdlen > payload_len) break;
        if (type == 1 && _class == 1 && rdlen == 4) { // A IN
            memcpy((void*)dns_state.ip, &payload[i], 4);
            dns_state.ttl = ttl;
            dns_state.have_answer = 1;
            return;
        }
        i += rdlen;
    }
}

int dns_resolve_a(const char *hostname, uint8_t out_ip[4]) {
    // Check cache first
    if (dns_cache_lookup(hostname, out_ip)) {
        return 0;  // Cache hit
    }

    uint8_t dns_ip[4];
    net_get_dns_server(dns_ip);
    if ((dns_ip[0]|dns_ip[1]|dns_ip[2]|dns_ip[3]) == 0) {
        printf("DNS: No DNS server configured\n");
        return -1;
    }

    // Build DNS query
    uint8_t query[512];
    memset(query, 0, sizeof(query));
    uint16_t id = dns_id_counter++;
    query[0] = (uint8_t)(id >> 8); query[1] = (uint8_t)(id & 0xFF);
    query[2] = 0x01; // RD
    query[5] = 0x01; // QDCOUNT=1

    // QNAME
    const char *h = hostname;
    size_t qi = 12;
    while (*h && qi + 1 < sizeof(query)) {
        const char *label = h;
        // Find next '.' manually (no libc strchr)
        size_t labellen = 0;
        while (h[labellen] && h[labellen] != '.') { labellen++; }
        if (labellen > 63 || qi + 1 + labellen >= sizeof(query)) return -1;
        query[qi++] = (uint8_t)labellen;
        memcpy(&query[qi], label, labellen);
        qi += labellen;
        if (h[labellen] == '.') { h += labellen + 1; }
        else { h += labellen; break; }
    }
    if (qi >= sizeof(query)) return -1;
    query[qi++] = 0; // end of QNAME
    // QTYPE=A, QCLASS=IN
    if (qi + 4 > sizeof(query)) return -1;
    query[qi++] = 0; query[qi++] = 1; // A
    query[qi++] = 0; query[qi++] = 1; // IN

    // Register UDP handler on port 40000 and send
    udp_register_handler(40000, dns_udp_handler);
    dns_state.pending_id = id;
    dns_state.have_answer = 0;
    dns_state.ttl = 0;

    if (udp_send(dns_ip, 40000, 53, query, (uint16_t)qi) != 0) {
        printf("DNS: Failed to send query\n");
        udp_unregister_handler(40000);
        return -1;
    }

    /* Poll the NIC while waiting for the reply.  Without this, the reply
     * packet sits in the RX ring indefinitely: the UDP handler is registered
     * but it is only called by ethernet_process_frame(), which nobody invokes
     * unless we drive the receive loop here. */
    extern int net_receive_packet(uint8_t *buffer, uint32_t max_length);
    extern void ethernet_process_frame(const uint8_t *frame, uint32_t length);
    extern uint64_t hpet_get_ticks(void);

    uint8_t rx_buf[2048];
    uint64_t start = hpet_get_ticks();
    while (hpet_get_ticks() - start < 3000) {
        int pkt_len = net_receive_packet(rx_buf, sizeof(rx_buf));
        if (pkt_len > 0) ethernet_process_frame(rx_buf, (uint32_t)pkt_len);

        if (dns_state.have_answer) {
            memcpy(out_ip, (const void*)dns_state.ip, 4);
            udp_unregister_handler(40000);
            printf("DNS: Reply received\n");

            // Cache the result with TTL from the response
            dns_cache_insert(hostname, out_ip, dns_state.ttl);
            return 0;
        }
        if (pkt_len <= 0) net_wait_idle();
    }
    udp_unregister_handler(40000);
    printf("DNS: Timeout waiting for reply\n");
    return -1;
}


