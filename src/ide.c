#include "ide.h"
#include "pci.h"
#include "storage.h"
#include "registers.h"
#include "mem.h"
#include "utils.h"
#include "atomic.h"
#include "string.h"
#include "hpet.h"
static void ide_dma_prepare(void);

static ide_controller_t ide_ctrl = {0};
static uint8_t ide_storage_device_id = 0xFF;
static spinlock_t ide_lock = {0};

// Helper functions for I/O operations
static uint8_t ide_read_reg(uint8_t reg) {
    return inb(ide_ctrl.base_port + reg);
}

static void ide_write_reg(uint8_t reg, uint8_t value) {
    outb(ide_ctrl.base_port + reg, value);
}

static void ide_read_data(uint16_t *buffer, uint32_t words) {
    for (uint32_t i = 0; i < words; i++) {
        buffer[i] = inw(ide_ctrl.base_port + IDE_REG_DATA);
    }
}

static void ide_write_data(uint16_t *buffer, uint32_t words) {
    for (uint32_t i = 0; i < words; i++) {
        outw(ide_ctrl.base_port + IDE_REG_DATA, buffer[i]);
    }
}

// Wait for IDE controller to be ready
static int ide_wait_ready() {
    int timeout = 10000;
    
    while (timeout--) {
        uint8_t status = ide_read_reg(IDE_REG_STATUS);
        if (!(status & IDE_STATUS_BSY) && (status & IDE_STATUS_RDY)) {
            return 0;
        }
        // Small delay
        for (volatile int i = 0; i < 100; i++);
    }
    
    return -1; // Timeout
}

// Wait for data ready
static int ide_wait_data() {
    int timeout = 10000;
    
    while (timeout--) {
        uint8_t status = ide_read_reg(IDE_REG_STATUS);
        if (status & IDE_STATUS_DRQ) {
            return 0;
        }
        if (status & IDE_STATUS_ERR) {
            return -1;
        }
        // Small delay
        for (volatile int i = 0; i < 100; i++);
    }
    
    return -1; // Timeout
}

// Identify IDE drive to get capacity
static int ide_identify_drive() {
    if (ide_wait_ready() != 0) {
        printf("IDE: Drive not ready for identify\n");
        return -1;
    }
    
    // Select drive (master)
    ide_write_reg(IDE_REG_DRIVE, IDE_DRIVE_MASTER);
    
    // Send IDENTIFY command
    ide_write_reg(IDE_REG_COMMAND, IDE_CMD_IDENTIFY);
    
    // Wait for data
    if (ide_wait_data() != 0) {
        printf("IDE: IDENTIFY command failed\n");
        return -1;
    }
    
    // Read 256 words of identification data
    uint16_t identify_data[256];
    ide_read_data(identify_data, 256);
    
    // Get sector count from words 60-61 (LBA28) or 100-103 (LBA48)
    uint32_t sectors = identify_data[60] | (identify_data[61] << 16);
    ide_ctrl.lba48 = false;
    if ((identify_data[83] & (1u << 14)) && (identify_data[83] & (1u << 10))) {
        uint64_t n48 = (uint64_t)identify_data[100]
            | ((uint64_t)identify_data[101] << 16)
            | ((uint64_t)identify_data[102] << 32)
            | ((uint64_t)identify_data[103] << 48);
        if (n48 > 0xFFFFFFFFull)
            n48 = 0xFFFFFFFFull;
        if ((uint32_t)n48 > sectors)
            sectors = (uint32_t)n48;
        ide_ctrl.lba48 = true;
    }

    if (sectors == 0) {
        printf("IDE: Drive reports 0 sectors\n");
        return -1;
    }

    ide_ctrl.sectors = sectors;
    uint32_t size_mb = (sectors * 512) / (1024 * 1024);
    
    printf("IDE: Drive capacity: %d sectors (%d MB)\n", sectors, size_mb);
    return 0;
}

void ide_init() {
    printf("IDE: Starting IDE initialization\n");
    struct pci_device* dev = pci_find_device(0x01, 0x01);
    if (dev == NULL) {
        printf("IDE: No IDE controller found (class 01:01)\n");
        return;
    }
    
    printf("IDE: Found IDE controller at bus %d, device %d, func %d\n", 
           dev->bus, dev->device, dev->function);
    
    ide_ctrl.pci_dev = dev;
    
    // Read BAR0 for primary IDE base address
    uint32_t bar0 = pci_device_read(dev, 0x10, 4);
    uint32_t bar1 = pci_device_read(dev, 0x14, 4);
    
    // IDE controllers often use legacy I/O addresses if BARs are 0 or 1
    if ((bar0 & 0xFFFFFFFC) == 0) {  /* BAR unset / legacy (bar0<=1 both mask to 0) */
        ide_ctrl.base_port = 0x1F0;  // Primary IDE legacy base
        ide_ctrl.ctrl_port = 0x3F6;  // Primary IDE control
        printf("IDE: Using legacy I/O addresses (0x1F0)\n");
    } else {
        ide_ctrl.base_port = bar0 & 0xFFFFFFFC;
        ide_ctrl.ctrl_port = (bar1 & 0xFFFFFFFC) + 2;
        printf("IDE: Using PCI BAR addresses (0x%x)\n", ide_ctrl.base_port);
    }
    
    // Enable bus mastering and I/O space
    uint16_t cmd = pci_device_read(dev, 0x04, 2);
    cmd |= 0x05; // Enable I/O space and bus mastering
    pci_device_write(dev, 0x04, 2, cmd);
    
    // Initialize drive
    ide_ctrl.drive = 0; // Master drive
    
    if (ide_identify_drive() != 0) {
        printf("IDE: No ATA disk on primary master\n");
    } else {
        ide_ctrl.initialized = true;
        printf("IDE: Controller initialized successfully\n");
        ide_register_storage();
        ide_dma_prepare();
    }
    /* DVD is the secondary IDE channel (Hyper-V controller 1). */
    ide_atapi_probe(dev);
}

int ide_register_storage() {
    if (!ide_ctrl.initialized) {
        return -1;
    }
    
    // Find a free storage device slot
    extern storage_device_t storage_devices[MAX_STORAGE_DEVICES];
    extern uint8_t storage_device_count;
    
    if (storage_device_count >= MAX_STORAGE_DEVICES) {
        printf("IDE: No free storage device slots\n");
        return -1;
    }
    
    ide_storage_device_id = storage_device_count;
    storage_device_t *ide_device = &storage_devices[ide_storage_device_id];
    
    ide_device->device_id = ide_storage_device_id;
    ide_device->sector_count = ide_ctrl.sectors;
    ide_device->sector_size = 512;
    ide_device->is_initialized = true;
    ide_device->optical = false;
    ide_device->pci_dev = ide_ctrl.pci_dev;
    ide_device->base_address = ide_ctrl.base_port;
    
    // Properly initialize the name field
    memset(ide_device->name, 0, sizeof(ide_device->name));
    const char *name = "IDE Drive";
    for (int i = 0; i < 31 && name[i] != '\0'; i++) {
        ide_device->name[i] = name[i];
    }
    
    storage_device_count++;
    
    printf("IDE: Registered as storage device %d\n", ide_storage_device_id);
    return 0;
}

#define IDE_PRD_MAX 16

typedef struct {
    uint32_t phys;
    uint32_t flags;
} __attribute__((packed)) ide_prd_t;

static int ide_pio_rw(uint64_t lba, uint32_t sector_count, void *buffer, int is_write) {
    uint8_t *bytes = (uint8_t *)buffer;
    uint32_t done = 0;

    while (done < sector_count) {
        uint32_t n = sector_count - done;
        uint64_t cur = lba + done;
        int ext = ide_ctrl.lba48 && (cur > 0x0FFFFFFFull || n > IDE_PIO_MAX_SECTORS);
        if (!ext && cur <= 0x0FFFFFFFull) {
            uint64_t room = 0x10000000ull - cur;
            if (room == 0)
                ext = ide_ctrl.lba48 ? 1 : 0;
            else if (n > room)
                n = (uint32_t)room;
        }
        if (!ext && n > IDE_PIO_MAX_SECTORS)
            n = IDE_PIO_MAX_SECTORS;
        if (ext && n > 65535)
            n = 65535;
        if (n == 0)
            return -1;
        if (ide_wait_ready() != 0)
            return -1;

        if (ext) {
            ide_write_reg(IDE_REG_DRIVE, IDE_DRIVE_MASTER | 0x40);
            ide_write_reg(IDE_REG_SECCOUNT, (uint8_t)((n >> 8) & 0xFF));
            ide_write_reg(IDE_REG_LBA_LOW, (uint8_t)((cur >> 24) & 0xFF));
            ide_write_reg(IDE_REG_LBA_MID, (uint8_t)((cur >> 32) & 0xFF));
            ide_write_reg(IDE_REG_LBA_HIGH, (uint8_t)((cur >> 40) & 0xFF));
            ide_write_reg(IDE_REG_SECCOUNT, (uint8_t)(n & 0xFF));
            ide_write_reg(IDE_REG_LBA_LOW, (uint8_t)(cur & 0xFF));
            ide_write_reg(IDE_REG_LBA_MID, (uint8_t)((cur >> 8) & 0xFF));
            ide_write_reg(IDE_REG_LBA_HIGH, (uint8_t)((cur >> 16) & 0xFF));
            ide_write_reg(IDE_REG_COMMAND, is_write ? IDE_CMD_WRITE_PIO_EXT : IDE_CMD_READ_PIO_EXT);
        } else {
            uint8_t sc = (n == IDE_PIO_MAX_SECTORS) ? 0 : (uint8_t)n;
            ide_write_reg(IDE_REG_DRIVE, (uint8_t)(IDE_DRIVE_MASTER | 0x40 | ((cur >> 24) & 0x0F)));
            ide_write_reg(IDE_REG_SECCOUNT, sc);
            ide_write_reg(IDE_REG_LBA_LOW, (uint8_t)(cur & 0xFF));
            ide_write_reg(IDE_REG_LBA_MID, (uint8_t)((cur >> 8) & 0xFF));
            ide_write_reg(IDE_REG_LBA_HIGH, (uint8_t)((cur >> 16) & 0xFF));
            ide_write_reg(IDE_REG_COMMAND, is_write ? IDE_CMD_WRITE_PIO : IDE_CMD_READ_PIO);
        }

        for (uint32_t i = 0; i < n; i++) {
            if (ide_wait_data() != 0) {
                printf("IDE: PIO %s failed at LBA %u\n", is_write ? "write" : "read",
                       (unsigned)(cur + i));
                return -1;
            }
            uint16_t *w = (uint16_t *)(bytes + (uint64_t)(done + i) * 512u);
            if (is_write)
                ide_write_data(w, 256);
            else
                ide_read_data(w, 256);
        }
        done += n;
    }
    return 0;
}

static int ide_dma_build_prdt(uint32_t bytes) {
    ide_prd_t *prdt = (ide_prd_t *)ide_ctrl.prdt;
    uint8_t *p = ide_ctrl.dma_buf;
    int n = 0;

    if (!prdt || !p || bytes == 0)
        return -1;
    while (bytes) {
        uint64_t phys;
        uint32_t room;
        if (n >= IDE_PRD_MAX)
            return -1;
        phys = virt_to_phys(p);
        if (phys >= 0x100000000ull)
            return -1;
        room = 0x10000u - (uint32_t)(phys & 0xFFFFu);
        if (room > bytes)
            room = bytes;
        room &= ~511u;
        if (room == 0)
            return -1;
        prdt[n].phys = (uint32_t)phys;
        prdt[n].flags = (room >= 0x10000u) ? 0 : room;
        p += room;
        bytes -= room;
        n++;
    }
    prdt[n - 1].flags |= 0x80000000u;
    return n;
}

static int ide_dma_wait(void) {
    uint64_t spins = 0;
    uint64_t start = hpet_get_time_us();

    for (;;) {
        uint8_t bm = inb((uint16_t)(ide_ctrl.bm_base + IDE_BM_STATUS));
        uint8_t st = ide_read_reg(IDE_REG_STATUS);
        if ((bm & IDE_BM_STATUS_ERR) || (st & (IDE_STATUS_ERR | IDE_STATUS_DF)))
            return -1;
        if ((bm & IDE_BM_STATUS_IRQ) && !(st & IDE_STATUS_BSY))
            return 0;
        if (!(bm & IDE_BM_STATUS_ACT) && !(st & IDE_STATUS_BSY) &&
            (st & IDE_STATUS_RDY) && !(st & IDE_STATUS_DRQ))
            return 0;
        if (++spins > 8000000ull)
            return -1;
        if ((spins & 0x3FFu) == 0) {
            uint64_t now = hpet_get_time_us();
            if (start && now > start && now - start > 3000000ull)
                return -1;
        }
    }
}

/* Bus-master WRITE/READ DMA. Caller owns ide_lock. Buffer is ide_ctrl.dma_buf. */
static int ide_dma_rw(uint64_t lba, uint32_t count, int is_write) {
    uint8_t dir;
    int ext;

    if (!ide_ctrl.bm_base || !ide_ctrl.dma_buf || !ide_ctrl.prdt)
        return -1;
    if (count == 0 || count > IDE_DMA_MAX_SECTORS)
        return -1;
    if (ide_dma_build_prdt(count * 512u) < 0)
        return -1;

    dir = is_write ? 0 : IDE_BM_CMD_READ;
    outb((uint16_t)(ide_ctrl.bm_base + IDE_BM_CMD), 0);
    outl((uint16_t)(ide_ctrl.bm_base + IDE_BM_PRDT), (uint32_t)virt_to_phys(ide_ctrl.prdt));
    outb((uint16_t)(ide_ctrl.bm_base + IDE_BM_CMD), dir);
    outb((uint16_t)(ide_ctrl.bm_base + IDE_BM_STATUS),
         (uint8_t)(IDE_BM_STATUS_ERR | IDE_BM_STATUS_IRQ));

    if (ide_wait_ready() != 0)
        return -1;

    ext = ide_ctrl.lba48 || (lba + count) > 0x0FFFFFFFull || count > 256;
    if (ext && !ide_ctrl.lba48)
        return -1;
    if (ext) {
        ide_write_reg(IDE_REG_DRIVE, IDE_DRIVE_MASTER | 0x40);
        ide_write_reg(IDE_REG_SECCOUNT, (uint8_t)((count >> 8) & 0xFF));
        ide_write_reg(IDE_REG_LBA_LOW, (uint8_t)((lba >> 24) & 0xFF));
        ide_write_reg(IDE_REG_LBA_MID, (uint8_t)((lba >> 32) & 0xFF));
        ide_write_reg(IDE_REG_LBA_HIGH, (uint8_t)((lba >> 40) & 0xFF));
        ide_write_reg(IDE_REG_SECCOUNT, (uint8_t)(count & 0xFF));
        ide_write_reg(IDE_REG_LBA_LOW, (uint8_t)(lba & 0xFF));
        ide_write_reg(IDE_REG_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
        ide_write_reg(IDE_REG_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFF));
        ide_write_reg(IDE_REG_COMMAND, is_write ? IDE_CMD_WRITE_DMA_EXT : IDE_CMD_READ_DMA_EXT);
    } else {
        uint8_t sc = (count == 256) ? 0 : (uint8_t)count;
        ide_write_reg(IDE_REG_DRIVE, (uint8_t)(IDE_DRIVE_MASTER | 0x40 | ((lba >> 24) & 0x0F)));
        ide_write_reg(IDE_REG_SECCOUNT, sc);
        ide_write_reg(IDE_REG_LBA_LOW, (uint8_t)(lba & 0xFF));
        ide_write_reg(IDE_REG_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
        ide_write_reg(IDE_REG_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFF));
        ide_write_reg(IDE_REG_COMMAND, is_write ? IDE_CMD_WRITE_DMA : IDE_CMD_READ_DMA);
    }

    outb((uint16_t)(ide_ctrl.bm_base + IDE_BM_CMD), (uint8_t)(IDE_BM_CMD_START | dir));
    if (ide_dma_wait() != 0) {
        outb((uint16_t)(ide_ctrl.bm_base + IDE_BM_CMD), 0);
        outb((uint16_t)(ide_ctrl.bm_base + IDE_BM_STATUS),
             (uint8_t)(IDE_BM_STATUS_ERR | IDE_BM_STATUS_IRQ));
        return -1;
    }
    outb((uint16_t)(ide_ctrl.bm_base + IDE_BM_CMD), 0);
    outb((uint16_t)(ide_ctrl.bm_base + IDE_BM_STATUS),
         (uint8_t)(IDE_BM_STATUS_ERR | IDE_BM_STATUS_IRQ));
    return 0;
}

static void ide_dma_prepare(void) {
    uint32_t bar4;
    uint16_t bm;
    uint8_t *buf;
    uint8_t *raw;
    uint8_t *prdt;
    uint8_t pio[512];
    ide_ctrl.dma_ok = false;
    ide_ctrl.bm_base = 0;
    ide_ctrl.dma_buf = NULL;
    ide_ctrl.prdt = NULL;
    if (!ide_ctrl.pci_dev)
        return;

    bar4 = pci_device_read(ide_ctrl.pci_dev, 0x20, 4);
    if ((bar4 & 1u) == 0 || (bar4 & 0xFFFCu) == 0) {
        printf("IDE: no bus-master I/O BAR (0x%x); writes use multi-sector PIO\n", bar4);
        return;
    }
    bm = (uint16_t)(bar4 & 0xFFFCu);
    ide_ctrl.bm_base = bm;

    buf = (uint8_t *)kalloc(IDE_DMA_MAX_SECTORS * 512u);
    raw = (uint8_t *)kalloc(4096);
    if (!buf || !raw) {
        printf("IDE: DMA buffer alloc failed; writes use multi-sector PIO\n");
        return;
    }
    if (virt_to_phys(buf) >= 0x100000000ull ||
        virt_to_phys(buf) + (uint64_t)IDE_DMA_MAX_SECTORS * 512u > 0x100000000ull) {
        printf("IDE: DMA buffer is above 4 GiB; writes use multi-sector PIO\n");
        return;
    }
    prdt = NULL;
    for (uint32_t off = 0; off + 256 <= 4096; off += 8) {
        uint64_t phys = virt_to_phys(raw + off);
        if (phys < 0x100000000ull && (phys & 7u) == 0 &&
            (phys & 0xFFFFu) + 256u <= 0x10000u) {
            prdt = raw + off;
            break;
        }
    }
    if (!prdt) {
        printf("IDE: PRDT alignment failed; writes use multi-sector PIO\n");
        return;
    }
    ide_ctrl.dma_buf = buf;
    ide_ctrl.prdt = prdt;

    if (ide_pio_rw(0, 1, pio, 0) != 0) {
        printf("IDE: PIO probe read failed; DMA left off\n");
        return;
    }
    if (ide_dma_rw(0, 1, 0) != 0 || memcmp(pio, ide_ctrl.dma_buf, 512) != 0) {
        printf("IDE: DMA verify failed; writes use %u-sector PIO\n", IDE_PIO_MAX_SECTORS);
        return;
    }
    ide_ctrl.dma_ok = true;
    printf("IDE: bus-master DMA writes, %u sectors (%u KiB) per command\n",
           IDE_DMA_MAX_SECTORS, (IDE_DMA_MAX_SECTORS * 512u) / 1024u);
}

int ide_read_sectors(storage_device_t *dev, uint64_t lba, uint32_t sector_count, void *buffer) {
    klock(&ide_lock);
    if (dev && dev->optical) {
        int rc = ide_atapi_read(dev, lba, sector_count, buffer);
        kunlock(&ide_lock);
        return rc;
    }
    if (!ide_ctrl.initialized || !dev || !buffer || sector_count == 0) {
        printf("IDE: Invalid parameters for read\n");
        kunlock(&ide_lock);
        return -1;
    }
    if (ide_wait_ready() != 0) {
        printf("IDE: Controller not ready for read\n");
        kunlock(&ide_lock);
        return -1;
    }
    if (lba + sector_count > ide_ctrl.sectors) {
        printf("IDE: Read beyond drive capacity\n");
        kunlock(&ide_lock);
        return -1;
    }

    if (ide_ctrl.dma_ok) {
        uint8_t *dst = (uint8_t *)buffer;
        uint32_t done = 0;
        while (done < sector_count) {
            uint32_t n = sector_count - done;
            if (n > IDE_DMA_MAX_SECTORS)
                n = IDE_DMA_MAX_SECTORS;
            if (ide_dma_rw(lba + done, n, 0) != 0) {
                printf("IDE: DMA read failed, falling back to PIO\n");
                ide_ctrl.dma_ok = false;
                break;
            }
            memcpy(dst + (uint64_t)done * 512u, ide_ctrl.dma_buf, n * 512u);
            done += n;
        }
        if (ide_ctrl.dma_ok) {
            kunlock(&ide_lock);
            return 0;
        }
        if (done) {
            /* Partial DMA success already copied. Finish the tail with PIO. */
            int rc = ide_pio_rw(lba + done, sector_count - done, dst + (uint64_t)done * 512u, 0);
            kunlock(&ide_lock);
            return rc;
        }
    }

    int rc = ide_pio_rw(lba, sector_count, buffer, 0);
    kunlock(&ide_lock);
    return rc;
}

int ide_write_sectors(storage_device_t *dev, uint64_t lba, uint32_t sector_count, const void *buffer) {
    klock(&ide_lock);
    if (!ide_ctrl.initialized || !dev || !buffer || dev->optical || sector_count == 0) {
        kunlock(&ide_lock);
        return -1;
    }
    if (dev->device_id != ide_storage_device_id) {
        kunlock(&ide_lock);
        return -1;
    }
    if (lba + sector_count > ide_ctrl.sectors) {
        printf("IDE: Write beyond drive capacity\n");
        kunlock(&ide_lock);
        return -1;
    }

    const uint8_t *src = (const uint8_t *)buffer;
    uint32_t done = 0;
    while (done < sector_count) {
        uint32_t n = sector_count - done;
        if (ide_ctrl.dma_ok) {
            if (n > IDE_DMA_MAX_SECTORS)
                n = IDE_DMA_MAX_SECTORS;
            memcpy(ide_ctrl.dma_buf, src + (uint64_t)done * 512u, n * 512u);
            if (ide_dma_rw(lba + done, n, 1) != 0) {
                printf("IDE: DMA write failed at LBA %llu (%u sectors), using PIO\n",
                       (unsigned long long)(lba + done), n);
                ide_ctrl.dma_ok = false;
            } else {
                done += n;
                continue;
            }
        }
        if (n > IDE_PIO_MAX_SECTORS)
            n = IDE_PIO_MAX_SECTORS;
        if (ide_pio_rw(lba + done, n, (void *)(src + (uint64_t)done * 512u), 1) != 0) {
            kunlock(&ide_lock);
            return -1;
        }
        done += n;
    }
    kunlock(&ide_lock);
    return 0;
}

int ide_flush(storage_device_t *dev) {
    // IDE doesn't need explicit flush for PIO mode
    return 0;
} 