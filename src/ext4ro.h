#ifndef EXT4RO_H
#define EXT4RO_H
#include "types.h"
#include "vfs.h"

/* Read-only ext2/3/4 driver (multi-instance).  Refuses unknown INCOMPAT
 * features; supports extents, flex_bg, 64bit, dir_index (read linearly),
 * filetype, huge_file, metadata_csum (checksums not verified), inline_data,
 * legacy block maps, fast/slow symlinks (followed on lookup). */
typedef struct e4_vol e4_vol_t;

bool      ext4ro_probe(uint8_t dev, uint32_t lba);
e4_vol_t *ext4ro_mount(uint8_t dev, uint32_t lba);
void      ext4ro_unmount(e4_vol_t *v);
const char *ext4ro_label(const e4_vol_t *v);
void      ext4ro_info(const e4_vol_t *v);
int       ext4ro_vfs_register(const char *path, e4_vol_t *v);
vfs_fs_ops_t *ext4ro_get_ops(void);
/* Mounted volume at (dev, lba) or NULL. */
e4_vol_t *ext4ro_find_mounted(uint8_t dev, uint32_t lba, const char **path);
#endif
