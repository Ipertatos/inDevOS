#pragma once

#include <stdint.h>
#include <stdbool.h>

/* Initialise the telnet server and begin listening on TCP port 23.
 * Call once after the network stack is up.                         */
void telnet_server_init(void);

/* Called from the main network polling loop to service pending IAC
 * negotiations and flush any buffered output.  Must be called often
 * (every network tick is fine).                                    */
void telnet_server_poll(void);
