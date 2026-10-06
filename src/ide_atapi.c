#include "ide.h"
#include "pci.h"
#include "storage.h"
#include "registers.h"
#include "mem.h"
#include "utils.h"
#include "string.h"

#define IDE_ATAPI_MAX 2
#define IDE_CMD_PACKET 0xA0
#define IDE_CMD_IDENTIFY_PACKET 0xA1

typedef struct {
    uint16_t base;
    uint16_t ctrl;
    uint8_t sel;
    uint8_t shift;
    uint32_t block_size;
    uint32_t sectors_512;
    uint8_t storage_id;
    int present;
    int cache_valid;
    uint32_t cache_lba;
    uint8_t cache[2048];
} ide_atapi_t;

static ide_atapi_t atapi_units[IDE_ATAPI_MAX];
static int atapi_count;

static void atapi_delay(uint16_t ctrl) {
    (void)inb(ctrl); (void)inb(ctrl); (void)inb(ctrl); (void)inb(ctrl);
}

static void atapi_reset(uint16_t ctrl) {
    int i;
    outb(ctrl, 0x06);
    atapi_delay(ctrl);
    outb(ctrl, 0x02);
    for (i = 0; i < 100000; i++) {
        if (!(inb(ctrl) & IDE_STATUS_BSY))
            return;
        io_wait();
    }
}

static int atapi_wait_drq(uint16_t base) {
    int i, saw = 0;
    for (i = 0; i < 500000; i++) {
        uint8_t st = inb(base + IDE_REG_STATUS);
        if (st == 0xFF)
            return -1;
        if (st & IDE_STATUS_BSY) {
            saw = 1;
            io_wait();
            continue;
        }
        if (st & IDE_STATUS_DRQ)
            return 0;
        if (saw && (st & (IDE_STATUS_ERR | IDE_STATUS_DF)))
            return -1;
        io_wait();
    }
    return -1;
}

static ide_atapi_t *atapi_lookup(storage_device_t *dev) {
    int i;
    if (!dev) return 0;
    for (i = 0; i < atapi_count; i++)
        if (atapi_units[i].present && atapi_units[i].storage_id == dev->device_id)
            return &atapi_units[i];
    return 0;
}

static int atapi_packet(ide_atapi_t *u, const uint8_t *cdb, void *buf, uint32_t buflen) {
    uint16_t base = u->base;
    uint32_t limit = buflen > 65534u ? 65534u : buflen;
    uint32_t bytes, words, keep, i;
    uint16_t *dst;
    uint8_t st;

    outb(u->ctrl, 0x02);
    outb(base + IDE_REG_DRIVE, u->sel);
    atapi_delay(u->ctrl);
    for (i = 0; i < 100000; i++) {
        st = inb(base + IDE_REG_STATUS);
        if (!(st & IDE_STATUS_BSY))
            break;
        io_wait();
    }

    outb(base + IDE_REG_FEATURES, 0);
    outb(base + IDE_REG_SECCOUNT, 0);
    outb(base + IDE_REG_LBA_LOW, 0);
    outb(base + IDE_REG_LBA_MID, (uint8_t)(limit & 0xFF));
    outb(base + IDE_REG_LBA_HIGH, (uint8_t)((limit >> 8) & 0xFF));
    outb(base + IDE_REG_COMMAND, IDE_CMD_PACKET);
    if (atapi_wait_drq(base) != 0)
        return -1;
    for (i = 0; i < 6; i++) {
        uint16_t w = (uint16_t)cdb[i * 2] | ((uint16_t)cdb[i * 2 + 1] << 8);
        outw(base + IDE_REG_DATA, w);
    }
    if (atapi_wait_drq(base) != 0)
        return -1;

    bytes = (uint32_t)inb(base + IDE_REG_LBA_MID) | ((uint32_t)inb(base + IDE_REG_LBA_HIGH) << 8);
    if (bytes == 0)
        bytes = limit;
    bytes &= ~1u;
    words = bytes / 2;
    keep = buflen / 2;
    dst = (uint16_t *)buf;
    for (i = 0; i < words; i++) {
        uint16_t w = inw(base + IDE_REG_DATA);
        if (i < keep)
            dst[i] = w;
    }
    for (i = 0; i < 100000; i++) {
        st = inb(base + IDE_REG_STATUS);
        if (st & IDE_STATUS_BSY) { io_wait(); continue; }
        if (st & (IDE_STATUS_ERR | IDE_STATUS_DF))
            return -1;
        if (!(st & IDE_STATUS_DRQ))
            return 0;
        io_wait();
    }
    return -1;
}

static int atapi_fill(ide_atapi_t *u, uint32_t blk) {
    uint8_t cdb[12];
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x28;
    cdb[2] = (uint8_t)((blk >> 24) & 0xFF);
    cdb[3] = (uint8_t)((blk >> 16) & 0xFF);
    cdb[4] = (uint8_t)((blk >> 8) & 0xFF);
    cdb[5] = (uint8_t)(blk & 0xFF);
    cdb[8] = 1;
    if (atapi_packet(u, cdb, u->cache, u->block_size) != 0) {
        u->cache_valid = 0;
        return -1;
    }
    u->cache_lba = blk;
    u->cache_valid = 1;
    return 0;
}

int ide_atapi_read(storage_device_t *dev, uint64_t lba, uint32_t count, void *buffer) {
    ide_atapi_t *u = atapi_lookup(dev);
    uint8_t *dst = (uint8_t *)buffer;
    if (!u || !buffer || count == 0)
        return -1;
    if (lba + count > u->sectors_512)
        return -1;
    while (count) {
        uint32_t blk = (uint32_t)(lba >> u->shift);
        uint32_t off = (uint32_t)(lba & ((1u << u->shift) - 1u)) * 512u;
        if (!u->cache_valid || u->cache_lba != blk) {
            if (atapi_fill(u, blk) != 0)
                return -1;
        }
        if (off + 512u > u->block_size)
            return -1;
        memcpy(dst, u->cache + off, 512);
        dst += 512;
        lba++;
        count--;
    }
    return 0;
}

static int atapi_register(ide_atapi_t *u, struct pci_device *pci) {
    extern storage_device_t storage_devices[MAX_STORAGE_DEVICES];
    extern uint8_t storage_device_count;
    storage_device_t *d;
    uint8_t id;
    uint32_t mb;
    const char *name = "IDE CD-ROM";
    int i;
    if (storage_device_count >= MAX_STORAGE_DEVICES)
        return -1;
    id = storage_device_count;
    d = &storage_devices[id];
    memset(d, 0, sizeof(*d));
    d->device_id = id;
    d->sector_count = u->sectors_512;
    d->sector_size = 512;
    d->is_initialized = true;
    d->optical = true;
    d->pci_dev = pci;
    d->base_address = u->base;
    for (i = 0; i < 31 && name[i]; i++)
        d->name[i] = name[i];
    u->storage_id = id;
    u->present = 1;
    storage_device_count++;
    mb = (uint32_t)(((uint64_t)u->sectors_512 * 512ull) / (1024ull * 1024ull));
    printf("IDE: ATAPI CD-ROM is storage device %u (io 0x%x, %u MB, %u-byte blocks)\n",
           id, u->base, mb, u->block_size);
    return 0;
}

static int atapi_capacity(ide_atapi_t *u) {
    uint8_t cdb[12];
    uint16_t raw[4];
    uint8_t *b;
    uint32_t last, blen;
    uint64_t sec512;
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = 0x25;
    memset(raw, 0, sizeof(raw));
    if (atapi_packet(u, cdb, raw, 8) != 0)
        return -1;
    b = (uint8_t *)raw;
    last = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
    blen = ((uint32_t)b[4] << 24) | ((uint32_t)b[5] << 16) | ((uint32_t)b[6] << 8) | b[7];
    if (blen == 2048) u->shift = 2;
    else if (blen == 512) u->shift = 0;
    else {
        printf("IDE: ATAPI block size %u unsupported\n", blen);
        return -1;
    }
    u->block_size = blen;
    sec512 = ((uint64_t)last + 1ull) << u->shift;
    if (sec512 == 0 || sec512 > 0xFFFFFFFFull)
        return -1;
    u->sectors_512 = (uint32_t)sec512;
    return 0;
}

static void atapi_try(uint16_t base, uint16_t ctrl, uint8_t sel, struct pci_device *pci) {
    ide_atapi_t *u;
    uint8_t st, mid, hi;
    uint16_t discard;
    int i;
    if (atapi_count >= IDE_ATAPI_MAX)
        return;
    outb(ctrl, 0x02);
    outb(base + IDE_REG_DRIVE, sel);
    atapi_delay(ctrl);
    st = inb(base + IDE_REG_STATUS);
    if (st == 0xFF)
        return;

    outb(base + IDE_REG_FEATURES, 0);
    outb(base + IDE_REG_SECCOUNT, 0);
    outb(base + IDE_REG_LBA_LOW, 0);
    outb(base + IDE_REG_LBA_MID, 0);
    outb(base + IDE_REG_LBA_HIGH, 0);
    outb(base + IDE_REG_COMMAND, IDE_CMD_IDENTIFY);
    for (i = 0; i < 200000; i++) {
        st = inb(base + IDE_REG_STATUS);
        if (st == 0x00 && i > 40000)
            break;
        if (!(st & IDE_STATUS_BSY) && (st & (IDE_STATUS_DRQ | IDE_STATUS_ERR | IDE_STATUS_RDY)))
            break;
        io_wait();
    }
    mid = inb(base + IDE_REG_LBA_MID);
    hi = inb(base + IDE_REG_LBA_HIGH);
    printf("IDE: io 0x%x sel 0x%02x status 0x%02x sig %02x:%02x\n", base, sel, st, mid, hi);
    if (st & IDE_STATUS_DRQ) {
        for (i = 0; i < 256; i++)
            discard = inw(base + IDE_REG_DATA);
        (void)discard;
    }
    if (!(mid == 0x14 && hi == 0xEB))
        return;

    outb(base + IDE_REG_FEATURES, 0);
    outb(base + IDE_REG_SECCOUNT, 0);
    outb(base + IDE_REG_LBA_LOW, 0);
    outb(base + IDE_REG_LBA_MID, 0);
    outb(base + IDE_REG_LBA_HIGH, 0);
    outb(base + IDE_REG_COMMAND, IDE_CMD_IDENTIFY_PACKET);
    if (atapi_wait_drq(base) != 0) {
        printf("IDE: IDENTIFY PACKET failed at io 0x%x\n", base);
        atapi_reset(ctrl);
        return;
    }
    for (i = 0; i < 256; i++)
        discard = inw(base + IDE_REG_DATA);
    (void)discard;
    (void)inb(base + IDE_REG_STATUS);

    u = &atapi_units[atapi_count];
    memset(u, 0, sizeof(*u));
    u->base = base;
    u->ctrl = ctrl;
    u->sel = sel;
    if (atapi_capacity(u) != 0) {
        printf("IDE: ATAPI at io 0x%x has no capacity\n", base);
        atapi_reset(ctrl);
        return;
    }
    if (atapi_register(u, pci) == 0)
        atapi_count++;
}

void ide_atapi_probe(struct pci_device *dev) {
    uint32_t bar2, bar3;
    uint16_t base, ctrl;
    if (!dev)
        return;
    bar2 = pci_device_read(dev, 0x18, 4);
    bar3 = pci_device_read(dev, 0x1C, 4);
    if ((bar2 & 0xFFFFFFFCu) == 0) {
        base = 0x170;
        ctrl = 0x376;
    } else {
        base = (uint16_t)(bar2 & 0xFFFFFFFCu);
        ctrl = (uint16_t)((bar3 & 0xFFFFFFFCu) + 2);
    }
    printf("IDE: Probing ATAPI on secondary channel io 0x%x\n", base);
    atapi_try(base, ctrl, IDE_DRIVE_MASTER, dev);
    atapi_try(base, ctrl, IDE_DRIVE_SLAVE, dev);
    if (atapi_count == 0)
        printf("IDE: No ATAPI CD-ROM found\n");
}
