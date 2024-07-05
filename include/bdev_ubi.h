#ifndef SPDK_BDEV_NULL_H
#define SPDK_BDEV_NULL_H

#include "spdk/blob.h"
#include "spdk/stdinc.h"

#define DEFAULT_STRIPE_SIZE_KB 1024

typedef void (*spdk_delete_ubi_complete)(void *cb_arg, int bdeverrno);
typedef void (*spdk_snapshot_ubi_complete)(void *cb_arg, int bdeverrno);

struct spdk_bdev;
struct spdk_uuid;

/*
 * Parameters to create a ubi bdev.
 */
struct spdk_ubi_bdev_opts {
    const char *name;
    const char *image_path;
    const char *base_bdev_name;
    bool no_sync;
    bool directio;
    bool format_bdev;
};

struct ubi_create_context {
    void (*done_fn)(void *cb_arg, struct spdk_bdev *bdev, int status);
    void *done_arg;

    struct ubi_bdev *ubi_bdev;
    bool registerd;
    bool format_bdev;

    struct spdk_bs_opts bs_opts;
    struct spdk_blob_opts blob_opts;

    /* temporary channel used to read metadata */
    struct spdk_io_channel *base_ch;
};

/*
 * Functions called by rpc methods.
 */
void bdev_ubi_create(const struct spdk_ubi_bdev_opts *opts,
                     struct ubi_create_context *context);
void bdev_ubi_delete(const char *bdev_name, spdk_delete_ubi_complete cb_fn, void *cb_arg);
void bdev_ubi_snapshot(const char *name, const char *path,
                       spdk_snapshot_ubi_complete cb_fn, void *cb_arg);

#endif /* SPDK_BDEV_NULL_H */
