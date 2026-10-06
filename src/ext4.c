/**
 * @file ext4.c
 * @brief EXT4 filesystem driver – extent-aware, read-write
 *
 * This implementation extends the ext2 driver with:
 *  - Extent tree traversal for block mapping
 *  - Block & inode allocation via bitmap
 *  - File creation / deletion
 *  - Directory entry insertion / removal
 *  - Superblock / BGDT write-back
 */

#include "ext4.h"
#include "mem.h"
#include "serial.h"
#include "string.h"
#include "atomic.h"

extern int  printf(const char *fmt, ...);
extern int  snprintf(char *str, size_t size, const char *fmt, ...);
extern void *memset(void *, int, size_t);
extern void *memcpy(void *, const void *, size_t);
extern int   strcmp(const char *a, const char *b);
extern int   strncmp(const char *a, const char *b, size_t n);
extern size_t strlen(const char *s);
extern char *strcpy(char *dst, const char *src);
extern char *strncpy(char *dst, const char *src, size_t n);

/* ================================================================
 *  Global state (one mounted ext4 at a time – mirrors ext2 pattern)
 * ================================================================ */

static ext4_fs_t ext4_fs;
static spinlock_t ext4_lock;
static int ext4_lock_depth;

void ext4_enter(void) {
    if (ext4_lock_depth == 0)
        klock(&ext4_lock);
    ext4_lock_depth++;
}

void ext4_leave(void) {
    if (ext4_lock_depth > 0 && --ext4_lock_depth == 0)
        kunlock(&ext4_lock);
}

/* ================================================================
 *  Low-level block I/O
 * ================================================================ */

int ext4_read_block(uint64_t block_num, void *buffer) {
    if (!ext4_fs.mounted || !ext4_fs.storage_dev) return -1;

    uint32_t sectors_per_block = ext4_fs.block_size / SECTOR_SIZE;
    uint64_t lba = ext4_fs.partition_lba + block_num * sectors_per_block;

    return storage_read_sectors(ext4_fs.storage_dev->device_id, lba,
                                sectors_per_block, buffer);
}

int ext4_write_block(uint64_t block_num, const void *buffer) {
    if (!ext4_fs.mounted || !ext4_fs.storage_dev) return -1;

    uint32_t sectors_per_block = ext4_fs.block_size / SECTOR_SIZE;
    uint64_t lba = ext4_fs.partition_lba + block_num * sectors_per_block;

    return storage_write_sectors(ext4_fs.storage_dev->device_id, lba,
                                 sectors_per_block, buffer);
}

/* ================================================================
 *  Mounting
 * ================================================================ */

int ext4_mount(uint8_t device_id, uint32_t partition_lba) {
    int rc = -1;
    ext4_enter();
    if (ext4_fs.mounted) {
        printf("ext4: already mounted\n");
        goto out;
    }

    memset(&ext4_fs, 0, sizeof(ext4_fs));
    ext4_fs.partition_lba = partition_lba;

    /* Resolve storage device */
    ext4_fs.storage_dev = storage_get_device(device_id);
    if (!ext4_fs.storage_dev) {
        printf("ext4: storage device %u not found\n", device_id);
        goto out;
    }

    /* Read superblock (1024 bytes at byte offset 1024 from partition start) */
    uint8_t sb_buf[2048];   /* need 4 sectors starting from +2 */
    uint32_t sb_lba = partition_lba + 2;  /* sector 2 = byte 1024 */
    if (storage_read_sectors(device_id, sb_lba, 4, sb_buf) != 0) {
        printf("ext4: failed to read superblock\n");
        goto out;
    }

    memcpy(&ext4_fs.superblock, sb_buf, sizeof(ext2_superblock_t));

    if (ext4_fs.superblock.s_magic != EXT2_MAGIC) {
        printf("ext4: bad magic 0x%04x (expected 0xEF53)\n", ext4_fs.superblock.s_magic);
        goto out;
    }

    ext4_fs.block_size       = 1024U << ext4_fs.superblock.s_log_block_size;
    ext4_fs.inode_size       = ext4_fs.superblock.s_inode_size;
    ext4_fs.inodes_per_group = ext4_fs.superblock.s_inodes_per_group;
    ext4_fs.blocks_per_group = ext4_fs.superblock.s_blocks_per_group;
    ext4_fs.first_data_block = ext4_fs.superblock.s_first_data_block;

    /* Total block count (ext4 64-bit aware) */
    ext4_fs.total_blocks = ext4_fs.superblock.s_blocks_count;
    ext4_fs.has_extents  = (ext4_fs.superblock.s_feature_incompat & EXT4_FEATURE_INCOMPAT_EXTENTS) != 0;
    ext4_fs.has_64bit    = (ext4_fs.superblock.s_feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) != 0;

    if (ext4_fs.has_64bit) {
        /* Read 64-bit block count from extra superblock fields at offset 0x150 */
        uint32_t hi = 0;
        memcpy(&hi, sb_buf + 0x150, 4);
        ext4_fs.total_blocks |= ((uint64_t)hi << 32);
    }

    /* Number of block groups */
    ext4_fs.num_block_groups =
        (ext4_fs.superblock.s_blocks_count + ext4_fs.blocks_per_group - 1)
        / ext4_fs.blocks_per_group;

    /* Read block group descriptor table */
    uint32_t bgdt_block = ext4_fs.first_data_block + 1;
    uint32_t bgdt_size  = ext4_fs.num_block_groups * sizeof(ext2_block_group_desc_t);
    uint32_t bgdt_blocks = (bgdt_size + ext4_fs.block_size - 1) / ext4_fs.block_size;

    ext4_fs.bgdt = (ext2_block_group_desc_t *)kalloc(bgdt_blocks * ext4_fs.block_size);
    if (!ext4_fs.bgdt) {
        printf("ext4: out of memory for BGDT\n");
        goto out;
    }

    for (uint32_t i = 0; i < bgdt_blocks; i++) {
        if (ext4_read_block(bgdt_block + i,
                            (uint8_t *)ext4_fs.bgdt + i * ext4_fs.block_size) != 0) {
            printf("ext4: failed to read BGDT block %u\n", bgdt_block + i);
            kfree(ext4_fs.bgdt);
            ext4_fs.bgdt = NULL;
            goto out;
        }
    }

    ext4_fs.mounted = true;
    serial_printf("ext4: mounted (block_size=%u, inodes=%u, blocks=%llu, extents=%s)\n",
                  ext4_fs.block_size,
                  ext4_fs.superblock.s_inodes_count,
                  (unsigned long long)ext4_fs.total_blocks,
                  ext4_fs.has_extents ? "yes" : "no");
    rc = 0;
out:
    ext4_leave();
    return rc;
}

void ext4_unmount(void) {
    ext4_enter();
    if (!ext4_fs.mounted) {
        ext4_leave();
        return;
    }
    ext4_sync();
    if (ext4_fs.bgdt) {
        kfree(ext4_fs.bgdt);
        ext4_fs.bgdt = NULL;
    }
    ext4_fs.mounted = false;
    serial_printf("ext4: unmounted\n");
    ext4_leave();
}

bool ext4_is_mounted(void) { return ext4_fs.mounted; }

/* ================================================================
 *  Inode read / write
 * ================================================================ */

int ext4_read_inode(uint32_t inode_num, ext2_inode_t *out) {
    if (!ext4_fs.mounted || inode_num == 0) return -1;

    uint32_t group = (inode_num - 1) / ext4_fs.inodes_per_group;
    uint32_t index = (inode_num - 1) % ext4_fs.inodes_per_group;

    if (group >= ext4_fs.num_block_groups) return -1;

    uint32_t inode_table_block = ext4_fs.bgdt[group].bg_inode_table;
    uint32_t offset_in_table   = index * ext4_fs.inode_size;
    uint32_t block_containing   = inode_table_block + (offset_in_table / ext4_fs.block_size);
    uint32_t offset_in_block    = offset_in_table % ext4_fs.block_size;

    uint8_t *block_buf = (uint8_t *)kalloc(ext4_fs.block_size);
    if (!block_buf) return -1;

    if (ext4_read_block(block_containing, block_buf) != 0) {
        kfree(block_buf);
        return -1;
    }

    memcpy(out, block_buf + offset_in_block,
           sizeof(ext2_inode_t) < ext4_fs.inode_size ? sizeof(ext2_inode_t) : ext4_fs.inode_size);

    kfree(block_buf);
    return 0;
}

int ext4_write_inode(uint32_t inode_num, const ext2_inode_t *inode) {
    if (!ext4_fs.mounted || inode_num == 0) return -1;

    uint32_t group = (inode_num - 1) / ext4_fs.inodes_per_group;
    uint32_t index = (inode_num - 1) % ext4_fs.inodes_per_group;

    if (group >= ext4_fs.num_block_groups) return -1;

    uint32_t inode_table_block = ext4_fs.bgdt[group].bg_inode_table;
    uint32_t offset_in_table   = index * ext4_fs.inode_size;
    uint32_t block_containing   = inode_table_block + (offset_in_table / ext4_fs.block_size);
    uint32_t offset_in_block    = offset_in_table % ext4_fs.block_size;

    uint8_t *block_buf = (uint8_t *)kalloc(ext4_fs.block_size);
    if (!block_buf) return -1;

    if (ext4_read_block(block_containing, block_buf) != 0) {
        kfree(block_buf);
        return -1;
    }

    uint32_t copy_size = sizeof(ext2_inode_t);
    if (copy_size > ext4_fs.inode_size) copy_size = ext4_fs.inode_size;
    memcpy(block_buf + offset_in_block, inode, copy_size);

    int ret = ext4_write_block(block_containing, block_buf);
    kfree(block_buf);
    return ret;
}

/* ================================================================
 *  Extent tree traversal
 * ================================================================ */

uint64_t ext4_extent_get_pblock(const ext4_extent_t *ext) {
    return ((uint64_t)ext->ee_start_hi << 32) | (uint64_t)ext->ee_start_lo;
}

static uint64_t ext4_idx_get_pblock(const ext4_extent_idx_t *idx) {
    return ((uint64_t)idx->ei_leaf_hi << 32) | (uint64_t)idx->ei_leaf_lo;
}

int ext4_extent_lookup(const ext2_inode_t *inode, uint32_t logical_block,
                       uint64_t *out_physical) {
    /* The extent header is stored in inode->i_block[0..2] (first 12 bytes) */
    const ext4_extent_header_t *eh = (const ext4_extent_header_t *)inode->i_block;

    if (eh->eh_magic != EXT4_EXT_MAGIC) {
        /* Not extent-based – fall back to classic indirect block mapping */
        *out_physical = 0;
        return -1;
    }

    /* Walk the tree from root down */
    uint8_t *node_buf = NULL;    /* for reading child nodes */
    const uint8_t *current = (const uint8_t *)inode->i_block;

    for (;;) {
        eh = (const ext4_extent_header_t *)current;

        if (eh->eh_depth == 0) {
            /* Leaf level – search for the extent containing logical_block */
            const ext4_extent_t *exts = (const ext4_extent_t *)(current + sizeof(ext4_extent_header_t));
            for (uint16_t i = 0; i < eh->eh_entries; i++) {
                uint32_t start = exts[i].ee_block;
                uint16_t len   = exts[i].ee_len & 0x7FFF;   /* mask uninitialised flag */
                if (logical_block >= start && logical_block < start + len) {
                    *out_physical = ext4_extent_get_pblock(&exts[i]) + (logical_block - start);
                    if (node_buf) kfree(node_buf);
                    return 0;
                }
            }
            /* Not found */
            if (node_buf) kfree(node_buf);
            *out_physical = 0;
            return -1;
        }

        /* Internal level – binary search for the right child */
        const ext4_extent_idx_t *idxs = (const ext4_extent_idx_t *)(current + sizeof(ext4_extent_header_t));
        int best = -1;
        for (uint16_t i = 0; i < eh->eh_entries; i++) {
            if (logical_block >= idxs[i].ei_block) best = i;
        }
        if (best < 0) {
            if (node_buf) kfree(node_buf);
            *out_physical = 0;
            return -1;
        }

        /* Read the child node */
        uint64_t child_block = ext4_idx_get_pblock(&idxs[best]);
        if (!node_buf) node_buf = (uint8_t *)kalloc(ext4_fs.block_size);
        if (!node_buf) { *out_physical = 0; return -1; }
        if (ext4_read_block(child_block, node_buf) != 0) {
            kfree(node_buf);
            *out_physical = 0;
            return -1;
        }
        current = node_buf;
    }
}

/* ================================================================
 *  Classic indirect block mapping (fallback for non-extent inodes)
 * ================================================================ */

static int ext4_classic_block_lookup(const ext2_inode_t *inode, uint32_t logical_block,
                                     uint64_t *out_physical) {
    uint32_t ptrs_per_block = ext4_fs.block_size / 4;

    /* Direct blocks (0-11) */
    if (logical_block < 12) {
        *out_physical = inode->i_block[logical_block];
        return (*out_physical != 0) ? 0 : -1;
    }
    logical_block -= 12;

    /* Single indirect (block 12) */
    if (logical_block < ptrs_per_block) {
        uint32_t *buf = (uint32_t *)kalloc(ext4_fs.block_size);
        if (!buf) return -1;
        if (ext4_read_block(inode->i_block[12], buf) != 0) { kfree(buf); return -1; }
        *out_physical = buf[logical_block];
        kfree(buf);
        return (*out_physical != 0) ? 0 : -1;
    }
    logical_block -= ptrs_per_block;

    /* Double indirect (block 13) */
    if (logical_block < ptrs_per_block * ptrs_per_block) {
        uint32_t *buf1 = (uint32_t *)kalloc(ext4_fs.block_size);
        if (!buf1) return -1;
        if (ext4_read_block(inode->i_block[13], buf1) != 0) { kfree(buf1); return -1; }
        uint32_t idx1 = logical_block / ptrs_per_block;
        uint32_t idx2 = logical_block % ptrs_per_block;
        uint32_t blk2 = buf1[idx1];
        if (ext4_read_block(blk2, buf1) != 0) { kfree(buf1); return -1; }
        *out_physical = buf1[idx2];
        kfree(buf1);
        return (*out_physical != 0) ? 0 : -1;
    }
    logical_block -= ptrs_per_block * ptrs_per_block;

    /* Triple indirect (block 14) */
    if (logical_block < (uint64_t)ptrs_per_block * ptrs_per_block * ptrs_per_block) {
        uint32_t *buf = (uint32_t *)kalloc(ext4_fs.block_size);
        if (!buf) return -1;
        if (ext4_read_block(inode->i_block[14], buf) != 0) { kfree(buf); return -1; }
        uint32_t idx1 = logical_block / (ptrs_per_block * ptrs_per_block);
        uint32_t rem  = logical_block % (ptrs_per_block * ptrs_per_block);
        uint32_t blk2 = buf[idx1];
        if (ext4_read_block(blk2, buf) != 0) { kfree(buf); return -1; }
        uint32_t idx2 = rem / ptrs_per_block;
        uint32_t idx3 = rem % ptrs_per_block;
        uint32_t blk3 = buf[idx2];
        if (ext4_read_block(blk3, buf) != 0) { kfree(buf); return -1; }
        *out_physical = buf[idx3];
        kfree(buf);
        return (*out_physical != 0) ? 0 : -1;
    }

    *out_physical = 0;
    return -1;
}

/* Unified block lookup – tries extents first, falls back to classic */
static int ext4_block_lookup(const ext2_inode_t *inode, uint32_t logical_block,
                             uint64_t *out_physical) {
    if (ext4_fs.has_extents && (inode->i_flags & 0x80000)) {
        /* EXT4_EXTENTS_FL = 0x80000 */
        return ext4_extent_lookup(inode, logical_block, out_physical);
    }
    return ext4_classic_block_lookup(inode, logical_block, out_physical);
}

/* ================================================================
 *  File read
 * ================================================================ */

int ext4_read_file(uint32_t inode_num, void *buffer, uint32_t offset, uint32_t size) {
    if (!ext4_fs.mounted) return -1;

    ext2_inode_t inode;
    if (ext4_read_inode(inode_num, &inode) != 0) return -1;

    uint32_t file_size = inode.i_size;
    /* 64-bit size for regular files */
    if ((inode.i_mode & EXT2_S_IFREG) && inode.i_dir_acl)
        file_size = inode.i_size;  /* just use lower 32 for now */

    if (offset >= file_size) return 0;
    if (offset + size > file_size) size = file_size - offset;

    uint8_t *block_buf = (uint8_t *)kalloc(ext4_fs.block_size);
    if (!block_buf) return -1;

    uint32_t bytes_read = 0;
    uint8_t *dst = (uint8_t *)buffer;

    while (bytes_read < size) {
        uint32_t logical_block = (offset + bytes_read) / ext4_fs.block_size;
        uint32_t block_offset  = (offset + bytes_read) % ext4_fs.block_size;
        uint32_t chunk = ext4_fs.block_size - block_offset;
        if (chunk > size - bytes_read) chunk = size - bytes_read;

        uint64_t pblock;
        if (ext4_block_lookup(&inode, logical_block, &pblock) != 0 || pblock == 0) {
            /* Sparse / hole – fill with zeros */
            memset(dst + bytes_read, 0, chunk);
        } else {
            if (ext4_read_block(pblock, block_buf) != 0) {
                kfree(block_buf);
                return -1;
            }
            memcpy(dst + bytes_read, block_buf + block_offset, chunk);
        }
        bytes_read += chunk;
    }

    kfree(block_buf);
    return (int)bytes_read;
}

/* ================================================================
 *  Block allocation (bitmap-based)
 * ================================================================ */

uint64_t ext4_alloc_block(void) {
    if (!ext4_fs.mounted) return 0;

    uint8_t *bitmap = (uint8_t *)kalloc(ext4_fs.block_size);
    if (!bitmap) return 0;

    for (uint32_t g = 0; g < ext4_fs.num_block_groups; g++) {
        if (ext4_fs.bgdt[g].bg_free_blocks_count == 0) continue;

        if (ext4_read_block(ext4_fs.bgdt[g].bg_block_bitmap, bitmap) != 0) continue;

        for (uint32_t b = 0; b < ext4_fs.blocks_per_group; b++) {
            uint32_t byte_idx = b / 8;
            uint8_t  bit_mask = 1U << (b % 8);
            if (!(bitmap[byte_idx] & bit_mask)) {
                /* Found free block */
                bitmap[byte_idx] |= bit_mask;
                ext4_write_block(ext4_fs.bgdt[g].bg_block_bitmap, bitmap);
                ext4_fs.bgdt[g].bg_free_blocks_count--;
                ext4_fs.superblock.s_free_blocks_count--;
                kfree(bitmap);
                return (uint64_t)g * ext4_fs.blocks_per_group + b + ext4_fs.first_data_block;
            }
        }
    }

    kfree(bitmap);
    return 0;  /* no free blocks */
}

void ext4_free_block(uint64_t block_num) {
    if (!ext4_fs.mounted || block_num == 0) return;

    uint64_t rel = block_num - ext4_fs.first_data_block;
    uint32_t g   = (uint32_t)(rel / ext4_fs.blocks_per_group);
    uint32_t b   = (uint32_t)(rel % ext4_fs.blocks_per_group);

    if (g >= ext4_fs.num_block_groups) return;

    uint8_t *bitmap = (uint8_t *)kalloc(ext4_fs.block_size);
    if (!bitmap) return;
    if (ext4_read_block(ext4_fs.bgdt[g].bg_block_bitmap, bitmap) != 0) { kfree(bitmap); return; }

    bitmap[b / 8] &= ~(1U << (b % 8));
    ext4_write_block(ext4_fs.bgdt[g].bg_block_bitmap, bitmap);
    ext4_fs.bgdt[g].bg_free_blocks_count++;
    ext4_fs.superblock.s_free_blocks_count++;
    kfree(bitmap);
}

/* ================================================================
 *  Inode allocation / deallocation
 * ================================================================ */

uint32_t ext4_alloc_inode(void) {
    if (!ext4_fs.mounted) return 0;

    uint8_t *bitmap = (uint8_t *)kalloc(ext4_fs.block_size);
    if (!bitmap) return 0;

    for (uint32_t g = 0; g < ext4_fs.num_block_groups; g++) {
        if (ext4_fs.bgdt[g].bg_free_inodes_count == 0) continue;

        if (ext4_read_block(ext4_fs.bgdt[g].bg_inode_bitmap, bitmap) != 0) continue;

        for (uint32_t i = 0; i < ext4_fs.inodes_per_group; i++) {
            uint32_t byte_idx = i / 8;
            uint8_t  bit_mask = 1U << (i % 8);
            if (!(bitmap[byte_idx] & bit_mask)) {
                bitmap[byte_idx] |= bit_mask;
                ext4_write_block(ext4_fs.bgdt[g].bg_inode_bitmap, bitmap);
                ext4_fs.bgdt[g].bg_free_inodes_count--;
                ext4_fs.superblock.s_free_inodes_count--;
                kfree(bitmap);
                return g * ext4_fs.inodes_per_group + i + 1;
            }
        }
    }

    kfree(bitmap);
    return 0;
}

void ext4_free_inode(uint32_t inode_num) {
    if (!ext4_fs.mounted || inode_num == 0) return;

    uint32_t g = (inode_num - 1) / ext4_fs.inodes_per_group;
    uint32_t i = (inode_num - 1) % ext4_fs.inodes_per_group;

    if (g >= ext4_fs.num_block_groups) return;

    uint8_t *bitmap = (uint8_t *)kalloc(ext4_fs.block_size);
    if (!bitmap) return;
    if (ext4_read_block(ext4_fs.bgdt[g].bg_inode_bitmap, bitmap) != 0) { kfree(bitmap); return; }

    bitmap[i / 8] &= ~(1U << (i % 8));
    ext4_write_block(ext4_fs.bgdt[g].bg_inode_bitmap, bitmap);
    ext4_fs.bgdt[g].bg_free_inodes_count++;
    ext4_fs.superblock.s_free_inodes_count++;
    kfree(bitmap);
}

/* ================================================================
 *  File write
 * ================================================================ */

int ext4_write_file(uint32_t inode_num, const void *buffer, uint32_t offset, uint32_t size) {
    if (!ext4_fs.mounted) return -1;

    ext2_inode_t inode;
    if (ext4_read_inode(inode_num, &inode) != 0) return -1;

    uint8_t *block_buf = (uint8_t *)kalloc(ext4_fs.block_size);
    if (!block_buf) return -1;

    const uint8_t *src = (const uint8_t *)buffer;
    uint32_t written = 0;

    while (written < size) {
        uint32_t logical_block = (offset + written) / ext4_fs.block_size;
        uint32_t block_offset  = (offset + written) % ext4_fs.block_size;
        uint32_t chunk = ext4_fs.block_size - block_offset;
        if (chunk > size - written) chunk = size - written;

        uint64_t pblock;
        if (ext4_block_lookup(&inode, logical_block, &pblock) != 0 || pblock == 0) {
            /* Need to allocate a new block */
            pblock = ext4_alloc_block();
            if (pblock == 0) {
                kfree(block_buf);
                return -1;
            }

            /* For classic (non-extent) inodes, assign direct block pointers */
            if (!(inode.i_flags & 0x80000) && logical_block < 12) {
                inode.i_block[logical_block] = (uint32_t)pblock;
            } else if (inode.i_flags & 0x80000) {
                /* Extent tree insertion is not implemented. Fail closed
                 * rather than writing to an orphaned allocated block and
                 * reporting success (would corrupt/leak space). */
                ext4_free_block(pblock);
                kfree(block_buf);
                return -1;  /* ENOSYS-equivalent: extent grow unsupported */
            } else {
                /* Classic inode but beyond direct blocks (indirect not wired) */
                ext4_free_block(pblock);
                kfree(block_buf);
                return -1;
            }
            memset(block_buf, 0, ext4_fs.block_size);
        } else {
            /* Read existing block for partial overwrite */
            if (block_offset != 0 || chunk < ext4_fs.block_size) {
                if (ext4_read_block(pblock, block_buf) != 0) {
                    kfree(block_buf);
                    return -1;
                }
            }
        }

        memcpy(block_buf + block_offset, src + written, chunk);
        if (ext4_write_block(pblock, block_buf) != 0) {
            kfree(block_buf);
            return -1;
        }

        written += chunk;
    }

    /* Update file size if we wrote past the end */
    if (offset + size > inode.i_size) {
        inode.i_size = offset + size;
        inode.i_blocks = ((inode.i_size + ext4_fs.block_size - 1) / ext4_fs.block_size)
                         * (ext4_fs.block_size / 512);
    }
    ext4_write_inode(inode_num, &inode);

    kfree(block_buf);
    return (int)written;
}

/* ================================================================
 *  Path lookup
 * ================================================================ */

uint32_t ext4_lookup_path(const char *path) {
    if (!ext4_fs.mounted || !path) return 0;
    if (path[0] == '/' && path[1] == '\0') return EXT2_ROOT_INO;

    uint32_t current_inode = EXT2_ROOT_INO;
    const char *p = path;
    if (*p == '/') p++;

    char component[256];
    while (*p) {
        /* Extract next path component */
        int len = 0;
        while (*p && *p != '/' && len < 255) {
            component[len++] = *p++;
        }
        component[len] = '\0';
        if (*p == '/') p++;
        if (len == 0) continue;

        /* Read current directory inode */
        ext2_inode_t dir_inode;
        if (ext4_read_inode(current_inode, &dir_inode) != 0) return 0;

        if (!(dir_inode.i_mode & EXT2_S_IFDIR)) return 0;  /* not a directory */

        /* Search directory entries */
        uint32_t dir_size = dir_inode.i_size;
        uint8_t *dir_buf = (uint8_t *)kalloc(dir_size < ext4_fs.block_size ? ext4_fs.block_size : dir_size);
        if (!dir_buf) return 0;

        /* Read directory blocks */
        uint32_t total_read = 0;
        uint32_t lb = 0;
        while (total_read < dir_size) {
            uint64_t pblock;
            if (ext4_block_lookup(&dir_inode, lb, &pblock) != 0 || pblock == 0) break;
            if (ext4_read_block(pblock, dir_buf + total_read) != 0) break;
            total_read += ext4_fs.block_size;
            lb++;
        }

        /* Scan directory entries */
        bool found = false;
        uint32_t off = 0;
        while (off < dir_size) {
            ext2_dir_entry_t *de = (ext2_dir_entry_t *)(dir_buf + off);
            if (de->rec_len == 0) break;
            if (de->inode != 0 && de->name_len == (uint8_t)len) {
                if (strncmp(de->name, component, len) == 0) {
                    current_inode = de->inode;
                    found = true;
                    break;
                }
            }
            off += de->rec_len;
        }

        kfree(dir_buf);
        if (!found) return 0;
    }

    return current_inode;
}

int ext4_file_exists(const char *path) {
    return ext4_lookup_path(path) != 0 ? 1 : 0;
}

/* ================================================================
 *  Directory listing
 * ================================================================ */

int ext4_list_directory(const char *path, ext4_dir_list_t *entries, uint32_t max_entries) {
    uint32_t dir_ino = ext4_lookup_path(path);
    if (dir_ino == 0) return -1;

    ext2_inode_t dir_inode;
    if (ext4_read_inode(dir_ino, &dir_inode) != 0) return -1;
    if (!(dir_inode.i_mode & EXT2_S_IFDIR)) return -1;

    uint32_t dir_size = dir_inode.i_size;
    uint8_t *dir_buf = (uint8_t *)kalloc(dir_size < ext4_fs.block_size ? ext4_fs.block_size : dir_size);
    if (!dir_buf) return -1;

    uint32_t total_read = 0;
    uint32_t lb = 0;
    while (total_read < dir_size) {
        uint64_t pblock;
        if (ext4_block_lookup(&dir_inode, lb, &pblock) != 0 || pblock == 0) break;
        if (ext4_read_block(pblock, dir_buf + total_read) != 0) break;
        total_read += ext4_fs.block_size;
        lb++;
    }

    int count = 0;
    uint32_t off = 0;
    while (off < dir_size && (uint32_t)count < max_entries) {
        ext2_dir_entry_t *de = (ext2_dir_entry_t *)(dir_buf + off);
        if (de->rec_len == 0) break;
        if (de->inode != 0 && de->name_len > 0) {
            size_t nlen = de->name_len;
            if (nlen > 255) nlen = 255;
            memcpy(entries[count].name, de->name, nlen);
            entries[count].name[nlen] = '\0';
            entries[count].inode = de->inode;
            entries[count].file_type = de->file_type;
            entries[count].is_directory = (de->file_type == EXT2_FT_DIR);

            /* Read inode to get size */
            ext2_inode_t child;
            if (ext4_read_inode(de->inode, &child) == 0)
                entries[count].size = child.i_size;
            else
                entries[count].size = 0;

            count++;
        }
        off += de->rec_len;
    }

    kfree(dir_buf);
    return count;
}

/* ================================================================
 *  File creation
 * ================================================================ */

/* Helper: add a directory entry to a directory inode */
static int ext4_dir_add_entry(uint32_t dir_ino, uint32_t child_ino,
                               const char *name, uint8_t file_type) {
    ext2_inode_t dir_inode;
    if (ext4_read_inode(dir_ino, &dir_inode) != 0) return -1;

    uint32_t dir_size = dir_inode.i_size;
    uint32_t name_len = (uint32_t)strlen(name);
    uint16_t needed   = (uint16_t)(8 + name_len + 3) & ~3U;  /* align to 4 */

    /* Read directory blocks looking for space */
    uint8_t *block_buf = (uint8_t *)kalloc(ext4_fs.block_size);
    if (!block_buf) return -1;

    uint32_t num_blocks = (dir_size + ext4_fs.block_size - 1) / ext4_fs.block_size;
    for (uint32_t lb = 0; lb < num_blocks; lb++) {
        uint64_t pblock;
        if (ext4_block_lookup(&dir_inode, lb, &pblock) != 0 || pblock == 0) continue;
        if (ext4_read_block(pblock, block_buf) != 0) continue;

        uint32_t off = 0;
        uint32_t block_end = ext4_fs.block_size;
        if (lb == num_blocks - 1 && dir_size % ext4_fs.block_size != 0)
            block_end = dir_size % ext4_fs.block_size;

        while (off < block_end) {
            ext2_dir_entry_t *de = (ext2_dir_entry_t *)(block_buf + off);
            if (de->rec_len == 0) break;

            uint16_t actual_size = (uint16_t)(8 + de->name_len + 3) & ~3U;
            uint16_t free_space  = de->rec_len - actual_size;

            if (de->inode == 0 && de->rec_len >= needed) {
                /* Reuse empty entry */
                de->inode     = child_ino;
                de->name_len  = (uint8_t)name_len;
                de->file_type = file_type;
                memcpy(de->name, name, name_len);
                ext4_write_block(pblock, block_buf);
                kfree(block_buf);
                return 0;
            }

            if (free_space >= needed && de->inode != 0) {
                /* Split this entry */
                de->rec_len = actual_size;

                ext2_dir_entry_t *new_de = (ext2_dir_entry_t *)(block_buf + off + actual_size);
                new_de->inode     = child_ino;
                new_de->rec_len   = free_space;
                new_de->name_len  = (uint8_t)name_len;
                new_de->file_type = file_type;
                memcpy(new_de->name, name, name_len);

                ext4_write_block(pblock, block_buf);
                kfree(block_buf);
                return 0;
            }

            off += de->rec_len;
        }
    }

    /* No space in existing blocks – allocate a new block */
    uint64_t new_block = ext4_alloc_block();
    if (new_block == 0) { kfree(block_buf); return -1; }

    memset(block_buf, 0, ext4_fs.block_size);
    ext2_dir_entry_t *new_de = (ext2_dir_entry_t *)block_buf;
    new_de->inode     = child_ino;
    new_de->rec_len   = ext4_fs.block_size;
    new_de->name_len  = (uint8_t)name_len;
    new_de->file_type = file_type;
    memcpy(new_de->name, name, name_len);

    ext4_write_block(new_block, block_buf);

    /* Link the new block into the directory inode */
    if (!(dir_inode.i_flags & 0x80000) && num_blocks < 12) {
        dir_inode.i_block[num_blocks] = (uint32_t)new_block;
    }
    dir_inode.i_size += ext4_fs.block_size;
    dir_inode.i_blocks += ext4_fs.block_size / 512;
    ext4_write_inode(dir_ino, &dir_inode);

    kfree(block_buf);
    return 0;
}

/* Split path into parent dir path and basename */
static int ext4_split_path(const char *path, char *parent, size_t parent_max,
                           char *base, size_t base_max) {
    size_t plen = strlen(path);
    if (plen == 0) return -1;

    /* Find last '/' */
    int last_slash = -1;
    for (int i = (int)plen - 1; i >= 0; i--) {
        if (path[i] == '/') { last_slash = i; break; }
    }

    if (last_slash < 0) {
        strncpy(parent, "/", parent_max - 1);
        strncpy(base, path, base_max - 1);
    } else if (last_slash == 0) {
        strncpy(parent, "/", parent_max - 1);
        strncpy(base, path + 1, base_max - 1);
    } else {
        size_t plen2 = (size_t)last_slash;
        if (plen2 >= parent_max) plen2 = parent_max - 1;
        memcpy(parent, path, plen2);
        parent[plen2] = '\0';
        strncpy(base, path + last_slash + 1, base_max - 1);
    }
    base[base_max - 1] = '\0';
    parent[parent_max - 1] = '\0';
    return 0;
}

int ext4_create_file(const char *path) {
    if (!ext4_fs.mounted || !path) return -1;
    if (ext4_lookup_path(path) != 0) return -1;  /* already exists */

    char parent[256], basename[256];
    if (ext4_split_path(path, parent, sizeof(parent), basename, sizeof(basename)) != 0) return -1;

    uint32_t parent_ino = ext4_lookup_path(parent);
    if (parent_ino == 0) {
        printf("ext4: parent directory not found: %s\n", parent);
        return -1;
    }

    uint32_t new_ino = ext4_alloc_inode();
    if (new_ino == 0) {
        printf("ext4: no free inodes\n");
        return -1;
    }

    /* Initialize inode */
    ext2_inode_t inode;
    memset(&inode, 0, sizeof(inode));
    inode.i_mode  = EXT2_S_IFREG | 0644;
    inode.i_links_count = 1;
    ext4_write_inode(new_ino, &inode);

    /* Add directory entry */
    if (ext4_dir_add_entry(parent_ino, new_ino, basename, EXT2_FT_REG_FILE) != 0) {
        ext4_free_inode(new_ino);
        return -1;
    }

    return 0;
}

int ext4_delete_file(const char *path) {
    if (!ext4_fs.mounted || !path) return -1;

    uint32_t ino = ext4_lookup_path(path);
    if (ino == 0) return -1;

    ext2_inode_t inode;
    if (ext4_read_inode(ino, &inode) != 0) return -1;
    if (inode.i_mode & EXT2_S_IFDIR) {
        printf("ext4: cannot delete directory with delete_file\n");
        return -1;
    }

    /* Free data blocks */
    uint32_t num_blocks = (inode.i_size + ext4_fs.block_size - 1) / ext4_fs.block_size;
    for (uint32_t lb = 0; lb < num_blocks; lb++) {
        uint64_t pblock;
        if (ext4_block_lookup(&inode, lb, &pblock) == 0 && pblock != 0) {
            ext4_free_block(pblock);
        }
    }

    /* Clear inode */
    memset(&inode, 0, sizeof(inode));
    ext4_write_inode(ino, &inode);
    ext4_free_inode(ino);

    /* Remove directory entry */
    char parent[256], basename[256];
    if (ext4_split_path(path, parent, sizeof(parent), basename, sizeof(basename)) == 0) {
        uint32_t parent_ino = ext4_lookup_path(parent);
        if (parent_ino != 0) {
            /* Mark directory entry as deleted (set inode to 0) */
            ext2_inode_t dir_inode;
            if (ext4_read_inode(parent_ino, &dir_inode) == 0) {
                uint8_t *block_buf = (uint8_t *)kalloc(ext4_fs.block_size);
                if (block_buf) {
                    uint32_t dir_blocks = (dir_inode.i_size + ext4_fs.block_size - 1) / ext4_fs.block_size;
                    uint32_t blen = (uint32_t)strlen(basename);
                    for (uint32_t lb = 0; lb < dir_blocks; lb++) {
                        uint64_t pblock;
                        if (ext4_block_lookup(&dir_inode, lb, &pblock) != 0 || pblock == 0) continue;
                        if (ext4_read_block(pblock, block_buf) != 0) continue;

                        uint32_t off = 0;
                        while (off < ext4_fs.block_size) {
                            ext2_dir_entry_t *de = (ext2_dir_entry_t *)(block_buf + off);
                            if (de->rec_len == 0) break;
                            if (de->inode == ino && de->name_len == (uint8_t)blen &&
                                strncmp(de->name, basename, blen) == 0) {
                                de->inode = 0;   /* mark deleted */
                                ext4_write_block(pblock, block_buf);
                                goto done_del;
                            }
                            off += de->rec_len;
                        }
                    }
                    done_del:
                    kfree(block_buf);
                }
            }
        }
    }

    return 0;
}

/* ================================================================
 *  Directory creation / removal
 * ================================================================ */

int ext4_create_directory(const char *path) {
    if (!ext4_fs.mounted || !path) return -1;
    if (ext4_lookup_path(path) != 0) return -1;  /* already exists */

    char parent[256], basename[256];
    if (ext4_split_path(path, parent, sizeof(parent), basename, sizeof(basename)) != 0) return -1;

    uint32_t parent_ino = ext4_lookup_path(parent);
    if (parent_ino == 0) return -1;

    uint32_t new_ino = ext4_alloc_inode();
    if (new_ino == 0) return -1;

    /* Allocate a block for . and .. entries */
    uint64_t dir_block = ext4_alloc_block();
    if (dir_block == 0) { ext4_free_inode(new_ino); return -1; }

    /* Initialize inode */
    ext2_inode_t inode;
    memset(&inode, 0, sizeof(inode));
    inode.i_mode        = EXT2_S_IFDIR | 0755;
    inode.i_size        = ext4_fs.block_size;
    inode.i_blocks      = ext4_fs.block_size / 512;
    inode.i_links_count = 2;
    inode.i_block[0]    = (uint32_t)dir_block;

    /* Create . and .. entries */
    uint8_t *block_buf = (uint8_t *)kalloc(ext4_fs.block_size);
    if (!block_buf) { ext4_free_block(dir_block); ext4_free_inode(new_ino); return -1; }
    memset(block_buf, 0, ext4_fs.block_size);

    ext2_dir_entry_t *dot = (ext2_dir_entry_t *)block_buf;
    dot->inode     = new_ino;
    dot->rec_len   = 12;
    dot->name_len  = 1;
    dot->file_type = EXT2_FT_DIR;
    dot->name[0]   = '.';

    ext2_dir_entry_t *dotdot = (ext2_dir_entry_t *)(block_buf + 12);
    dotdot->inode     = parent_ino;
    dotdot->rec_len   = ext4_fs.block_size - 12;
    dotdot->name_len  = 2;
    dotdot->file_type = EXT2_FT_DIR;
    dotdot->name[0]   = '.';
    dotdot->name[1]   = '.';

    ext4_write_block(dir_block, block_buf);
    kfree(block_buf);

    ext4_write_inode(new_ino, &inode);

    /* Add entry in parent */
    ext4_dir_add_entry(parent_ino, new_ino, basename, EXT2_FT_DIR);

    /* Increment parent link count */
    ext2_inode_t parent_inode;
    if (ext4_read_inode(parent_ino, &parent_inode) == 0) {
        parent_inode.i_links_count++;
        ext4_write_inode(parent_ino, &parent_inode);
    }

    /* Update BGDT used dirs count */
    uint32_t g = (new_ino - 1) / ext4_fs.inodes_per_group;
    if (g < ext4_fs.num_block_groups)
        ext4_fs.bgdt[g].bg_used_dirs_count++;

    return 0;
}

int ext4_remove_directory(const char *path) {
    if (!ext4_fs.mounted || !path) return -1;

    uint32_t ino = ext4_lookup_path(path);
    if (ino == 0 || ino == EXT2_ROOT_INO) return -1;

    ext2_inode_t inode;
    if (ext4_read_inode(ino, &inode) != 0) return -1;
    if (!(inode.i_mode & EXT2_S_IFDIR)) return -1;

    /* Check if directory is empty (only . and ..) */
    ext4_dir_list_t entries[8];
    int n = ext4_list_directory(path, entries, 8);
    if (n < 0) return -1;
    int real_count = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(entries[i].name, ".") != 0 && strcmp(entries[i].name, "..") != 0)
            real_count++;
    }
    if (real_count > 0) {
        printf("ext4: directory not empty\n");
        return -1;
    }

    /* Free data blocks */
    uint32_t num_blocks = (inode.i_size + ext4_fs.block_size - 1) / ext4_fs.block_size;
    for (uint32_t lb = 0; lb < num_blocks; lb++) {
        uint64_t pblock;
        if (ext4_block_lookup(&inode, lb, &pblock) == 0 && pblock != 0)
            ext4_free_block(pblock);
    }

    memset(&inode, 0, sizeof(inode));
    ext4_write_inode(ino, &inode);
    ext4_free_inode(ino);

    /* Remove parent directory entry (same as delete_file) */
    char parent_path[256], basename[256];
    if (ext4_split_path(path, parent_path, sizeof(parent_path), basename, sizeof(basename)) == 0) {
        uint32_t parent_ino = ext4_lookup_path(parent_path);
        if (parent_ino != 0) {
            ext2_inode_t parent_inode;
            if (ext4_read_inode(parent_ino, &parent_inode) == 0) {
                parent_inode.i_links_count--;
                ext4_write_inode(parent_ino, &parent_inode);

                uint8_t *block_buf = (uint8_t *)kalloc(ext4_fs.block_size);
                if (block_buf) {
                    uint32_t dir_blocks = (parent_inode.i_size + ext4_fs.block_size - 1) / ext4_fs.block_size;
                    uint32_t blen = (uint32_t)strlen(basename);
                    for (uint32_t lb = 0; lb < dir_blocks; lb++) {
                        uint64_t pblock;
                        if (ext4_block_lookup(&parent_inode, lb, &pblock) != 0 || pblock == 0) continue;
                        if (ext4_read_block(pblock, block_buf) != 0) continue;
                        uint32_t off2 = 0;
                        while (off2 < ext4_fs.block_size) {
                            ext2_dir_entry_t *de = (ext2_dir_entry_t *)(block_buf + off2);
                            if (de->rec_len == 0) break;
                            if (de->inode == ino && de->name_len == (uint8_t)blen &&
                                strncmp(de->name, basename, blen) == 0) {
                                de->inode = 0;
                                ext4_write_block(pblock, block_buf);
                                goto done_rmdir;
                            }
                            off2 += de->rec_len;
                        }
                    }
                    done_rmdir:
                    kfree(block_buf);
                }
            }
        }
    }

    return 0;
}

/* ================================================================
 *  Sync – write superblock and BGDT back to disk
 * ================================================================ */

int ext4_sync(void) {
    ext4_enter();
    if (!ext4_fs.mounted) {
        ext4_leave();
        return -1;
    }

    /* Write superblock back (at block group 0, offset 1024) */
    uint8_t sb_buf[2048];
    memset(sb_buf, 0, sizeof(sb_buf));
    memcpy(sb_buf, &ext4_fs.superblock, sizeof(ext2_superblock_t));

    uint32_t sb_lba = ext4_fs.partition_lba + 2;
    storage_write_sectors(ext4_fs.storage_dev->device_id, sb_lba, 4, sb_buf);

    /* Write BGDT */
    uint32_t bgdt_block = ext4_fs.first_data_block + 1;
    uint32_t bgdt_size  = ext4_fs.num_block_groups * sizeof(ext2_block_group_desc_t);
    uint32_t bgdt_blocks = (bgdt_size + ext4_fs.block_size - 1) / ext4_fs.block_size;

    for (uint32_t i = 0; i < bgdt_blocks; i++) {
        ext4_write_block(bgdt_block + i,
                         (uint8_t *)ext4_fs.bgdt + i * ext4_fs.block_size);
    }

    ext4_leave();
    return 0;
}

/* ================================================================
 *  Info / debug
 * ================================================================ */

void ext4_print_info(void) {
    ext4_enter();
    if (!ext4_fs.mounted) {
        printf("ext4: not mounted\n");
        ext4_leave();
        return;
    }

    printf("=== EXT4 Filesystem Info ===\n");
    printf("Block size:       %u bytes\n", ext4_fs.block_size);
    printf("Inode size:       %u bytes\n", ext4_fs.inode_size);
    printf("Total blocks:     %llu\n", (unsigned long long)ext4_fs.total_blocks);
    printf("Total inodes:     %u\n", ext4_fs.superblock.s_inodes_count);
    printf("Free blocks:      %u\n", ext4_fs.superblock.s_free_blocks_count);
    printf("Free inodes:      %u\n", ext4_fs.superblock.s_free_inodes_count);
    printf("Block groups:     %u\n", ext4_fs.num_block_groups);
    printf("Blocks/group:     %u\n", ext4_fs.blocks_per_group);
    printf("Inodes/group:     %u\n", ext4_fs.inodes_per_group);
    printf("Extents:          %s\n", ext4_fs.has_extents ? "yes" : "no");
    printf("64-bit:           %s\n", ext4_fs.has_64bit ? "yes" : "no");
    printf("Volume name:      %.16s\n", ext4_fs.superblock.s_volume_name);
    ext4_leave();
}
