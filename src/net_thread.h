#pragma once

/* Network RX service: IRQ-gated packet processing from the kernel main loop.
 * NIC ISRs flag pending RX with net_thread_notify_rx(); the main loop drains
 * packets via net_rx_service(). */

/* Drain pending RX packets + periodic ARP cleanup; returns packets processed. */
int net_rx_service(void);

void network_thread_status(void);

/* Called from NIC IRQ handlers to flag pending RX for the main loop. */
void net_thread_notify_rx(void);

/* One iteration of a synchronous network wait: drives ARP retries, then
 * sleeps until the next interrupt.  Enables interrupts. */
void net_wait_idle(void);
