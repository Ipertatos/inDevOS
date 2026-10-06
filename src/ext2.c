#include "ext2.h"
#include "mem.h"
#include "utils.h"
#include "string.h"
#include "serial.h"

// ---- ext2 read-only filesystem driver ---------------------------------------

// Read a filesystem block into buffer
int ext2_read_block(ext2_fs_t *fs, uint32_t block_num, void *buffer) {
    if (!fs || !fs->storage_dev || !buffer) return -1;

    // Convert block number to LBA
    uint32_t sectors_per_block = fs->block_size / SECTOR_SIZE;
    uint64_t lba = fs->partition_lba + (uint64_t)block_num * sectors_per_block;

    return storage_read_sectors(fs->storage_dev->device_id, lba,
                                sectors_per_block, buffer);
}

// Mount: read superblock and BGDT
int ext2_mount(uint8_t device_id, uint32_t partition_lba, ext2_fs_t *fs) {
    if (!fs) return -1;
    memset(fs, 0, sizeof(ext2_fs_t));

    storage_device_t *dev = storage_get_device(device_id);
    if (!dev) {
        printf("ext2: Device %d not found\n", device_id);
        return -1;
    }

    fs->storage_dev   = dev;
    fs->partition_lba  = partition_lba;

    // Read superblock (at byte offset 1024 = sector 2 from partition start)
    uint8_t sb_buf[1024];
    if (storage_read_sectors(device_id, partition_lba + 2, 2, sb_buf) != 0) {
        printf("ext2: Failed to read superblock\n");
        return -1;
    }
    memmove(&fs->superblock, sb_buf, sizeof(ext2_superblock_t));

    if (fs->superblock.s_magic != EXT2_MAGIC) {
        printf("ext2: Bad magic 0x%x (expected 0x%x)\n",
               fs->superblock.s_magic, EXT2_MAGIC);
        return -1;
    }

    // Derived values
    fs->block_size       = 1024U << fs->superblock.s_log_block_size;
    fs->inodes_per_group = fs->superblock.s_inodes_per_group;
    fs->blocks_per_group = fs->superblock.s_blocks_per_group;
    fs->first_data_block = fs->superblock.s_first_data_block;

    if (fs->superblock.s_rev_level >= 1) {
        fs->inode_size = fs->superblock.s_inode_size;
    } else {
        fs->inode_size = 128;  // good-old revision
    }

    // Number of block groups
    fs->num_block_groups = (fs->superblock.s_blocks_count +
                            fs->blocks_per_group - 1) / fs->blocks_per_group;

    printf("ext2: Mounted – block_size=%u, inodes=%u, blocks=%u, groups=%u\n",
           fs->block_size, fs->superblock.s_inodes_count,
           fs->superblock.s_blocks_count, fs->num_block_groups);
    printf("ext2: Volume: '%s'\n", fs->superblock.s_volume_name);

    // Read Block Group Descriptor Table (starts in the block after superblock)
    uint32_t bgdt_block = fs->first_data_block + 1;
    uint32_t bgdt_size  = fs->num_block_groups * sizeof(ext2_block_group_desc_t);
    uint32_t bgdt_blocks = (bgdt_size + fs->block_size - 1) / fs->block_size;

    fs->bgdt = kalloc(bgdt_blocks * fs->block_size);
    if (!fs->bgdt) {
        printf("ext2: Failed to allocate BGDT\n");
        return -1;
    }

    for (uint32_t i = 0; i < bgdt_blocks; i++) {
        if (ext2_read_block(fs, bgdt_block + i,
                            (uint8_t*)fs->bgdt + i * fs->block_size) != 0) {
            printf("ext2: Failed to read BGDT block %u\n", bgdt_block + i);
            kfree(fs->bgdt);
            fs->bgdt = NULL;
            return -1;
        }
    }

    fs->mounted = true;
    return 0;
}

void ext2_unmount(ext2_fs_t *fs) {
    if (!fs) return;
    if (fs->bgdt) {
        kfree(fs->bgdt);
        fs->bgdt = NULL;
    }
    fs->mounted = false;
    printf("ext2: Unmounted\n");
}

bool ext2_is_mounted(ext2_fs_t *fs) {
    return fs && fs->mounted;
}

// Read a specific inode (1-based inode number)
int ext2_read_inode(ext2_fs_t *fs, uint32_t inode_num, ext2_inode_t *out) {
    if (!fs || !fs->mounted || !out || inode_num == 0) return -1;

    uint32_t group = (inode_num - 1) / fs->inodes_per_group;
    uint32_t index = (inode_num - 1) % fs->inodes_per_group;

    if (group >= fs->num_block_groups) return -1;

    uint32_t inode_table_block = fs->bgdt[group].bg_inode_table;

    // Byte offset of the inode within the table
    uint64_t byte_offset = (uint64_t)index * fs->inode_size;
    uint32_t block_containing = inode_table_block +
                                (uint32_t)(byte_offset / fs->block_size);
    uint32_t offset_in_block = (uint32_t)(byte_offset % fs->block_size);

    void *block_buf = kalloc(fs->block_size);
    if (!block_buf) return -1;

    if (ext2_read_block(fs, block_containing, block_buf) != 0) {
        kfree(block_buf);
        return -1;
    }

    memmove(out, (uint8_t*)block_buf + offset_in_block, sizeof(ext2_inode_t));
    kfree(block_buf);
    return 0;
}

// Resolve an indirect block pointer to find the actual data block number
// at a given logical index within the indirect level.
static uint32_t ext2_resolve_indirect(ext2_fs_t *fs, uint32_t indirect_block,
                                       uint32_t index) {
    if (indirect_block == 0) return 0;

    void *buf = kalloc(fs->block_size);
    if (!buf) return 0;

    if (ext2_read_block(fs, indirect_block, buf) != 0) {
        kfree(buf);
        return 0;
    }

    uint32_t result = ((uint32_t*)buf)[index];
    kfree(buf);
    return result;
}

/**
 * Get the physical block number for a given logical block index of a file.
 * Handles direct, singly indirect, doubly indirect, and triply indirect blocks.
 */
static uint32_t ext2_get_block(ext2_fs_t *fs, ext2_inode_t *inode,
                                uint32_t logical_block) {
    uint32_t ptrs_per_block = fs->block_size / 4;

    // Direct blocks (0-11)
    if (logical_block < 12) {
        return inode->i_block[logical_block];
    }
    logical_block -= 12;

    // Singly indirect (12)
    if (logical_block < ptrs_per_block) {
        return ext2_resolve_indirect(fs, inode->i_block[12], logical_block);
    }
    logical_block -= ptrs_per_block;

    // Doubly indirect (13)
    if (logical_block < ptrs_per_block * ptrs_per_block) {
        uint32_t idx1 = logical_block / ptrs_per_block;
        uint32_t idx2 = logical_block % ptrs_per_block;
        uint32_t single = ext2_resolve_indirect(fs, inode->i_block[13], idx1);
        if (single == 0) return 0;
        return ext2_resolve_indirect(fs, single, idx2);
    }
    logical_block -= ptrs_per_block * ptrs_per_block;

    // Triply indirect (14)
    {
        uint32_t sq = ptrs_per_block * ptrs_per_block;
        uint32_t idx1 = logical_block / sq;
        uint32_t rem  = logical_block % sq;
        uint32_t idx2 = rem / ptrs_per_block;
        uint32_t idx3 = rem % ptrs_per_block;

        uint32_t dbl = ext2_resolve_indirect(fs, inode->i_block[14], idx1);
        if (dbl == 0) return 0;
        uint32_t sgl = ext2_resolve_indirect(fs, dbl, idx2);
        if (sgl == 0) return 0;
        return ext2_resolve_indirect(fs, sgl, idx3);
    }
}

// Read file data from an inode
int ext2_read_file(ext2_fs_t *fs, ext2_inode_t *inode, void *buffer,
                   uint32_t offset, uint32_t size) {
    if (!fs || !inode || !buffer) return -1;

    uint32_t file_size = inode->i_size;
    if (offset >= file_size) return 0;  // past EOF
    if (offset + size > file_size) size = file_size - offset;

    void *block_buf = kalloc(fs->block_size);
    if (!block_buf) return -1;

    uint8_t *out = (uint8_t*)buffer;
    uint32_t bytes_read = 0;

    while (bytes_read < size) {
        uint32_t cur_offset    = offset + bytes_read;
        uint32_t logical_block = cur_offset / fs->block_size;
        uint32_t block_offset  = cur_offset % fs->block_size;
        uint32_t to_copy       = fs->block_size - block_offset;
        if (to_copy > size - bytes_read) to_copy = size - bytes_read;

        uint32_t phys_block = ext2_get_block(fs, inode, logical_block);
        if (phys_block == 0) {
            // Sparse block – fill with zeros
            memset(out + bytes_read, 0, to_copy);
        } else {
            if (ext2_read_block(fs, phys_block, block_buf) != 0) {
                kfree(block_buf);
                return bytes_read > 0 ? (int)bytes_read : -1;
            }
            memmove(out + bytes_read, (uint8_t*)block_buf + block_offset, to_copy);
        }

        bytes_read += to_copy;
    }

    kfree(block_buf);
    return (int)bytes_read;
}

// List directory contents from a directory inode
int ext2_list_directory(ext2_fs_t *fs, uint32_t dir_inode_num,
                        ext2_dir_list_t *entries, uint32_t max_entries) {
    if (!fs || !entries) return -1;

    ext2_inode_t dir_inode;
    if (ext2_read_inode(fs, dir_inode_num, &dir_inode) != 0) return -1;
    if (!(dir_inode.i_mode & EXT2_S_IFDIR)) return -1;

    uint32_t dir_size = dir_inode.i_size;
    void *dir_data = kalloc(dir_size);
    if (!dir_data) return -1;

    if (ext2_read_file(fs, &dir_inode, dir_data, 0, dir_size) < 0) {
        kfree(dir_data);
        return -1;
    }

    uint32_t count = 0;
    uint32_t pos = 0;

    while (pos < dir_size && count < max_entries) {
        ext2_dir_entry_t *de = (ext2_dir_entry_t*)((uint8_t*)dir_data + pos);

        if (de->rec_len == 0) break;  // corrupt
        if (de->inode != 0 && de->name_len > 0) {
            uint32_t nlen = de->name_len;
            if (nlen >= 256) nlen = 255;
            memmove(entries[count].name, de->name, nlen);
            entries[count].name[nlen] = '\0';
            entries[count].inode = de->inode;
            entries[count].file_type = de->file_type;
            entries[count].is_directory = (de->file_type == EXT2_FT_DIR);

            // Look up size
            ext2_inode_t child_inode;
            if (ext2_read_inode(fs, de->inode, &child_inode) == 0) {
                entries[count].size = child_inode.i_size;
            } else {
                entries[count].size = 0;
            }
            count++;
        }
        pos += de->rec_len;
    }

    kfree(dir_data);
    return (int)count;
}

// Look up a path (e.g. "/etc/fstab") and return the inode number.
uint32_t ext2_lookup_path(ext2_fs_t *fs, const char *path) {
    if (!fs || !path) return 0;

    uint32_t current_inode = EXT2_ROOT_INO;

    // Skip leading slash
    while (*path == '/') path++;
    if (*path == '\0') return current_inode;  // root itself

    char component[256];

    while (*path) {
        // Extract next path component
        int clen = 0;
        while (*path && *path != '/' && clen < 255) {
            component[clen++] = *path++;
        }
        component[clen] = '\0';
        while (*path == '/') path++;

        // Look up 'component' in current directory
        ext2_inode_t dir_inode;
        if (ext2_read_inode(fs, current_inode, &dir_inode) != 0) return 0;
        if (!(dir_inode.i_mode & EXT2_S_IFDIR)) return 0;

        uint32_t dir_size = dir_inode.i_size;
        void *dir_data = kalloc(dir_size);
        if (!dir_data) return 0;

        if (ext2_read_file(fs, &dir_inode, dir_data, 0, dir_size) < 0) {
            kfree(dir_data);
            return 0;
        }

        bool found = false;
        uint32_t pos = 0;
        while (pos < dir_size) {
            ext2_dir_entry_t *de = (ext2_dir_entry_t*)((uint8_t*)dir_data + pos);
            if (de->rec_len == 0) break;

            if (de->inode != 0 && de->name_len == (uint8_t)clen) {
                if (memcmp(de->name, component, clen) == 0) {
                    current_inode = de->inode;
                    found = true;
                    break;
                }
            }
            pos += de->rec_len;
        }

        kfree(dir_data);
        if (!found) return 0;
    }

    return current_inode;
}
