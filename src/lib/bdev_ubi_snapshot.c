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
    spdk_blob_id snapshot_blobid;
    spdk_blob_id clone_blobid;
    struct spdk_io_channel *ch;
    struct spdk_bs_dev *shallow_copy_bs_dev;
    char *path;
};

static void cleanup_snapshot_context(struct snapshot_context *ctx) {
    if (ctx->shallow_copy_bs_dev != NULL) {
        ctx->shallow_copy_bs_dev->destroy(ctx->shallow_copy_bs_dev);
    }
    if (ctx->ch != NULL) {
        spdk_bs_free_io_channel(ctx->ch);
    }
    free(ctx->path);
    free(ctx);
}

static void ubi_shallow_copy_complete_cb(void *cb_arg, int rc) {
    SPDK_WARNLOG("shallow copy complete\n");
    struct snapshot_context *ctx = cb_arg;
    struct ubi_bdev *ubi_bdev = ctx->ubi_bdev;

    if (rc != 0) {
        SPDK_ERRLOG("Failed to shallow copy %s: %d\n", ubi_bdev->bdev.name, rc);
        ubi_bdev->snapshot_status.result = rc;
    }

    cleanup_snapshot_context(ctx);
    ubi_bdev->snapshot_status.in_progress = false;
}

static void ubi_shallow_copy_status_cb(uint64_t copied_clusters, void *cb_arg) {
    SPDK_WARNLOG("shallow copy status: %lu\n", copied_clusters);
    struct snapshot_context *context = cb_arg;
    struct ubi_bdev *ubi_bdev = context->ubi_bdev;
    ubi_bdev->snapshot_status.copied_clusters = copied_clusters;
}

static void ubi_start_snapshot(void *cb_arg, int bserrno) {
    struct snapshot_context *ctx = cb_arg;
    struct ubi_bdev *ubi_bdev = ctx->ubi_bdev;

    SPDK_WARNLOG("Starting snapshot for %s\n", ubi_bdev->bdev.name);

    if (bserrno != 0) {
        SPDK_ERRLOG("Failed to close clone for %s: %d\n", ubi_bdev->bdev.name, bserrno);
        ubi_bdev->snapshot_status.result = bserrno;
        cleanup_snapshot_context(ctx);
        ctx->cb_fn(ctx->cb_arg, bserrno);
        return;
    }

    ubi_bdev->snapshot_status.in_progress = true;
    ubi_bdev->snapshot_status.total_clusters =
        spdk_bs_total_data_cluster_count(ubi_bdev->blobstore);

    uint64_t cluster_size = spdk_bs_get_cluster_size(ubi_bdev->blobstore);

    ctx->shallow_copy_bs_dev =
        bs_dev_delta_create(ctx->path, ubi_bdev->bdev.blockcnt, ubi_bdev->bdev.blocklen,
                            cluster_size, BS_DEV_DELTA_WRITE);

    SPDK_WARNLOG("starting shallow copy for %s, blobid: %lu\n", ubi_bdev->bdev.name,
                 ctx->clone_blobid);
    int ret = spdk_bs_blob_shallow_copy(
        ubi_bdev->blobstore, ctx->ch, ctx->clone_blobid, ctx->shallow_copy_bs_dev,
        ubi_shallow_copy_status_cb, ctx, ubi_shallow_copy_complete_cb, ctx);
    SPDK_WARNLOG("shallow copy returned %d\n", ret);
    ctx->cb_fn(ctx->cb_arg, ret);
}

static void ubi_close_clone_cb(void *cb_arg, int bserrno) {
    struct snapshot_context *ctx = cb_arg;
    struct ubi_bdev *ubi_bdev = ctx->ubi_bdev;

    if (bserrno != 0) {
        SPDK_ERRLOG("Failed to close clone for %s: %d\n", ubi_bdev->bdev.name, bserrno);
        ubi_bdev->snapshot_status.result = bserrno;
        cleanup_snapshot_context(ctx);
        ctx->cb_fn(ctx->cb_arg, bserrno);
        return;
    }

    SPDK_WARNLOG("Clone closed for %s\n", ubi_bdev->bdev.name);

    spdk_bs_blob_decouple_parent(ubi_bdev->blobstore, ctx->ch, ctx->clone_blobid,
                                 ubi_start_snapshot, ctx);
}

static void ubi_open_clone_cb(void *cb_arg, struct spdk_blob *blob, int bserrno) {
    struct snapshot_context *ctx = cb_arg;
    struct ubi_bdev *ubi_bdev = ctx->ubi_bdev;

    if (bserrno != 0) {
        SPDK_ERRLOG("Failed to open clone for %s: %d\n", ubi_bdev->bdev.name, bserrno);
        ubi_bdev->snapshot_status.result = bserrno;
        cleanup_snapshot_context(ctx);
        ctx->cb_fn(ctx->cb_arg, bserrno);
        return;
    }

    SPDK_WARNLOG("Clone opened for %s\n", ubi_bdev->bdev.name);

    int ret = spdk_blob_set_read_only(blob);
    if (ret != 0) {
        SPDK_ERRLOG("Failed to set clone read-only for %s: %d\n", ubi_bdev->bdev.name,
                    ret);
        ubi_bdev->snapshot_status.result = ret;
        cleanup_snapshot_context(ctx);
        ctx->cb_fn(ctx->cb_arg, ret);
        return;
    }

    spdk_blob_close(blob, ubi_close_clone_cb, ctx);
}

static void ubi_clone_create_cb(void *cb_arg, spdk_blob_id blobid, int bserrno) {
    struct snapshot_context *ctx = cb_arg;
    struct ubi_bdev *ubi_bdev = ctx->ubi_bdev;

    if (bserrno != 0) {
        SPDK_ERRLOG("Failed to create clone for %s: %d\n", ubi_bdev->bdev.name, bserrno);
        ubi_bdev->snapshot_status.result = bserrno;
        cleanup_snapshot_context(ctx);
        ctx->cb_fn(ctx->cb_arg, bserrno);
        return;
    }

    SPDK_WARNLOG("Clone created for %s, blobid: %lu \n", ubi_bdev->bdev.name, blobid);

    ctx->clone_blobid = blobid;

    spdk_bs_open_blob(ubi_bdev->blobstore, blobid, ubi_open_clone_cb, ctx);
}

struct spdk_blob_xattr_opts g_xattrs = {0};

static void ubi_snapshot_create_cb(void *cb_arg, spdk_blob_id blobid, int bserrno) {
    struct snapshot_context *ctx = cb_arg;
    struct ubi_bdev *ubi_bdev = ctx->ubi_bdev;

    if (bserrno != 0) {
        SPDK_ERRLOG("Failed to create snapshot for %s: %d\n", ubi_bdev->bdev.name,
                    bserrno);
        ubi_bdev->snapshot_status.result = bserrno;
        cleanup_snapshot_context(ctx);
        ctx->cb_fn(ctx->cb_arg, bserrno);
        return;
    }

    SPDK_WARNLOG("Snapshot created for %s, blobid: %lu \n", ubi_bdev->bdev.name, blobid);

    ctx->snapshot_blobid = blobid;
    ctx->ch = spdk_bs_alloc_io_channel(ubi_bdev->blobstore);

    spdk_bs_create_clone(ubi_bdev->blobstore, blobid, &g_xattrs, ubi_clone_create_cb,
                         ctx);
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
    struct snapshot_context *ctx = calloc(1, sizeof(*ctx));
    SPDK_WARNLOG("Creating snapshot for %s, blobid: %lu\n", ubi_bdev->bdev.name,
                 ubi_bdev->blobid);

    ctx->cb_fn = cb_fn;
    ctx->cb_arg = cb_arg;
    ctx->ubi_bdev = ubi_bdev;
    ctx->path = strdup(path);
    spdk_bs_create_snapshot(ubi_bdev->blobstore, ubi_bdev->blobid, &g_xattrs,
                            ubi_snapshot_create_cb, ctx);
}
