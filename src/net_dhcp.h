#pragma once
#include <stdint.h>

// Initial DHCP acquisition (DISCOVER-OFFER-REQUEST-ACK)
void dhcp_start(void);

// Lease renewal – call periodically (e.g. every 30s) from a timer or network task.
// Sends a unicast DHCPREQUEST at T1, broadcast at T2, and full re-acquire at expiry.
// Returns: 0 = no action needed, 1 = renewed/rebound, -1 = lease lost
int dhcp_renew_check(void);

// Query lease state
uint32_t dhcp_get_lease_remaining_s(void);   // Seconds until lease expires (0 = expired)
int dhcp_has_lease(void);                     // 1 if we have a valid lease






















































