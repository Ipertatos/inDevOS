#include "partscan.h"
#include "storage.h"
#include "string.h"

int partscan_starts(uint8_t id, uint32_t *starts, int max) {
    static uint8_t s0[512], s1[512], ents[512];
    int ns = 0;
    if (max <= 0) return 0;
    if (storage_read_sectors(id, 0, 1, s0) != 0) return 0;
    bool gpt = false;
    if (s0[510] == 0x55 && s0[511] == 0xAA) {
        for (int i = 0; i < 4; i++) {
            const uint8_t *e = s0 + 446 + i * 16;
            if (e[4] == 0xEE) gpt = true;
            uint32_t lba = *(const uint32_t *)(e + 8);
            if (e[4] && e[4] != 0xEE && lba && ns < max - 1) starts[ns++] = lba;
        }
    }
    if (gpt && storage_read_sectors(id, 1, 1, s1) == 0 && memcmp(s1, "EFI PART", 8) == 0) {
        uint64_t elba = *(uint64_t *)(s1 + 72);
        uint32_t cnt = *(uint32_t *)(s1 + 80), esz = *(uint32_t *)(s1 + 84);
        if (esz >= 128 && esz <= 512 && cnt > 0) {
            uint32_t per = 512u / esz;
            for (uint32_t k = 0; k < (cnt + per - 1) / per && k < 32 && ns < max - 1; k++) {
                if (storage_read_sectors(id, elba + k, 1, ents) != 0) break;
                for (uint32_t j = 0; j < per && ns < max - 1; j++) {
                    const uint8_t *e = ents + j * esz;
                    uint64_t first = *(const uint64_t *)(e + 32);
                    bool used = false;
                    for (int q = 0; q < 16; q++) if (e[q]) used = true;
                    if (used && first && first < 0xFFFFFFFFull) starts[ns++] = (uint32_t)first;
                }
            }
        }
    }
    starts[ns++] = 0;                                       /* partitionless volume */
    return ns;
}
