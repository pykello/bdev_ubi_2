#ifndef BDEV_UBI_INTERNAL_H
#define BDEV_UBI_INTERNAL_H

#include "spdk/bdev.h"
#include "spdk/bdev_module.h"
#include "spdk/blob.h"
#include "spdk/blob_bdev.h"
#include "spdk/env.h"
#include "spdk/json.h"
#include "spdk/stdinc.h"
#include "spdk/string.h"
#include "spdk/thread.h"

#include "bdev_ubi.h"

#include <liburing.h>

#define UBI_PATH_LEN 1024
#define MAX_CLUSTERS 1024 * 1024 * 8

/*
 * Block device's state. ubi_create creates and sets up a ubi_bdev.
 * ubi_bdev->bdev is registered with spdk. When registering, a pointer to
 * the ubi_bdev is saved in "bdev.ctxt".
 */
struct ubi_bdev {
    struct spdk_bdev bdev;

    char image_path[UBI_PATH_LEN];
    char snapshot_path[UBI_PATH_LEN];
    uint32_t alignment_bytes;
    bool no_sync;
    bool directio;

    struct spdk_bs_dev *bs_dev;

    struct spdk_blob_store *blobstore;
    spdk_blob_id blobid;
    struct spdk_blob *blob;
    char esnap_id[64];

    struct {
        bool in_progress;
        int result;
        uint64_t copied_clusters;
        uint64_t total_clusters;
    } snapshot_status;

    /*
     * Thread where ubi_bdev was initialized. It's essential to close the base
     * bdev in the same thread in which it was opened.
     */
    struct spdk_thread *thread;

    /* queue pointer */
    TAILQ_ENTRY(ubi_bdev) tailq;
};

enum ubi_io_type { UBI_BDEV_IO, UBI_STRIPE_FETCH };

struct ubi_io_op {
    enum ubi_io_type type;
};

/*
 * per I/O operation state.
 */
struct ubi_bdev_io {
    struct ubi_io_op op;

    struct ubi_bdev *ubi_bdev;
    struct ubi_io_channel *ubi_ch;

    uint64_t block_offset;
    uint64_t block_count;

    uint64_t stripes_fetched;
};

/*
 * Per thread state for ubi bdev.
 */
struct ubi_io_channel {
    struct ubi_bdev *ubi_bdev;
    struct spdk_poller *poller;
    struct spdk_io_channel *bs_channel;

    uint64_t blocks_read;
    uint64_t blocks_written;

    uint64_t active_reads;

    /* queue pointer */
    TAILQ_HEAD(, spdk_bdev_io) io;
};

enum bs_dev_delta_direction {
    BS_DEV_DELTA_READ,
    BS_DEV_DELTA_WRITE,
};

/* bdev_ubi_io_channel.c */
int ubi_create_channel_cb(void *io_device, void *ctx_buf);
void ubi_destroy_channel_cb(void *io_device, void *ctx_buf);

/* spdk_bs_dev_uring.c */
struct spdk_bs_dev *bs_dev_uring_create(const char *filename, const char *snapshot_path,
                                        uint32_t blocklen, uint32_t cluster_size,
                                        bool directio);

/* spdk_bs_dev_delta.c */
struct spdk_bs_dev *bs_dev_delta_create(const char *filename, uint64_t blockcnt,
                                        uint32_t blocklen, uint32_t cluster_size,
                                        enum bs_dev_delta_direction direction);

/* macros */
#define UBI_ERRLOG(ubi_bdev, format, ...)                                                \
    SPDK_ERRLOG("[%s] " format, ubi_bdev->bdev.name __VA_OPT__(, ) __VA_ARGS__)

int ubi_read_cluster_map(const char *filename, uint64_t *cluster_map);

#endif
