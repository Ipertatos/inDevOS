#ifndef EXFAT_H
#define EXFAT_H
#include "types.h"
#include "vfs.h"

/* exFAT driver (run 1: read-only).  Volumes are independent instances, so
 * several can be mounted at once (e.g. a Ventoy stick at /mnt/ventoy). */

typedef struct exfat_vol exfat_vol_t;

/* Is there an exFAT boot sector at @lba on @dev? */
bool exfat_probe(uint8_t dev, uint32_t lba);
/* First exFAT partition start on @dev, or -1. */
int64_t exfat_find_partition(uint8_t dev);

exfat_vol_t *exfat_mount(uint8_t dev, uint32_t part_lba);
void exfat_unmount(exfat_vol_t *v);
void exfat_info(const exfat_vol_t *v);
const char *exfat_label(const exfat_vol_t *v);
/* Switch a mounted volume to read-write: loads the allocation bitmap and sets
 * VolumeDirty (cleared again by exfat_unmount). Not used by auto-mount. */
int exfat_enable_rw(exfat_vol_t *v);
int exfat_sync(exfat_vol_t *v);
/* Already-mounted volume at (@dev, @part), its mount path in *@path; or NULL. */
exfat_vol_t *exfat_find_mounted(uint8_t dev, uint32_t part, const char **path);

int exfat_vfs_register(const char *mount_path, exfat_vol_t *v);
vfs_fs_ops_t *exfat_get_ops(void);
#endif
