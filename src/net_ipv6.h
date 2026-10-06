/**
 * @file net_ipv6.h
 * @brief IPv6 protocol implementation (basic)
 *
 * Provides IPv6 packet handling, ICMPv6 (neighbor discovery, echo),
 * and SLAAC (Stateless Address Autoconfiguration).
 */

#ifndef NET_IPV6_H
#define NET_IPV6_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ============================================================================
 * IPv6 Address
 * ============================================================================ */

/** IPv6 address (128 bits) */
typedef struct {
    uint8_t addr[16];
} ipv6_addr_t;

/** Well-known addresses */
extern const ipv6_addr_t IPV6_ADDR_UNSPECIFIED;    /**< :: */
extern const ipv6_addr_t IPV6_ADDR_LOOPBACK;       /**< ::1 */
extern const ipv6_addr_t IPV6_ADDR_ALL_NODES;      /**< ff02::1 */
extern const ipv6_addr_t IPV6_ADDR_ALL_ROUTERS;    /**< ff02::2 */

/* ============================================================================
 * IPv6 Header (RFC 2460, 40 bytes fixed)
 * ============================================================================ */

typedef struct __attribute__((packed)) {
    uint32_t version_tc_flow;       /**< Version(4) + Traffic Class(8) + Flow Label(20) */
    uint16_t payload_length;        /**< Payload length (network byte order) */
    uint8_t  next_header;           /**< Next header protocol */
    uint8_t  hop_limit;             /**< Hop limit (like TTL) */
    ipv6_addr_t src;                /**< Source address */
    ipv6_addr_t dst;                /**< Destination address */
} ipv6_header_t;

_Static_assert(sizeof(ipv6_header_t) == 40, "IPv6 header must be 40 bytes");

/* ============================================================================
 * ICMPv6 (RFC 4443)
 * ============================================================================ */

/** ICMPv6 header */
typedef struct __attribute__((packed)) {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
} icmpv6_header_t;

/** ICMPv6 types */
#define ICMPV6_ECHO_REQUEST        128
#define ICMPV6_ECHO_REPLY          129
#define ICMPV6_ROUTER_SOLICITATION  133
#define ICMPV6_ROUTER_ADVERTISEMENT 134
#define ICMPV6_NEIGHBOR_SOLICITATION 135
#define ICMPV6_NEIGHBOR_ADVERTISEMENT 136

/* Neighbor Discovery option types */
#define ND_OPT_SOURCE_LINK_ADDR    1
#define ND_OPT_TARGET_LINK_ADDR    2
#define ND_OPT_PREFIX_INFO         3
#define ND_OPT_MTU                 5

/** Neighbor Solicitation message body */
typedef struct __attribute__((packed)) {
    uint32_t reserved;
    ipv6_addr_t target;             /**< Target IPv6 address */
} nd_neighbor_solicit_t;

/** Neighbor Advertisement message body */
typedef struct __attribute__((packed)) {
    uint32_t flags;                 /**< R(1) S(1) O(1) Reserved(29) */
    ipv6_addr_t target;
} nd_neighbor_advert_t;

/** Router Solicitation body */
typedef struct __attribute__((packed)) {
    uint32_t reserved;
} nd_router_solicit_t;

/** Router Advertisement body */
typedef struct __attribute__((packed)) {
    uint8_t  cur_hop_limit;
    uint8_t  flags;                 /**< M(1) O(1) Reserved(6) */
    uint16_t router_lifetime;
    uint32_t reachable_time;
    uint32_t retrans_timer;
} nd_router_advert_t;

/** Prefix Information option */
typedef struct __attribute__((packed)) {
    uint8_t  type;                  /**< ND_OPT_PREFIX_INFO (3) */
    uint8_t  length;                /**< 4 (in 8-byte units) */
    uint8_t  prefix_length;         /**< Prefix length in bits */
    uint8_t  flags;                 /**< L(1) A(1) Reserved(6) */
    uint32_t valid_lifetime;
    uint32_t preferred_lifetime;
    uint32_t reserved2;
    ipv6_addr_t prefix;             /**< Network prefix */
} nd_prefix_info_t;

/** ND Source/Target Link-Layer Address option */
typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t length;                 /**< 1 (in 8-byte units for Ethernet) */
    uint8_t addr[6];               /**< MAC address */
} nd_link_layer_addr_t;

/* Neighbor Advertisement flags */
#define ND_NA_FLAG_ROUTER    (1 << 31)
#define ND_NA_FLAG_SOLICITED (1 << 30)
#define ND_NA_FLAG_OVERRIDE  (1 << 29)

/* ============================================================================
 * Neighbor Cache
 * ============================================================================ */

#define IPV6_NEIGHBOR_CACHE_SIZE 32

typedef enum {
    NDP_STATE_INCOMPLETE = 0,
    NDP_STATE_REACHABLE,
    NDP_STATE_STALE,
    NDP_STATE_DELAY,
    NDP_STATE_PROBE
} ndp_state_t;

typedef struct {
    ipv6_addr_t ip;
    uint8_t mac[6];
    ndp_state_t state;
    uint32_t timestamp;             /**< Last update time */
} ndp_neighbor_entry_t;

/* ============================================================================
 * IPv6 Interface State
 * ============================================================================ */

#define IPV6_MAX_ADDRESSES 4

typedef struct {
    ipv6_addr_t address;
    uint8_t prefix_length;
    bool valid;
} ipv6_addr_entry_t;

typedef struct {
    bool enabled;
    ipv6_addr_entry_t addresses[IPV6_MAX_ADDRESSES];
    ipv6_addr_t link_local;         /**< Link-local address (fe80::) */
    ipv6_addr_t gateway;            /**< Default gateway */
    uint8_t mac[6];                 /**< Interface MAC address */
    uint8_t hop_limit;              /**< Default hop limit (64) */
    ndp_neighbor_entry_t neighbor_cache[IPV6_NEIGHBOR_CACHE_SIZE];
    bool slaac_complete;            /**< SLAAC configuration done */
    bool dad_complete;              /**< Duplicate Address Detection done */
} ipv6_interface_t;

/* ============================================================================
 * Public API
 * ============================================================================ */

/**
 * @brief Initialize IPv6 stack
 * @return 0 on success
 */
int ipv6_init(void);

/**
 * @brief Handle an incoming IPv6 packet
 * @param frame   Raw frame data (starting from IPv6 header)
 * @param len     Frame length
 */
void ipv6_handle_packet(const uint8_t *frame, uint32_t len);

/**
 * @brief Send an IPv6 packet
 * @param dst         Destination IPv6 address
 * @param next_header Protocol number
 * @param payload     Payload data
 * @param payload_len Payload length
 * @return 0 on success, -1 on error
 */
int ipv6_send_packet(const ipv6_addr_t *dst, uint8_t next_header,
                     const uint8_t *payload, uint16_t payload_len);

/**
 * @brief Send ICMPv6 Echo Request (ping6)
 * @param dst Target address
 * @return 0 on success
 */
int ipv6_ping(const ipv6_addr_t *dst);

/**
 * @brief Start SLAAC (Stateless Address Autoconfiguration)
 *
 * Generates a link-local address from the MAC address (EUI-64),
 * performs DAD, then sends Router Solicitation.
 */
void ipv6_start_slaac(void);

/**
 * @brief Perform Neighbor Discovery for a target address
 * @param target Target IPv6 address to resolve
 * @return 0 if entry already cached, 1 if solicitation sent
 */
int ipv6_neighbor_resolve(const ipv6_addr_t *target);

/**
 * @brief Lookup a neighbor's MAC address in the cache
 * @param ip    IPv6 address
 * @param mac   Output MAC address (6 bytes)
 * @return 0 on success (found), -1 if not cached
 */
int ipv6_neighbor_lookup(const ipv6_addr_t *ip, uint8_t *mac);

/**
 * @brief Get the link-local address
 * @return Pointer to link-local address
 */
const ipv6_addr_t* ipv6_get_link_local(void);

/**
 * @brief Get the first global address (if SLAAC is complete)
 * @return Pointer to address, or NULL
 */
const ipv6_addr_t* ipv6_get_global_address(void);

/**
 * @brief Print IPv6 status information
 */
void ipv6_print_status(void);

/**
 * @brief Check if IPv6 is enabled and configured
 * @return true if IPv6 is operational
 */
bool ipv6_is_enabled(void);

/* Address utility functions */
bool ipv6_addr_is_unspecified(const ipv6_addr_t *addr);
bool ipv6_addr_is_loopback(const ipv6_addr_t *addr);
bool ipv6_addr_is_multicast(const ipv6_addr_t *addr);
bool ipv6_addr_is_link_local(const ipv6_addr_t *addr);
bool ipv6_addr_equal(const ipv6_addr_t *a, const ipv6_addr_t *b);
void ipv6_addr_to_string(const ipv6_addr_t *addr, char *buf, size_t buflen);
int  ipv6_addr_from_string(const char *str, ipv6_addr_t *addr);

/** Generate solicited-node multicast address for a unicast address */
void ipv6_solicited_node_addr(const ipv6_addr_t *unicast, ipv6_addr_t *solicited);

/** Generate EUI-64 interface ID from MAC address */
void ipv6_eui64_from_mac(const uint8_t *mac, uint8_t *iid);

#endif /* NET_IPV6_H */
