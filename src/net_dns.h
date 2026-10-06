#pragma once
#include <stdint.h>

// Resolve hostname to IPv4 address (checks cache first, then queries server)
int dns_resolve_a(const char *hostname, uint8_t out_ip[4]);

// Cache management
void dns_cache_flush(void);              // Clear all cached entries
int  dns_cache_get_count(void);          // Number of valid entries in cache






















































