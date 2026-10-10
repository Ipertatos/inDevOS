/**
 * @file usb_msc.c
 * @brief USB mass storage: Bulk-Only Transport + SCSI block commands.
 *
 * INQUIRY, TEST UNIT READY / REQUEST SENSE (spin-up, unit attention), READ
 * CAPACITY(10) and (16) for > 2 TiB, READ/WRITE(10) and (16), SYNCHRONIZE
 * CACHE. Stall handling per BOT: clear the halted pipe and still read the
 * CSW; phase errors and failed CSWs use Bulk-Only Mass Storage Reset +
 * clear halts (reset recovery).
 *
 * Every disk is registered with the storage layer as 512-byte sectors named
 * usbN. Disks with larger logical blocks (4 KiB) are emulated: reads fetch
 * whole blocks, partial writes are read-modify-write, like NVMe.
 */
#include "usb_core.h"
#include "storage.h"
#include "mem.h"
#include "string.h"
#include "serial.h"
#include "stdio.h"
#include "ktime.h"

extern void *dma_alloc_pages(uint32_t bytes, uint64_t *phys);
extern void dma_free_pages(void *p);

#define MSC_MAX        8
#define MSC_XFER_BYTES (120u * 1024u)     /* per command, like Linux's 240 sectors */
#define MSC_TAG_BASE   0x5553420000ull     /* storage_device_t.base_address marker */

typedef struct {
    bool online;
    int index;
    usb_dev_t *dev;
    uint8_t iface, ep_in, ep_out, lun;
    uint32_t tag;
    uint32_t block_size;
    uint64_t blocks;
    uint8_t *cbw, *csw, *sense;
    uint8_t *bounce;
    uint8_t storage_id;
    char vendor[9], product[17];
    uint32_t errors;
} msc_t;

static msc_t *msc_devs[MSC_MAX];

static void msc_reset_recovery(msc_t *m) {
    serial_printf("usb%d: bulk-only reset recovery\n", m->index);
    usb_ctrl(m->dev, 0x21, 0xFF, 0, m->iface, 0, NULL);
    ktime_busywait_ms(20);
    usb_clear_halt(m->dev, m->ep_in);
    usb_clear_halt(m->dev, m->ep_out);
}

/* One BOT command. data must be a DMA buffer (m->bounce or m->sense).
 * Returns 0 good, 1 CHECK CONDITION, -1 transport failure. */
static int bot(msc_t *m, const uint8_t *cdb, uint8_t cdblen, bool in, void *data,
               uint32_t len, uint32_t timeout_ms, uint32_t *residue) {
    if (!m->online) return -1;
    xhci_slot_t *s = m->dev->slot;
    uint8_t *c = m->cbw;
    memset(c, 0, 31);
    uint32_t tag = ++m->tag;
    c[0] = 0x55; c[1] = 0x53; c[2] = 0x42; c[3] = 0x43;
    memcpy(c + 4, &tag, 4);
    memcpy(c + 8, &len, 4);
    c[12] = in ? 0x80 : 0x00;
    c[13] = m->lun;
    c[14] = cdblen;
    memcpy(c + 15, cdb, cdblen);
    int r = xhci_bulk(s, m->ep_out, c, 31, 5000);
    if (r != 31) {
        if (r == -XHCI_CC_STALL) usb_clear_halt(m->dev, m->ep_out);
        msc_reset_recovery(m);
        return -1;
    }
    if (len) {
        r = xhci_bulk(s, in ? m->ep_in : m->ep_out, data, len, timeout_ms);
        if (r == -XHCI_CC_STALL) {
            usb_clear_halt(m->dev, in ? m->ep_in : m->ep_out);
        } else if (r < 0) {
            msc_reset_recovery(m);
            return -1;
        }
    }
    r = xhci_bulk(s, m->ep_in, m->csw, 13, 5000);
    if (r == -XHCI_CC_STALL) {
        usb_clear_halt(m->dev, m->ep_in);
        r = xhci_bulk(s, m->ep_in, m->csw, 13, 5000);
    }
    uint32_t sig, ctag, res;
    memcpy(&sig, m->csw, 4);
    memcpy(&ctag, m->csw + 4, 4);
    memcpy(&res, m->csw + 8, 4);
    if (r != 13 || sig != 0x53425355u || ctag != tag || m->csw[12] > 1) {
        serial_printf("usb%d: bad CSW (r=%d sig=0x%x tag %u/%u status %u)\n", m->index, r,
                      sig, ctag, tag, m->csw[12]);
        msc_reset_recovery(m);
        return -1;
    }
    if (residue) *residue = res;
    return m->csw[12] ? 1 : 0;
}

/* sense key / ASC of the last CHECK CONDITION */
static int request_sense(msc_t *m, uint8_t *key, uint8_t *asc) {
    uint8_t cdb[6] = { 0x03, 0, 0, 0, 18, 0 };
    if (bot(m, cdb, 6, true, m->sense, 18, 2000, NULL) != 0) return -1;
    *key = m->sense[2] & 0x0F;
    *asc = m->sense[12];
    return 0;
}

/* Run a command, retrying on unit attention / not ready a few times. */
static int scsi(msc_t *m, const uint8_t *cdb, uint8_t cdblen, bool in, void *data,
                uint32_t len, uint32_t timeout_ms) {
    for (int attempt = 0; attempt < 4; attempt++) {
        int r = bot(m, cdb, cdblen, in, data, len, timeout_ms, NULL);
        if (r == 0) return 0;
        if (r < 0) continue;                    /* transport error: reset done, retry */
        uint8_t key = 0, asc = 0;
        if (request_sense(m, &key, &asc) != 0) continue;
        if (key == 0x06 || key == 0x02 || key == 0x0B) {   /* unit attention, not ready, aborted */
            ktime_busywait_ms(key == 0x02 ? 200 : 10);
            continue;
        }
        serial_printf("usb%d: SCSI 0x%02x failed, sense key 0x%x asc 0x%02x\n", m->index,
                      cdb[0], key, asc);
        return -1;
    }
    m->errors++;
    return -1;
}

static inline void be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static inline void be64(uint8_t *p, uint64_t v) {
    be32(p, (uint32_t)(v >> 32));
    be32(p + 4, (uint32_t)v);
}
static inline uint32_t get_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static int rw_blocks(msc_t *m, bool write, uint64_t blk, uint32_t n, void *dma) {
    uint8_t cdb[16];
    memset(cdb, 0, sizeof(cdb));
    uint32_t bytes = n * m->block_size;
    uint32_t tmo = 10000 + bytes / 1024u * 10u;
    if (blk + n > 0xFFFFFFFFull || m->blocks > 0xFFFFFFFFull) {
        cdb[0] = write ? 0x8A : 0x88;
        be64(cdb + 2, blk);
        be32(cdb + 10, n);
        return scsi(m, cdb, 16, !write, dma, bytes, tmo);
    }
    cdb[0] = write ? 0x2A : 0x28;
    be32(cdb + 2, (uint32_t)blk);
    cdb[7] = (uint8_t)(n >> 8);
    cdb[8] = (uint8_t)n;
    return scsi(m, cdb, 10, !write, dma, bytes, tmo);
}

static msc_t *msc_of(storage_device_t *dev) {
    if (!dev || (dev->base_address & ~0xFFull) != MSC_TAG_BASE) return NULL;
    uint32_t i = (uint32_t)(dev->base_address & 0xFF);
    return i < MSC_MAX ? msc_devs[i] : NULL;
}

/* 512-byte sector interface for storage.c */
static int msc_io(storage_device_t *dev, uint64_t lba, uint32_t count, void *buf, bool write) {
    msc_t *m = msc_of(dev);
    if (!m || !m->online) return -1;
    uint32_t spb = m->block_size / 512u;          /* sectors per block */
    uint32_t max_blocks = MSC_XFER_BYTES / m->block_size;
    uint8_t *p = (uint8_t *)buf;
    int rc = 0;
    usb_lock();
    while (count && rc == 0) {
        uint64_t blk = lba / spb;
        uint32_t off = (uint32_t)(lba % spb);       /* sector offset inside blk */
        uint32_t nblk = (off + count + spb - 1) / spb;
        if (nblk > max_blocks) nblk = max_blocks;
        uint32_t secs = nblk * spb - off;
        if (secs > count) secs = count;
        bool partial = off != 0 || ((off + secs) % spb) != 0;
        if (!write) {
            rc = rw_blocks(m, false, blk, nblk, m->bounce);
            if (rc == 0) memcpy(p, m->bounce + off * 512u, secs * 512u);
        } else {
            if (partial) rc = rw_blocks(m, false, blk, nblk, m->bounce);
            if (rc == 0) {
                memcpy(m->bounce + off * 512u, p, secs * 512u);
                rc = rw_blocks(m, true, blk, nblk, m->bounce);
            }
        }
        lba += secs;
        count -= secs;
        p += secs * 512u;
    }
    usb_unlock();
    return rc == 0 ? 0 : -1;
}

int usb_msc_read_sectors(storage_device_t *dev, uint64_t lba, uint32_t count, void *buf) {
    return msc_io(dev, lba, count, buf, false);
}

int usb_msc_write_sectors(storage_device_t *dev, uint64_t lba, uint32_t count, const void *buf) {
    return msc_io(dev, lba, count, (void *)buf, true);
}

int usb_msc_flush(storage_device_t *dev) {
    msc_t *m = msc_of(dev);
    if (!m || !m->online) return -1;
    uint8_t cdb[10] = { 0x35, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    usb_lock();
    int r = bot(m, cdb, 10, false, NULL, 0, 20000, NULL);
    if (r == 1) {                                  /* not supported by many sticks */
        uint8_t k, a;
        request_sense(m, &k, &a);
        r = 0;
    }
    usb_unlock();
    return r == 0 ? 0 : -1;
}

static void copy_str(char *dst, const uint8_t *src, int n) {
    int i;
    for (i = 0; i < n; i++) dst[i] = (src[i] >= 0x20 && src[i] < 0x7F) ? (char)src[i] : ' ';
    dst[n] = '\0';
    while (i > 0 && dst[i - 1] == ' ') dst[--i] = '\0';
}

int usb_msc_attach(usb_dev_t *d, usb_iface_info_t *ifc, void **data) {
    int idx = -1;
    for (int i = 0; i < MSC_MAX; i++) if (!msc_devs[i]) { idx = i; break; }
    if (idx < 0) return -1;
    uint8_t in = 0, out = 0;
    usb_ep_info_t *ein = NULL, *eout = NULL;
    for (int e = 0; e < ifc->n_ep; e++) {
        if (ifc->ep[e].attr != 2) continue;
        if ((ifc->ep[e].addr & 0x80) && !in) { in = ifc->ep[e].addr; ein = &ifc->ep[e]; }
        if (!(ifc->ep[e].addr & 0x80) && !out) { out = ifc->ep[e].addr; eout = &ifc->ep[e]; }
    }
    if (!ein || !eout) return -1;
    if (xhci_configure_endpoint(d->slot, in, 2, ein->mps, 0, ein->burst) != 0 ||
        xhci_configure_endpoint(d->slot, out, 2, eout->mps, 0, eout->burst) != 0)
        return -1;
    msc_t *m = (msc_t *)kalloc(sizeof(msc_t));
    if (!m) return -1;
    memset(m, 0, sizeof(*m));
    uint64_t ph;
    uint8_t *small = (uint8_t *)dma_alloc_pages(4096, &ph);
    m->bounce = (uint8_t *)dma_alloc_pages(128u * 1024u, &ph);
    if (!small || !m->bounce) {
        if (small) dma_free_pages(small);
        if (m->bounce) dma_free_pages(m->bounce);
        kfree(m);
        return -1;
    }
    m->cbw = small;
    m->csw = small + 64;
    m->sense = small + 128;
    m->index = idx;
    m->dev = d;
    m->iface = ifc->number;
    m->ep_in = in;
    m->ep_out = out;
    m->online = true;
    m->storage_id = 0xFF;
    msc_devs[idx] = m;
    *data = m;

    /* Max LUN (stall = single LUN). Only LUN 0 is used. */
    uint8_t maxlun = 0;
    usb_ctrl(d, 0xA1, 0xFE, 0, m->iface, 1, &maxlun);

    uint8_t inq[6] = { 0x12, 0, 0, 0, 36, 0 };
    if (scsi(m, inq, 6, true, m->bounce, 36, 3000) == 0) {
        copy_str(m->vendor, m->bounce + 8, 8);
        copy_str(m->product, m->bounce + 16, 16);
        if ((m->bounce[0] & 0x1F) != 0x00) {
            printf("usb%d: SCSI device type 0x%x is not a disk\n", idx, m->bounce[0] & 0x1F);
        }
    }
    /* Wait for the medium (card readers, spinning up). */
    bool ready = false;
    for (int t = 0; t < 20 && !ready; t++) {
        uint8_t tur[6] = { 0, 0, 0, 0, 0, 0 };
        int r = bot(m, tur, 6, false, NULL, 0, 3000, NULL);
        if (r == 0) { ready = true; break; }
        if (r == 1) { uint8_t k, a; request_sense(m, &k, &a); }
        ktime_busywait_ms(100);
    }
    uint8_t rc10[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    if (scsi(m, rc10, 10, true, m->bounce, 8, 3000) != 0) {
        printf("usb%d: %s %s: no medium / READ CAPACITY failed\n", idx, m->vendor, m->product);
        m->online = false;
        return 0;                       /* stays bound so lsusb shows it */
    }
    uint32_t last = get_be32(m->bounce);
    m->block_size = get_be32(m->bounce + 4);
    m->blocks = (uint64_t)last + 1;
    if (last == 0xFFFFFFFFu) {
        uint8_t rc16[16] = { 0x9E, 0x10 };
        rc16[13] = 32;
        if (scsi(m, rc16, 16, true, m->bounce, 32, 3000) == 0) {
            m->blocks = (((uint64_t)get_be32(m->bounce) << 32) | get_be32(m->bounce + 4)) + 1;
            m->block_size = get_be32(m->bounce + 8);
        }
    }
    if (m->block_size < 512 || m->block_size > 65536 || (m->block_size % 512) != 0) {
        printf("usb%d: unsupported block size %u\n", idx, m->block_size);
        m->online = false;
        return 0;
    }
    (void)ready;
    uint64_t secs = m->blocks * (m->block_size / 512u);
    printf("usb%d: %s %s, %llu MiB, %u-byte blocks%s\n", idx, m->vendor, m->product,
           (unsigned long long)(secs >> 11), m->block_size,
           m->block_size != 512 ? " (512-byte sectors emulated)" : "");

    if (storage_device_count >= MAX_STORAGE_DEVICES) {
        printf("usb%d: no free storage device slot\n", idx);
        return 0;
    }
    if (secs > 0xFFFFFFFFull) {
        printf("usb%d: larger than 2 TiB; only the first 2 TiB are addressable\n", idx);
        secs = 0xFFFFFFFFull;
    }
    uint8_t id = storage_device_count;
    storage_device_t *sd = &storage_devices[id];
    memset(sd, 0, sizeof(*sd));
    sd->device_id = id;
    sd->sector_count = (uint32_t)secs;
    sd->sector_size = 512;
    sd->pci_dev = d->hc->pci;
    sd->base_address = MSC_TAG_BASE | (uint64_t)idx;
    snprintf(sd->name, sizeof(sd->name), "usb%d %s %s", idx, m->vendor, m->product);
    sd->is_initialized = true;
    storage_device_count++;
    m->storage_id = id;               /* fresh slot: nothing cached for it */
    printf("usb%d: storage device %u\n", idx, id);
    return 0;
}

void usb_msc_detach(void *data) {
    msc_t *m = (msc_t *)data;
    if (!m) return;
    m->online = false;
    if (m->storage_id != 0xFF && m->storage_id < MAX_STORAGE_DEVICES) {
        /* No bcache call here: usb_poll holds the USB lock and a reader may
         * hold storage_lock waiting for it. Slots are never reused, so the
         * stale cache lines for this id are harmless. */
        storage_devices[m->storage_id].is_initialized = false;
        printf("usb%d: removed (storage device %u is offline)\n", m->index, m->storage_id);
    }
    /* The storage slot keeps pointing at this index; keep the struct so a
     * stale caller fails cleanly instead of touching freed memory. */
    m->dev = NULL;
}

/* For lsblk-style listings */
bool usb_msc_is_usb(storage_device_t *dev) { return msc_of(dev) != NULL; }
