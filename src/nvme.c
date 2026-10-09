/*
 * nvme.c - NVMe (PCI class 01:08:02) block driver, polled mode.
 *
 * One controller, one namespace (the first active one), one admin queue
 * pair and one I/O queue pair. Every command is submitted and then polled
 * to completion (no interrupts: INTx is disabled and INTMS masks vectors),
 * with a timeout on every wait. Data moves through a driver-owned,
 * physically contiguous bounce buffer, so callers can pass any kernel or
 * kernel-image buffer and the PRP entries are always page-aligned physical
 * addresses (PRP1, PRP2 or a PRP list for > 2 pages).
 *
 * The block layer works in 512-byte sectors. Namespaces formatted with
 * 4096-byte LBAs are exposed as 512-byte sectors too: reads pick the needed
 * part of each 4K block and unaligned writes do read-modify-write.
 */
#include "pci.h"
#include "nvme.h"
#include "mem.h"
#include "utils.h"
#include "serial.h"
#include "atomic.h"
#include "ktime.h"
#include "string.h"
#include "stdio.h"

#define NVME_AQ_DEPTH       32u         /* admin queue entries */
#define NVME_IOQ_DEPTH_MAX  64u         /* I/O queue entries (<= CAP.MQES+1) */
#define NVME_BOUNCE_BYTES   (256u * 1024u)  /* max bytes per I/O command */
#define NVME_ADMIN_TIMEOUT_MS 5000u
#define NVME_IO_TIMEOUT_MS    10000u

typedef struct {
    volatile nvme_command_t *sq;
    volatile nvme_completion_t *cq;
    uint64_t sq_phys, cq_phys;
    uint16_t qid, depth, tail, head, cid;
    uint8_t phase;
} nvme_queue_t;

typedef struct {
    struct pci_device *pci_dev;
    uint64_t bar_phys;
    uint64_t bar_size;
    uint64_t regs;              /* virtual address of BAR0 */
    uint64_t cap;
    uint32_t dstrd_bytes;       /* doorbell stride in bytes */
    uint32_t timeout_ms;        /* CAP.TO */
    uint32_t version;
    nvme_queue_t admin, io;
    uint8_t *idbuf;             /* 4 KiB identify buffer */
    uint64_t idbuf_phys;
    uint64_t *prp_list;         /* one page of PRP entries */
    uint64_t prp_list_phys;
    uint8_t *bounce;
    uint64_t bounce_phys;
    uint32_t max_xfer;          /* bytes per command (bounce, MDTS) */
    uint32_t nsid;
    uint32_t lba_shift;         /* log2(LBA size), 9 or 12 typical */
    uint64_t ns_blocks;         /* namespace size in LBAs */
    uint64_t sectors512;        /* namespace size in 512-byte sectors */
    bool vwc;                   /* volatile write cache present: flush matters */
    char serial[21], model[41], fw[9];
    bool initialized;
    bool dead;                  /* a command timed out or CSTS.CFS: refuse I/O */
    int index;                  /* nvmeN */
    uint8_t storage_id;         /* block-layer device id, 0xFF = none */
    spinlock_t lock;
} nvme_controller_t;

#define NVME_MAX_CTRL 4

static nvme_controller_t nvme_ctrls[NVME_MAX_CTRL];
static int nvme_ctrl_count;

/* ------------------------------------------------------------------------
 * Register access. 64-bit registers are accessed as two dwords (low first),
 * which every controller must accept.
 * ------------------------------------------------------------------------ */
static inline uint32_t nvme_rd32(nvme_controller_t *n, uint32_t off) {
    return *(volatile uint32_t *)(n->regs + off);
}
static inline void nvme_wr32(nvme_controller_t *n, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(n->regs + off) = v;
}
static inline uint64_t nvme_rd64(nvme_controller_t *n, uint32_t off) {
    uint64_t lo = nvme_rd32(n, off);
    uint64_t hi = nvme_rd32(n, off + 4);
    return lo | (hi << 32);
}
static inline void nvme_wr64(nvme_controller_t *n, uint32_t off, uint64_t v) {
    nvme_wr32(n, off, (uint32_t)v);
    nvme_wr32(n, off + 4, (uint32_t)(v >> 32));
}
static inline uint32_t nvme_sq_db(nvme_controller_t *n, uint16_t qid) {
    return NVME_REG_DBS + (2u * qid) * n->dstrd_bytes;
}
static inline uint32_t nvme_cq_db(nvme_controller_t *n, uint16_t qid) {
    return NVME_REG_DBS + (2u * qid + 1u) * n->dstrd_bytes;
}

/* ------------------------------------------------------------------------
 * BAR0 mapping. The BAR may be anywhere in the 64-bit space. Map it at
 * HHDM+phys as uncached (PCD|PWT) 4 KiB pages. Pages the bootloader already
 * maps there (HHDM huge pages below 4 GiB, which the firmware MTRRs make UC
 * for the PCI hole) are left alone.
 * ------------------------------------------------------------------------ */
#define NVME_PTE_PS (1ull << 7)

static int nvme_map_page_uc(uint64_t va, uint64_t pa) {
    uint64_t *tbl = vmm_get_kernel_pml4();
    const unsigned idx[3] = { PML4_INDEX(va), PDP_INDEX(va), PD_INDEX(va) };
    if (!tbl) return -1;
    for (int lvl = 0; lvl < 3; lvl++) {
        uint64_t e = tbl[idx[lvl]];
        if (e & VMM_PRESENT) {
            if (lvl > 0 && (e & NVME_PTE_PS))
                return 0;               /* inside an existing huge mapping */
            tbl = (uint64_t *)phys_to_virt(e & PTE_ADDR_MASK);
            continue;
        }
        uint64_t f = vmm_alloc_frame();
        if (!f) return -1;
        if (lvl == 0)
            serial_printf("NVMe: new PML4 slot %u for BAR mapping\n", idx[0]);
        tbl[idx[lvl]] = f | VMM_PRESENT | VMM_WRITABLE;
        tbl = (uint64_t *)phys_to_virt(f);
    }
    uint64_t *pte = &tbl[PT_INDEX(va)];
    if (*pte & VMM_PRESENT)
        return 0;
    *pte = (pa & PTE_ADDR_MASK) | VMM_PRESENT | VMM_WRITABLE |
           VMM_NOCACHE | VMM_WRITETHRU | VMM_NX;
    __asm__ volatile("invlpg (%0)" :: "r"(va) : "memory");
    return 0;
}

static uint64_t nvme_map_bar(uint64_t phys, uint64_t size) {
    uint64_t start = phys & ~0xFFFull;
    uint64_t end = (phys + size + 0xFFFull) & ~0xFFFull;
    for (uint64_t p = start; p < end; p += PAGE_SIZE) {
        if (nvme_map_page_uc(p + vmm_higher_half_offset, p) != 0)
            return 0;
    }
    return phys + vmm_higher_half_offset;
}

/* ------------------------------------------------------------------------
 * DMA memory: kalloc of whole pages comes from the page allocator, which is
 * page-aligned and physically contiguous (HHDM is a linear map).
 * ------------------------------------------------------------------------ */
static void *nvme_dma_alloc(uint32_t bytes, uint64_t *phys) {
    uint32_t sz = (bytes + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (sz < PAGE_SIZE) sz = PAGE_SIZE;
    void *p = kalloc(sz);
    if (!p) return NULL;
    uint64_t pa = virt_to_phys(p);
    if (pa & (PAGE_SIZE - 1)) {
        printf("NVMe: DMA buffer not page aligned (0x%llx)\n", (unsigned long long)pa);
        kfree(p);
        return NULL;
    }
    memset(p, 0, sz);
    *phys = pa;
    return p;
}

/* ------------------------------------------------------------------------
 * Waits (all bounded)
 * ------------------------------------------------------------------------ */
static int nvme_wait_rdy(nvme_controller_t *n, bool want, uint32_t timeout_ms) {
    uint64_t deadline = ktime_deadline_ms(timeout_ms);
    uint64_t spins = 0;
    for (;;) {
        uint32_t csts = nvme_rd32(n, NVME_REG_CSTS);
        if (csts == 0xFFFFFFFFu)
            return -2;                  /* device gone / not decoding */
        if (((csts & NVME_CSTS_RDY) != 0) == want)
            return 0;
        if (want && (csts & NVME_CSTS_CFS))
            return -3;
        if (ktime_expired(deadline))
            return -1;
        /* Clock not running yet: fall back to a generous spin bound. */
        if (++spins > (uint64_t)timeout_ms * 200000ull)
            return -1;
        __asm__ volatile("pause");
    }
}

static int nvme_submit_wait(nvme_controller_t *n, nvme_queue_t *q, nvme_command_t *cmd,
                            uint32_t timeout_ms, uint32_t *result) {
    if (n->dead)
        return -1;
    uint16_t cid = q->cid++;
    if (q->cid == 0xFFFF) q->cid = 0;
    cmd->cdw0 = (cmd->cdw0 & 0xFFFFu) | ((uint32_t)cid << 16);
    volatile uint32_t *dst = (volatile uint32_t *)(uintptr_t)&q->sq[q->tail];
    uint32_t src[16];
    memcpy(src, cmd, sizeof(src));
    for (int i = 0; i < 16; i++)
        dst[i] = src[i];
    q->tail = (uint16_t)((q->tail + 1u) % q->depth);
    __asm__ volatile("mfence" ::: "memory");
    nvme_wr32(n, nvme_sq_db(n, q->qid), q->tail);

    uint64_t deadline = ktime_deadline_ms(timeout_ms);
    uint64_t spins = 0;
    for (;;) {
        volatile nvme_completion_t *c = &q->cq[q->head];
        uint16_t st = c->status;
        if ((st & 1u) == q->phase) {
            __asm__ volatile("lfence" ::: "memory");
            uint16_t ccid = c->cid;
            uint32_t res = c->result;
            q->head = (uint16_t)(q->head + 1u);
            if (q->head == q->depth) {
                q->head = 0;
                q->phase ^= 1u;
            }
            nvme_wr32(n, nvme_cq_db(n, q->qid), q->head);
            if (ccid != cid) {
                serial_printf("nvme%d: q%u stale completion cid=%u (want %u)\n", n->index,
                              q->qid, ccid, cid);
                continue;
            }
            uint32_t sc = (st >> 1) & 0xFFu, sct = (st >> 9) & 0x7u;
            if (sc || sct) {
                printf("nvme%d: q%u opcode 0x%02x failed: SCT=%u SC=0x%02x%s\n", n->index,
                       q->qid, cmd->cdw0 & 0xFFu, sct, sc, (st & 0x8000u) ? " DNR" : "");
                return -1;
            }
            if (result) *result = res;
            return 0;
        }
        if ((++spins & 0xFFFu) == 0) {
            uint32_t csts = nvme_rd32(n, NVME_REG_CSTS);
            if (csts == 0xFFFFFFFFu || (csts & NVME_CSTS_CFS)) {
                printf("nvme%d: controller fatal status (CSTS=0x%x) during opcode 0x%02x\n", n->index,
                       csts, cmd->cdw0 & 0xFFu);
                n->dead = true;
                return -1;
            }
            if (ktime_expired(deadline) || spins > (uint64_t)timeout_ms * 200000ull) {
                printf("nvme%d: q%u opcode 0x%02x timed out after %u ms; I/O disabled\n", n->index,
                       q->qid, cmd->cdw0 & 0xFFu, timeout_ms);
                n->dead = true;
                return -1;
            }
        }
        __asm__ volatile("pause");
    }
}

static int nvme_admin(nvme_controller_t *n, nvme_command_t *cmd, uint32_t *result) {
    return nvme_submit_wait(n, &n->admin, cmd, NVME_ADMIN_TIMEOUT_MS, result);
}

static int nvme_identify(nvme_controller_t *n, uint32_t cns, uint32_t nsid) {
    nvme_command_t c;
    memset(&c, 0, sizeof(c));
    memset(n->idbuf, 0, PAGE_SIZE);
    c.cdw0 = NVME_ADMIN_IDENTIFY;
    c.nsid = nsid;
    c.prp1 = n->idbuf_phys;
    c.cdw10 = cns;
    return nvme_admin(n, &c, NULL);
}

static void nvme_copy_str(char *dst, const uint8_t *src, int n) {
    int i;
    for (i = 0; i < n; i++)
        dst[i] = (src[i] >= 0x20 && src[i] < 0x7F) ? (char)src[i] : ' ';
    dst[n] = '\0';
    while (n > 0 && dst[n - 1] == ' ')
        dst[--n] = '\0';
}

static int nvme_alloc_queue(nvme_queue_t *q, uint16_t qid, uint16_t depth) {
    memset(q, 0, sizeof(*q));
    q->qid = qid;
    q->depth = depth;
    q->phase = 1;
    q->sq = (volatile nvme_command_t *)nvme_dma_alloc(depth * 64u, &q->sq_phys);
    q->cq = (volatile nvme_completion_t *)nvme_dma_alloc(depth * 16u, &q->cq_phys);
    return (q->sq && q->cq) ? 0 : -1;
}

static int nvme_create_io_queues(nvme_controller_t *n) {
    uint32_t mqes = NVME_CAP_MQES(n->cap) + 1u;
    uint16_t depth = (uint16_t)(mqes < NVME_IOQ_DEPTH_MAX ? mqes : NVME_IOQ_DEPTH_MAX);
    nvme_command_t c;

    if (nvme_alloc_queue(&n->io, 1, depth) != 0)
        return -1;

    /* Number of Queues: 1 SQ + 1 CQ (0-based). Optional on some controllers. */
    memset(&c, 0, sizeof(c));
    c.cdw0 = NVME_ADMIN_SET_FEATURES;
    c.cdw10 = 0x07;
    c.cdw11 = 0;
    if (nvme_admin(n, &c, NULL) != 0)
        serial_printf("nvme%d: Set Features (Number of Queues) failed, continuing\n", n->index);

    memset(&c, 0, sizeof(c));
    c.cdw0 = NVME_ADMIN_CREATE_CQ;
    c.prp1 = n->io.cq_phys;
    c.cdw10 = ((uint32_t)(depth - 1) << 16) | 1u;
    c.cdw11 = 1u;                       /* physically contiguous, no interrupts */
    if (nvme_admin(n, &c, NULL) != 0) {
        printf("nvme%d: Create I/O CQ failed\n", n->index);
        return -1;
    }
    memset(&c, 0, sizeof(c));
    c.cdw0 = NVME_ADMIN_CREATE_SQ;
    c.prp1 = n->io.sq_phys;
    c.cdw10 = ((uint32_t)(depth - 1) << 16) | 1u;
    c.cdw11 = (1u << 16) | 1u;          /* CQID 1, physically contiguous */
    if (nvme_admin(n, &c, NULL) != 0) {
        printf("nvme%d: Create I/O SQ failed\n", n->index);
        return -1;
    }
    serial_printf("nvme%d: I/O queue pair 1 created, depth %u\n", n->index, depth);
    return 0;
}

static int nvme_pick_namespace(nvme_controller_t *n, uint32_t nn) {
    uint32_t nsid = 0;
    if (nvme_identify(n, NVME_ID_CNS_NS_ACTIVE, 0) == 0) {
        nsid = ((uint32_t *)n->idbuf)[0];
    } else {
        /* NVMe 1.0 controllers: probe NSIDs directly. */
        for (uint32_t i = 1; i <= nn && i <= 16; i++) {
            if (nvme_identify(n, NVME_ID_CNS_NS, i) == 0 &&
                *(uint64_t *)n->idbuf != 0) {
                nsid = i;
                break;
            }
            if (n->dead) return -1;
        }
    }
    if (nsid == 0) {
        printf("nvme%d: no active namespace\n", n->index);
        return -1;
    }
    if (nvme_identify(n, NVME_ID_CNS_NS, nsid) != 0)
        return -1;

    const uint8_t *ns = n->idbuf;
    uint64_t nsze = *(const uint64_t *)(ns + 0);
    uint8_t nlbaf = ns[25];
    uint8_t flbas = ns[26];
    uint32_t fmt = (flbas & 0x0Fu) | ((uint32_t)((flbas >> 5) & 0x3u) << 4);
    if (fmt > nlbaf) {
        printf("nvme%d: bad FLBAS %u (NLBAF %u)\n", n->index, fmt, nlbaf);
        return -1;
    }
    uint32_t lbaf = *(const uint32_t *)(ns + 128 + 4 * fmt);
    uint32_t ms = lbaf & 0xFFFFu;
    uint32_t lbads = (lbaf >> 16) & 0xFFu;
    serial_printf("nvme%d: ns %u NSZE=%llu NLBAF=%u FLBAS=0x%x LBAF%u: LBADS=%u MS=%u\n", n->index,
                  nsid, (unsigned long long)nsze, nlbaf, flbas, fmt, lbads, ms);
    if (lbads < 9 || lbads > 12) {
        printf("nvme%d: unsupported LBA size 2^%u\n", n->index, lbads);
        return -1;
    }
    if (ms != 0) {
        if (flbas & 0x10u) {
            printf("nvme%d: namespace uses extended-LBA metadata (%u bytes), unsupported\n", n->index, ms);
            return -1;
        }
        printf("nvme%d: namespace has %u bytes of separate metadata; ignoring it\n", n->index, ms);
    }
    if (nsze == 0) {
        printf("nvme%d: namespace %u is empty\n", n->index, nsid);
        return -1;
    }
    n->nsid = nsid;
    n->lba_shift = lbads;
    n->ns_blocks = nsze;
    n->sectors512 = nsze << (lbads - 9);
    return 0;
}

static void nvme_probe_bar(nvme_controller_t *n, struct pci_device *dev) {
    uint32_t lo = pci_device_read(dev, 0x10, 4);
    uint32_t hi = 0;
    bool is64 = (lo & 0x6u) == 0x4u;
    if (is64)
        hi = pci_device_read(dev, 0x14, 4);
    n->bar_phys = ((uint64_t)hi << 32) | (lo & 0xFFFFFFF0u);

    /* Size the BAR with memory decoding off, then restore it. */
    uint16_t cmd = (uint16_t)pci_device_read(dev, 0x04, 2);
    pci_device_write(dev, 0x04, 2, cmd & (uint16_t)~PCI_CMD_MEMORY_SPACE);
    pci_device_write(dev, 0x10, 4, 0xFFFFFFFFu);
    uint32_t mlo = pci_device_read(dev, 0x10, 4);
    uint32_t mhi = 0xFFFFFFFFu;
    if (is64) {
        pci_device_write(dev, 0x14, 4, 0xFFFFFFFFu);
        mhi = pci_device_read(dev, 0x14, 4);
        pci_device_write(dev, 0x14, 4, hi);
    }
    pci_device_write(dev, 0x10, 4, lo);
    pci_device_write(dev, 0x04, 2, cmd);

    uint64_t mask = ((uint64_t)mhi << 32) | (mlo & 0xFFFFFFF0u);
    n->bar_size = mask ? (~mask + 1u) : 0;
    if (!is64)
        n->bar_size &= 0xFFFFFFFFull;
    if (n->bar_size == 0 || n->bar_size > (64ull << 20))
        n->bar_size = 0x4000;   /* registers + doorbells */
    printf("nvme%d: BAR0 phys 0x%llx size 0x%llx (%s)\n", n->index,
           (unsigned long long)n->bar_phys,
           (unsigned long long)n->bar_size, is64 ? "64-bit" : "32-bit");
}

/* Bring up one controller. Returns 0 when the namespace is usable. */
static int nvme_init_ctrl(nvme_controller_t *n, struct pci_device *dev) {
    n->pci_dev = dev;
    n->storage_id = 0xFF;
    printf("NVMe: nvme%d at PCI %02x:%02x.%x, vendor:device %04x:%04x\n",
           n->index, dev->bus, dev->device, dev->function, dev->vendor_id, dev->device_id);

    nvme_probe_bar(n, dev);
    if (n->bar_phys == 0) {
        printf("nvme%d: BAR0 not assigned by firmware\n", n->index);
        return -1;
    }
    /* Memory decoding + bus mastering on, legacy INTx off (we poll). */
    uint16_t cmd = (uint16_t)pci_device_read(dev, 0x04, 2);
    cmd |= PCI_CMD_MEMORY_SPACE | PCI_CMD_BUS_MASTER | PCI_CMD_INT_DISABLE;
    pci_device_write(dev, 0x04, 2, cmd);

    n->regs = nvme_map_bar(n->bar_phys, n->bar_size);
    if (!n->regs) {
        printf("nvme%d: cannot map BAR0\n", n->index);
        return -1;
    }

    uint64_t cap = nvme_rd64(n, NVME_REG_CAP);
    if (cap == 0xFFFFFFFFFFFFFFFFull) {
        printf("nvme%d: registers read all-ones; controller not responding\n", n->index);
        return -1;
    }
    n->cap = cap;
    n->version = nvme_rd32(n, NVME_REG_VS);
    n->dstrd_bytes = 4u << NVME_CAP_DSTRD(cap);
    n->timeout_ms = NVME_CAP_TO(cap) * 500u;
    if (n->timeout_ms < 500u) n->timeout_ms = 500u;
    printf("nvme%d: version %u.%u.%u CAP=0x%llx MQES=%u DSTRD=%u TO=%u ms MPSMIN=%u MPSMAX=%u\n", n->index,
           n->version >> 16, (n->version >> 8) & 0xFFu, n->version & 0xFFu,
           (unsigned long long)cap, NVME_CAP_MQES(cap) + 1u, NVME_CAP_DSTRD(cap),
           n->timeout_ms, 4096u << NVME_CAP_MPSMIN(cap), 4096u << NVME_CAP_MPSMAX(cap));
    if (!NVME_CAP_CSS_NVM(cap)) {
        printf("nvme%d: controller does not support the NVM command set\n", n->index);
        return -1;
    }
    if (NVME_CAP_MPSMIN(cap) != 0) {
        printf("nvme%d: minimum page size > 4 KiB, unsupported\n", n->index);
        return -1;
    }
    if (n->bar_size < NVME_REG_DBS + 4u * n->dstrd_bytes) {
        n->bar_size = NVME_REG_DBS + 4u * n->dstrd_bytes;
        if (!nvme_map_bar(n->bar_phys, n->bar_size)) return -1;
    }

    /* Controller reset: if enabling is still in progress wait for RDY first,
     * then clear EN and wait for RDY=0. */
    uint32_t cc = nvme_rd32(n, NVME_REG_CC);
    if (cc & NVME_CC_ENABLE) {
        if (!(nvme_rd32(n, NVME_REG_CSTS) & NVME_CSTS_RDY))
            (void)nvme_wait_rdy(n, true, n->timeout_ms);
        nvme_wr32(n, NVME_REG_CC, cc & ~NVME_CC_ENABLE);
    }
    int r = nvme_wait_rdy(n, false, n->timeout_ms);
    if (r != 0) {
        printf("nvme%d: controller did not disable (CSTS=0x%x, err %d)\n", n->index,
               nvme_rd32(n, NVME_REG_CSTS), r);
        return -1;
    }

    /* DMA memory */
    n->idbuf = nvme_dma_alloc(PAGE_SIZE, &n->idbuf_phys);
    n->prp_list = (uint64_t *)nvme_dma_alloc(PAGE_SIZE, &n->prp_list_phys);
    n->bounce = nvme_dma_alloc(NVME_BOUNCE_BYTES, &n->bounce_phys);
    if (!n->idbuf || !n->prp_list || !n->bounce ||
        nvme_alloc_queue(&n->admin, 0, NVME_AQ_DEPTH) != 0) {
        printf("nvme%d: out of DMA memory\n", n->index);
        return -1;
    }

    /* Admin queue + enable */
    nvme_wr32(n, NVME_REG_AQA, ((NVME_AQ_DEPTH - 1u) << 16) | (NVME_AQ_DEPTH - 1u));
    nvme_wr64(n, NVME_REG_ASQ, n->admin.sq_phys);
    nvme_wr64(n, NVME_REG_ACQ, n->admin.cq_phys);
    cc = NVME_CC_CSS_NVM | NVME_CC_MPS_4K | NVME_CC_AMS_RR |
         NVME_CC_IOSQES_64 | NVME_CC_IOCQES_16;
    nvme_wr32(n, NVME_REG_CC, cc);
    nvme_wr32(n, NVME_REG_CC, cc | NVME_CC_ENABLE);
    r = nvme_wait_rdy(n, true, n->timeout_ms);
    if (r != 0) {
        printf("nvme%d: controller did not become ready (CSTS=0x%x, err %d%s)\n", n->index,
               nvme_rd32(n, NVME_REG_CSTS), r, r == -3 ? ", fatal status" : "");
        return -1;
    }
    nvme_wr32(n, NVME_REG_INTMS, 0xFFFFFFFFu);   /* polled: mask all vectors */

    /* Identify controller */
    if (nvme_identify(n, NVME_ID_CNS_CTRL, 0) != 0) {
        printf("nvme%d: Identify Controller failed\n", n->index);
        return -1;
    }
    const uint8_t *id = n->idbuf;
    nvme_copy_str(n->serial, id + 4, 20);
    nvme_copy_str(n->model, id + 24, 40);
    nvme_copy_str(n->fw, id + 64, 8);
    uint8_t mdts = id[77];
    uint32_t nn = *(const uint32_t *)(id + 516);
    n->vwc = (id[525] & 1u) != 0;
    n->max_xfer = NVME_BOUNCE_BYTES;
    if (mdts) {
        uint64_t lim = (uint64_t)PAGE_SIZE << mdts;   /* MPSMIN is 4 KiB here */
        if (lim < n->max_xfer)
            n->max_xfer = (uint32_t)lim;
    }
    printf("nvme%d: model \"%s\" serial \"%s\" fw \"%s\" NN=%u MDTS=%u VWC=%u\n",
           n->index, n->model, n->serial, n->fw, nn, mdts, n->vwc);

    if (nvme_pick_namespace(n, nn) != 0)
        return -1;
    if ((1u << n->lba_shift) > n->max_xfer) {
        printf("nvme%d: MDTS smaller than one LBA\n", n->index);
        return -1;
    }
    if (nvme_create_io_queues(n) != 0)
        return -1;

    n->initialized = true;
    printf("nvme%d: namespace %u: %llu LBAs x %u bytes (%llu MiB), max transfer %u KiB\n", n->index,
           n->nsid, (unsigned long long)n->ns_blocks, 1u << n->lba_shift,
           (unsigned long long)((n->ns_blocks << n->lba_shift) >> 20),
           n->max_xfer / 1024u);
    return 0;
}

static int nvme_register_ctrl(nvme_controller_t *n) {
    if (n->storage_id != 0xFF)
        return 0;
    if (storage_device_count >= MAX_STORAGE_DEVICES) {
        printf("nvme%d: no free storage device slot\n", n->index);
        return -1;
    }
    uint8_t id = storage_device_count;
    storage_device_t *d = &storage_devices[id];
    memset(d, 0, sizeof(*d));
    d->device_id = id;
    uint64_t secs = n->sectors512;
    if (secs > 0xFFFFFFFFull) {
        printf("nvme%d: disk larger than 2 TiB; only the first 2 TiB are addressable\n", n->index);
        secs = 0xFFFFFFFFull;
    }
    d->sector_count = (uint32_t)secs;
    d->sector_size = 512;               /* 4K namespaces are emulated as 512 */
    d->is_initialized = true;
    d->pci_dev = n->pci_dev;
    d->base_address = n->bar_phys;
    snprintf(d->name, sizeof(d->name), "nvme%dn%u %s", n->index, n->nsid,
             n->model[0] ? n->model : "NVMe SSD");
    n->storage_id = id;
    storage_device_count++;
    return 0;
}

void nvme_init(void) {
    static bool done;
    if (done)
        return;
    done = true;
    int found = 0;
    for (int i = 0; i < pci_device_count; i++) {
        struct pci_device *dev = pci_devices[i];
        if (dev->class_code != 0x01 || dev->subclass_code != 0x08 || dev->prog_if != 0x02)
            continue;
        found++;
        if (nvme_ctrl_count >= NVME_MAX_CTRL) {
            printf("NVMe: more than %d controllers, ignoring %02x:%02x.%x\n",
                   NVME_MAX_CTRL, dev->bus, dev->device, dev->function);
            continue;
        }
        nvme_controller_t *n = &nvme_ctrls[nvme_ctrl_count];
        memset(n, 0, sizeof(*n));
        n->index = nvme_ctrl_count++;
        if (nvme_init_ctrl(n, dev) != 0) {
            printf("NVMe: nvme%d (%02x:%02x.%x %04x:%04x) NOT usable, see messages above\n",
                   n->index, dev->bus, dev->device, dev->function, dev->vendor_id, dev->device_id);
            continue;
        }
        n->initialized = true;
        nvme_register_ctrl(n);
        printf("NVMe: nvme%dn%u = %02x:%02x.%x %04x:%04x model \"%s\" serial \"%s\" fw \"%s\", "
               "%llu MiB, LBA %u B, storage device %d\n",
               n->index, n->nsid, dev->bus, dev->device, dev->function,
               dev->vendor_id, dev->device_id, n->model, n->serial, n->fw,
               (unsigned long long)((n->ns_blocks << n->lba_shift) >> 20),
               1u << n->lba_shift, n->storage_id == 0xFF ? -1 : (int)n->storage_id);
    }
    printf("NVMe: %d controller(s) found, %d usable\n", found, nvme_usable_count());
}

int nvme_usable_count(void) {
    int c = 0;
    for (int i = 0; i < nvme_ctrl_count; i++)
        if (nvme_ctrls[i].initialized && !nvme_ctrls[i].dead) c++;
    return c;
}

int nvme_register_storage(void) {
    int r = -1;
    for (int i = 0; i < nvme_ctrl_count; i++)
        if (nvme_ctrls[i].initialized && nvme_register_ctrl(&nvme_ctrls[i]) == 0) r = 0;
    return r;
}

/* One I/O command on nblocks LBAs through the bounce buffer. */
static int nvme_io(nvme_controller_t *n, uint8_t opcode, uint64_t blba, uint32_t nblocks) {
    uint32_t bytes = nblocks << n->lba_shift;
    uint32_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    nvme_command_t c;
    memset(&c, 0, sizeof(c));
    c.cdw0 = opcode;
    c.nsid = n->nsid;
    c.prp1 = n->bounce_phys;
    if (pages == 2) {
        c.prp2 = n->bounce_phys + PAGE_SIZE;
    } else if (pages > 2) {
        for (uint32_t i = 1; i < pages; i++)
            n->prp_list[i - 1] = n->bounce_phys + (uint64_t)i * PAGE_SIZE;
        c.prp2 = n->prp_list_phys;
    }
    c.cdw10 = (uint32_t)blba;
    c.cdw11 = (uint32_t)(blba >> 32);
    c.cdw12 = (nblocks - 1u) & 0xFFFFu;
    return nvme_submit_wait(n, &n->io, &c, NVME_IO_TIMEOUT_MS, NULL);
}

static nvme_controller_t *nvme_for_dev(storage_device_t *dev) {
    if (!dev) return NULL;
    for (int i = 0; i < nvme_ctrl_count; i++) {
        nvme_controller_t *n = &nvme_ctrls[i];
        if (n->initialized && n->storage_id == dev->device_id)
            return n->dead ? NULL : n;
    }
    return NULL;
}

static nvme_controller_t *nvme_check(storage_device_t *dev, uint64_t lba, uint32_t count) {
    nvme_controller_t *n = nvme_for_dev(dev);
    if (!n)
        return NULL;
    if (count == 0 || lba + count > n->sectors512) {
        printf("nvme%d: access beyond end (lba %llu + %u)\n", n->index, (unsigned long long)lba, count);
        return NULL;
    }
    return n;
}

static int nvme_rw512(nvme_controller_t *n, int write, uint64_t lba, uint32_t count, uint8_t *rbuf, const uint8_t *wbuf) {
    uint32_t spb = 1u << (n->lba_shift - 9);       /* 512-byte sectors per LBA */
    uint32_t max_blocks = n->max_xfer >> n->lba_shift;
    while (count) {
        uint64_t blba = lba / spb;
        uint32_t off = (uint32_t)(lba % spb);
        uint32_t take = max_blocks * spb - off;
        if (take > count) take = count;
        uint32_t nb = (off + take + spb - 1) / spb;
        uint32_t bytes = take * 512u;
        if (!write) {
            if (nvme_io(n, NVME_CMD_READ, blba, nb) != 0) return -1;
            memcpy(rbuf, n->bounce + off * 512u, bytes);
            rbuf += bytes;
        } else {
            if ((off != 0 || ((off + take) % spb) != 0) &&
                nvme_io(n, NVME_CMD_READ, blba, nb) != 0)
                return -1;              /* read-modify-write of partial LBAs */
            memcpy(n->bounce + off * 512u, wbuf, bytes);
            if (nvme_io(n, NVME_CMD_WRITE, blba, nb) != 0) return -1;
            wbuf += bytes;
        }
        lba += take;
        count -= take;
    }
    return 0;
}

int nvme_read_sectors(storage_device_t *dev, uint64_t lba, uint32_t sector_count, void *buffer) {
    nvme_controller_t *n = nvme_check(dev, lba, sector_count);
    if (!n || !buffer)
        return -1;
    klock(&n->lock);
    int r = nvme_rw512(n, 0, lba, sector_count, (uint8_t *)buffer, NULL);
    kunlock(&n->lock);
    return r;
}

int nvme_write_sectors(storage_device_t *dev, uint64_t lba, uint32_t sector_count, const void *buffer) {
    nvme_controller_t *n = nvme_check(dev, lba, sector_count);
    if (!n || !buffer)
        return -1;
    klock(&n->lock);
    int r = nvme_rw512(n, 1, lba, sector_count, NULL, (const uint8_t *)buffer);
    kunlock(&n->lock);
    return r;
}

int nvme_flush(storage_device_t *dev) {
    nvme_controller_t *n = nvme_for_dev(dev);
    if (!n)
        return -1;
    if (!n->vwc)
        return 0;                       /* no volatile cache: writes are durable */
    nvme_command_t c;
    memset(&c, 0, sizeof(c));
    c.cdw0 = NVME_CMD_FLUSH;
    c.nsid = n->nsid;
    klock(&n->lock);
    int r = nvme_submit_wait(n, &n->io, &c, NVME_IO_TIMEOUT_MS, NULL);
    kunlock(&n->lock);
    return r;
}

void nvme_print_info(void) {
    if (nvme_ctrl_count == 0) {
        printf("NVMe: no controller\n");
        return;
    }
    for (int i = 0; i < nvme_ctrl_count; i++) {
        nvme_controller_t *n = &nvme_ctrls[i];
        struct pci_device *d = n->pci_dev;
        printf("nvme%d: PCI %02x:%02x.%x %04x:%04x BAR0 0x%llx: %s%s\n", n->index,
               d->bus, d->device, d->function, d->vendor_id, d->device_id,
               (unsigned long long)n->bar_phys,
               n->initialized ? "ready" : "not initialised",
               n->dead ? " (failed, I/O disabled)" : "");
        if (!n->initialized) continue;
        printf("  model \"%s\" serial \"%s\" fw \"%s\"\n", n->model, n->serial, n->fw);
        printf("  nvme%dn%u: %llu LBAs x %u B (%llu MiB), storage device %d, max xfer %u KiB, VWC %u\n",
               n->index, n->nsid, (unsigned long long)n->ns_blocks, 1u << n->lba_shift,
               (unsigned long long)((n->ns_blocks << n->lba_shift) >> 20),
               n->storage_id == 0xFF ? -1 : (int)n->storage_id, n->max_xfer / 1024u, n->vwc);
    }
}
