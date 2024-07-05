#include "bdev_ubi_internal.h"

#include "spdk/blob.h"
#include "spdk/likely.h"
#include "spdk/log.h"

/*
 * Static function forward declarations
 */

void bdev_ubi_snapshot(const char *name, const char *path,
                       spdk_snapshot_ubi_complete cb_fn, void *cb_arg) {
    struct spdk_bdev *bdev = spdk_bdev_get_by_name(name);
    if (bdev == NULL) {
        SPDK_ERRLOG("bdev %s not found\n", name);
        cb_fn(cb_arg, -ENOENT);
        return;
    }

    struct ubi_bdev *ubi_bdev = SPDK_CONTAINEROF(bdev, struct ubi_bdev, bdev);
    (void)ubi_bdev;
    cb_fn(cb_arg, 0);
}
