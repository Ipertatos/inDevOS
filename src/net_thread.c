#include "net_thread.h"
#include "net_device.h"
#include "net_arp.h"
#include "net_ethernet.h"
#include "net_ip.h"
#include "hpet.h"
#include "serial.h"
#include "utils.h"
#include "stdio.h"

/*
 * Network RX service: IRQ-gated packet processing from the kernel main loop.
 *
 * NIC interrupt handlers call net_thread_notify_rx() to flag pending
 * packets, and the kernel main loop calls net_rx_service() each
 * iteration.  The flag also wakes the CPU out of its idle HLT, giving
 * interrupt-latency RX response without busy-polling the NIC every
 * iteration.  (The timer/IPI path can now resume kernel threads, but RX
 * still runs from the main loop so it stays serialized with the stack.)
 *
 * A slow fallback poll covers NICs whose IRQ path is missing or misrouted.
 */
#define NET_RX_BATCH_SIZE       8
#define NET_FALLBACK_POLL_MS    50
#define NET_ARP_CLEANUP_MS      10000
#define NET_ARPQ_TICK_MS        50

static volatile int net_rx_event = 0;
static uint64_t net_rx_packets_total = 0;

/* Called from NIC interrupt handlers: flag pending RX work for the main
 * loop.  The IRQ itself already wakes the CPU from HLT. */
void net_thread_notify_rx(void) {
    net_rx_event = 1;
}

/* Called from the kernel main loop.  Drains a batch of received packets
 * when the RX flag is set (or on the slow fallback poll), and performs
 * periodic ARP cache cleanup.  Returns the number of packets processed. */
int net_rx_service(void) {
    static uint64_t last_fallback_poll = 0;
    static uint64_t last_arp_cleanup = 0;

    uint64_t now = hpet_get_ticks();
    int processed = 0;

    if (net_rx_event || now - last_fallback_poll >= NET_FALLBACK_POLL_MS) {
        net_rx_event = 0;
        last_fallback_poll = now;

        for (int i = 0; i < NET_RX_BATCH_SIZE; i++) {
            uint8_t packet_buffer[2048];
            int length = net_receive_packet(packet_buffer, sizeof(packet_buffer));
            if (length <= 0)
                break;
            ethernet_process_frame(packet_buffer, length);
            processed++;
        }
        net_rx_packets_total += (uint64_t)processed;
    }

    /* Local (127.x / own address) datagrams queued by ip_send_packet(). */
    if (ip_loopback_pending())
        processed += ip_loopback_drain();

    static uint64_t last_arpq_tick = 0;
    if (now - last_arpq_tick >= NET_ARPQ_TICK_MS) {
        ip_arpq_tick();
        last_arpq_tick = now;
    }

    if (now - last_arp_cleanup > NET_ARP_CLEANUP_MS) {
        arp_cleanup_expired();
        last_arp_cleanup = now;
    }

    return processed;
}

/* For synchronous waits (DNS/DHCP/ping): keep ARP retries going and sleep
 * until the next interrupt (NIC RX or the 1 ms timer) instead of spinning. */
void net_wait_idle(void) {
    static uint64_t last_tick = 0;
    uint64_t now = hpet_get_ticks();
    if (now - last_tick >= NET_ARPQ_TICK_MS) {
        ip_arpq_tick();
        last_tick = now;
    }
    __asm__ volatile("sti; hlt");
}

void network_thread_status(void) {
    printf("Network RX Service:\n");
    printf("  Mode: IRQ-gated main-loop service\n");
    printf("  RX event pending: %s\n", net_rx_event ? "yes" : "no");
    printf("  Packets processed: %llu\n",
           (unsigned long long)net_rx_packets_total);
}
