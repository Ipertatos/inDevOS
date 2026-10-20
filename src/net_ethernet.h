#pragma once

#include <stdint.h>

// Process an ethernet frame - centralized entry point for the network stack
void ethernet_process_frame(const uint8_t *frame, uint32_t length);

// Control verbose Ethernet logging (0=quiet, 1=verbose).
void ethernet_set_verbose(int enabled);
int ethernet_get_verbose(void);

/* Receive-path logging.  The RX path must never write to the console by
 * default (it interleaves with the shell's input line and, on busy LANs,
 * stalls the main loop).  NET_RX_LOG goes to the console only in
 * 'netverbose on' mode; otherwise to serial, at most one line per second. */
#include "serial.h"
int net_rx_log_allow(void);   /* 2 = console (verbose), 1 = serial, 0 = suppressed */
#define NET_RX_LOG(...) do { int _nrl = net_rx_log_allow(); \
    if (_nrl == 2) printf(__VA_ARGS__); else if (_nrl == 1) serial_printf(__VA_ARGS__); } while (0)

/* Silent-drop counters (shown by 'net' and 'usbnet'). */
void net_rx_count_unknown_l2(uint16_t ethertype);
void net_rx_count_ip_proto(uint8_t proto);   /* IGMP or other unhandled IP protocol */
void net_rx_count_malformed(void);
void net_rx_print_drop_stats(const char *indent);
