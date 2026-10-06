#pragma once

#include <stdint.h>

// Process an ethernet frame - centralized entry point for the network stack
void ethernet_process_frame(const uint8_t *frame, uint32_t length);

// Control verbose Ethernet logging (0=quiet, 1=verbose).
void ethernet_set_verbose(int enabled);
int ethernet_get_verbose(void);