#ifndef PARTSCAN_H
#define PARTSCAN_H
#include "types.h"
/* Start LBAs of the partitions on @dev (MBR primaries, or GPT entries when
 * the MBR is protective), followed by 0 for a partitionless volume.
 * Returns the number of LBAs stored (<= max). */
int partscan_starts(uint8_t dev, uint32_t *starts, int max);
#endif
