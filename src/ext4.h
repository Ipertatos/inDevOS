/**
 * @file ext4.h
 * @brief EXT4 filesystem driver (read-write)
 *
 * Extends the ext2 foundation with ext4 features:
 *   - Extent tree (replaces indirect block pointers)
 *   - 64-bit block numbers
 *   - Large file support (>= 2 GB)
 *   - HTree directory indexing (basic)
 *   - File creation, writing, deletion
 *   - Directory creation and removal
 */
#ifndef EXT4_H
#define EXT4_H

#include "types.h"
#include "storage.h"
#include "ext2.h"          /* reuse superblock / inode / block group structures */

/* ================================================================
 *  EXT4 feature flags (superblock s_feature_incompat)
 * ================================================================ */
#define EXT4_FEATURE_INCOMPAT_EXTENTS     0x0040
#define EXT4_FEATURE_INCOMPAT_64BIT       0x0002
#define EXT4_FEATURE_INCOMPAT_FLEX_BG     0x0200
#define EXT4_FEATURE_INCOMPAT_FILETYPE    0x0002

#define EXT4_FEATURE_RO_COMPAT_HUGE_FILE  0x0008
#define EXT4_FEATURE_RO_COMPAT_DIR_NLINK  0x0020
#define EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE 0x0040

/* ================================================================
 *  Extent structures (EXT4_FEATURE_INCOMPAT_EXTENTS)
 * ================================================================ */

/* Extent header – first 12 bytes of i_block when extents are enabled */
typedef struct __attribute__((packed)) {
    uint16_t eh_magic;      /* 0xF30A */
    uint16_t eh_entries;    /* number of valid entries */
    uint16_t eh_max;        /* capacity of entries */
    uint16_t eh_depth;      /* 0 = leaf, >0 = internal */
    uint32_t eh_generation;
} ext4_extent_header_t;

#define EXT4_EXT_MAGIC 0xF30A

/* Leaf extent – maps a range of logical blocks to physical blocks */
typedef struct __attribute__((packed)) {
    uint32_t ee_block;      /* first logical block */
    uint16_t ee_len;        /* number of blocks (<=32768; if >32768, it's uninitialized) */
    uint16_t ee_start_hi;   /* upper 16 bits of physical block */
    uint32_t ee_start_lo;   /* lower 32 bits of physical block */
} ext4_extent_t;

/* Internal (index) node – points to a child extent block */
typedef struct __attribute__((packed)) {
    uint32_t ei_block;      /* logical block covered */
    uint32_t ei_leaf_lo;    /* lower 32 bits of child block */
    uint16_t ei_leaf_hi;    /* upper 16 bits of child block */
    uint16_t ei_unused;
} ext4_extent_idx_t;

/* Maximum extents in inode (60 bytes / 12 bytes per extent = 4 leaf or 4 idx, + header) */
#define EXT4_INODE_EXTENTS  4

/* ================================================================
 *  EXT4 superblock extra fields (appended after ext2 superblock)
 * ================================================================ */
typedef struct __attribute__((packed)) {
    /* --- ext2_superblock_t fields occupy first 236 bytes --- */
    /* We overlay the ext4 extra fields starting at offset 0x150 (336) */
    /* For simplicity we only track the most important ext4 additions */
    uint32_t s_blocks_count_hi;     /* upper 32 bits of block count   */
    uint32_t s_r_blocks_count_hi;
    uint32_t s_free_blocks_count_hi;
    uint16_t s_min_extra_isize;
    uint16_t s_want_extra_isize;
    uint32_t s_flags;
    /* ... more fields, but rarely needed for basic r/w ... */
} ext4_superblock_extra_t;

/* ================================================================
 *  In-memory filesystem state
 * ================================================================ */
typedef struct {
    storage_device_t *storage_dev;
    uint32_t partition_lba;         /* LBA offset of the partition */
    uint32_t block_size;
    uint32_t inode_size;
    uint32_t inodes_per_group;
    uint32_t blocks_per_group;
    uint32_t num_block_groups;
    uint32_t first_data_block;
    uint64_t total_blocks;          /* 64-bit total block count */
    bool     has_extents;           /* extents feature detected */
    bool     has_64bit;             /* 64-bit feature detected */
    bool     mounted;
    ext2_superblock_t superblock;
    ext2_block_group_desc_t *bgdt;  /* block group descriptor table */
} ext4_fs_t;

/* ================================================================
 *  Directory listing (reuses ext2 format for compatibility)
 * ================================================================ */
typedef ext2_dir_list_t ext4_dir_list_t;

/* ================================================================
 *  Public API
 * ================================================================ */

/* Reentrant FS lock (preemption-disabled).  VFS ops and mount/unmount hold it. */
void ext4_enter(void);
void ext4_leave(void);

/* Mounting / unmounting */
int  ext4_mount(uint8_t device_id, uint32_t partition_lba);
void ext4_unmount(void);
bool ext4_is_mounted(void);

/* Inode operations */
int  ext4_read_inode(uint32_t inode_num, ext2_inode_t *out);
int  ext4_write_inode(uint32_t inode_num, const ext2_inode_t *inode);

/* File reading (extent-aware) */
int  ext4_read_file(uint32_t inode_num, void *buffer, uint32_t offset, uint32_t size);

/* File writing */
int  ext4_write_file(uint32_t inode_num, const void *buffer, uint32_t offset, uint32_t size);

/* File creation & deletion */
int  ext4_create_file(const char *path);
int  ext4_delete_file(const char *path);

/* Directory operations */
int  ext4_create_directory(const char *path);
int  ext4_remove_directory(const char *path);
int  ext4_list_directory(const char *path, ext4_dir_list_t *entries, uint32_t max_entries);

/* Path lookup – returns inode number, 0 if not found */
uint32_t ext4_lookup_path(const char *path);

/* File existence */
int  ext4_file_exists(const char *path);

/* Block I/O */
int  ext4_read_block(uint64_t block_num, void *buffer);
int  ext4_write_block(uint64_t block_num, const void *buffer);

/* Block allocation / deallocation */
uint64_t ext4_alloc_block(void);
void     ext4_free_block(uint64_t block_num);

/* Inode allocation / deallocation */
uint32_t ext4_alloc_inode(void);
void     ext4_free_inode(uint32_t inode_num);

/* Extent helpers */
uint64_t ext4_extent_get_pblock(const ext4_extent_t *ext);
int      ext4_extent_lookup(const ext2_inode_t *inode, uint32_t logical_block,
                            uint64_t *out_physical);

/* Sync */
int  ext4_sync(void);

/* Status / debug */
void ext4_print_info(void);

#endif /* EXT4_H */
