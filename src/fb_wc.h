#ifndef FB_WC_H
#define FB_WC_H
#include <stdint.h>
#include <stdbool.h>
void fb_wc_pat_init_cpu(void);   /* every CPU: ensure PAT[5] = WC */
void fb_wc_init(void);           /* BSP, after vmm + fb: map fb WC, log, bench */
bool fb_wc_pat_ready(void);
bool fb_wc_active(void);
uint64_t fb_wc_pte_flags(void);  /* 4K PTE bits for a WC (or UC fallback) mapping */
#endif
