#pragma once

#include "types.h"
#include "storage.h"

// ---- ext2 filesystem read-only driver --------------------------------------

// ext2 magic number (offset 0x38 in superblock)
#define EXT2_MAGIC 0xEF53

// Inode constants
#define EXT2_ROOT_INO       2
#define EXT2_GOOD_OLD_FIRST_INO 11

// File type bits in inode->i_mode
#define EXT2_S_IFREG  0x8000
#define EXT2_S_IFDIR  0x4000
#define EXT2_S_IFLNK  0xA000

// Directory entry file type (d_file_type)
#define EXT2_FT_UNKNOWN  0
#define EXT2_FT_REG_FILE 1
#define EXT2_FT_DIR      2
#define EXT2_FT_CHRDEV   3
#define EXT2_FT_BLKDEV   4
#define EXT2_FT_FIFO     5
#define EXT2_FT_SOCK     6
#define EXT2_FT_SYMLINK  7

// Superblock (1024 bytes at offset 1024 from start of partition)
typedef struct __attribute__((packed)) {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;      // block size = 1024 << s_log_block_size
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    // EXT2_DYNAMIC_REV fields
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t  s_uuid[16];
    char     s_volume_name[16];
    char     s_last_mounted[64];
    uint32_t s_algo_bitmap;
    // ... more fields exist but we don't need them for read-only
} ext2_superblock_t;

// Block Group Descriptor (32 bytes)
typedef struct __attribute__((packed)) {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint8_t  bg_reserved[12];
} ext2_block_group_desc_t;

// Inode (128 or 256 bytes depending on revision)
typedef struct __attribute__((packed)) {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;           // file size in bytes (lower 32 bits)
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;         // count of 512-byte blocks
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15];      // block pointers: 0-11 direct, 12 indirect,
                                //                 13 double indirect, 14 triple indirect
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;        // also i_size_high for regular files
    uint32_t i_faddr;
    uint8_t  i_osd2[12];
} ext2_inode_t;

// Directory Entry (variable length)
typedef struct __attribute__((packed)) {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[];           // flexible array member
} ext2_dir_entry_t;

// In-memory ext2 filesystem state
typedef struct {
    storage_device_t *storage_dev;
    uint32_t partition_lba;    // LBA offset of the ext2 partition
    uint32_t block_size;       // bytes per block (1024, 2048, or 4096)
    uint32_t inodes_per_group;
    uint32_t blocks_per_group;
    uint32_t inode_size;       // bytes per inode
    uint32_t num_block_groups;
    uint32_t first_data_block;
    ext2_superblock_t superblock;
    ext2_block_group_desc_t *bgdt;  // block group descriptor table (heap-allocated)
    bool mounted;
} ext2_fs_t;

// Directory listing entry (for readdir)
typedef struct {
    char     name[256];
    uint32_t inode;
    uint8_t  file_type;
    uint32_t size;
    bool     is_directory;
} ext2_dir_list_t;

// ---- Public API (read-only) -------------------------------------------------

// Mount an ext2 filesystem from a storage device + partition LBA
int  ext2_mount(uint8_t device_id, uint32_t partition_lba, ext2_fs_t *fs);
void ext2_unmount(ext2_fs_t *fs);
bool ext2_is_mounted(ext2_fs_t *fs);

// Read an inode by number (1-based)
int ext2_read_inode(ext2_fs_t *fs, uint32_t inode_num, ext2_inode_t *out);

// Read file data from an inode into buffer (up to 'size' bytes from 'offset')
int ext2_read_file(ext2_fs_t *fs, ext2_inode_t *inode, void *buffer,
                   uint32_t offset, uint32_t size);

// List directory contents
int ext2_list_directory(ext2_fs_t *fs, uint32_t dir_inode_num,
                        ext2_dir_list_t *entries, uint32_t max_entries);

// Look up a path and return the inode number.  Returns 0 if not found.
uint32_t ext2_lookup_path(ext2_fs_t *fs, const char *path);

// Read a block from the ext2 filesystem
int ext2_read_block(ext2_fs_t *fs, uint32_t block_num, void *buffer);
