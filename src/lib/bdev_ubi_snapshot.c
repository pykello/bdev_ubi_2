#include "bdev_ubi_internal.h"

#include "spdk/blob.h"
#include "spdk/likely.h"
#include "spdk/log.h"

/*
 * Static function forward declarations
 */

struct snapshot_context {
    spdk_snapshot_ubi_complete cb_fn;
    void *cb_arg;
    struct ubi_bdev *ubi_bdev;
};

static void ubi_shallow_copy_status_cb(uint64_t copied_clusters, void *cb_arg) {
    struct ubi_bdev *ubi_bdev = cb_arg;
    ubi_bdev->snapshot_status.copied_clusters = copied_clusters;
}

static void ubi_shallow_copy_complete_cb(void *cb_arg, int rc) {
    struct ubi_bdev *ubi_bdev = cb_arg;

    if (rc != 0) {
        SPDK_ERRLOG("Failed to shallow copy %s: %d\n", ubi_bdev->bdev.name, rc);
        ubi_bdev->snapshot_status.result = rc;
    }

    ubi_bdev->snapshot_status.in_progress = false;
}

static void ubi_snapshot_create_cb(void *cb_arg, spdk_blob_id blobid, int bserrno) {
    struct snapshot_context *ctx = cb_arg;
    struct ubi_bdev *ubi_bdev = ctx->ubi_bdev;

    SPDK_WARNLOG("Snapshot created for %s\n", ubi_bdev->bdev.name);

    if (bserrno != 0) {
        SPDK_ERRLOG("Failed to create snapshot for %s: %d\n", ubi_bdev->bdev.name,
                    bserrno);
        ubi_bdev->snapshot_status.result = bserrno;
        return;
    }

    ubi_bdev->snapshot_status.in_progress = true;
    ubi_bdev->snapshot_status.snapshot_blobid = blobid;
    ubi_bdev->snapshot_status.total_clusters =
        spdk_bs_total_data_cluster_count(ubi_bdev->blobstore);
    ubi_bdev->snapshot_status.shallow_copy_bs_dev =
        bs_dev_delta_create("/tmp/ubi_bdev", ubi_bdev->bdev.blocklen, 2048);
    ctx->cb_fn(ctx->cb_arg, 0);
}

void bdev_ubi_snapshot(const char *name, const char *path,
                       spdk_snapshot_ubi_complete cb_fn, void *cb_arg) {
    struct spdk_bdev *bdev = spdk_bdev_get_by_name(name);
    if (bdev == NULL) {
        SPDK_ERRLOG("bdev %s not found\n", name);
        cb_fn(cb_arg, -ENOENT);
        return;
    }

    struct ubi_bdev *ubi_bdev = SPDK_CONTAINEROF(bdev, struct ubi_bdev, bdev);
    struct spdk_blob_xattr_opts xattrs = {0};
    struct snapshot_context *ctx = calloc(1, sizeof(*ctx));
    ctx->cb_fn = cb_fn;
    ctx->cb_arg = cb_arg;
    ctx->ubi_bdev = ubi_bdev;
    spdk_bs_create_snapshot(ubi_bdev->blobstore, ubi_bdev->blobid, &xattrs,
                            ubi_snapshot_create_cb, ctx);
}
