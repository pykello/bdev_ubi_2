#include "bdev_ubi.h"
#include "bdev_ubi_internal.h"

#include "spdk/blob.h"
#include "spdk/blob_bdev.h"
#include "spdk/fd.h"
#include "spdk/likely.h"
#include "spdk/log.h"

#include <errno.h>
#include <inttypes.h>
#include <string.h>

/*
 * Static function forward declarations
 */
static int ubi_initialize(void);
static void ubi_finish(void);
static int ubi_get_ctx_size(void);
static int ubi_destruct(void *ctx);
static void ubi_finish_create(int status, struct ubi_create_context *context);
static void ubi_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io);
static bool ubi_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type);
static struct spdk_io_channel *ubi_get_io_channel(void *ctx);
static void ubi_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w);
static void ubi_handle_base_bdev_event(enum spdk_bdev_event_type type,
                                       struct spdk_bdev *bdev, void *event_ctx);

const char UBI_URING_ESNAP_ID[] = "ubi_uring_esnap_id";
/*
 * Module Interface
 */
static struct spdk_bdev_module ubi_if = {
    .name = "ubi",
    .module_init = ubi_initialize,
    .module_fini = ubi_finish,
    .async_fini = false,
    .async_init = false,
    .get_ctx_size = ubi_get_ctx_size,
};

SPDK_BDEV_MODULE_REGISTER(ubi, &ubi_if)

/*
 * Block device operations
 */
static const struct spdk_bdev_fn_table ubi_fn_table = {
    .destruct = ubi_destruct,
    .submit_request = ubi_submit_request,
    .io_type_supported = ubi_io_type_supported,
    .get_io_channel = ubi_get_io_channel,
    .write_config_json = ubi_write_config_json,
};

static TAILQ_HEAD(, ubi_bdev) g_ubi_bdev_head = TAILQ_HEAD_INITIALIZER(g_ubi_bdev_head);

/*
 * ubi_initialize is called when the module is initialized.
 */
static int ubi_initialize(void) { return 0; }

/*
 * ubi_finish is called when the module is finished.
 */
static void ubi_finish(void) {}

/*
 * ubi_get_ctx_size returns the size of I/O cotnext.
 */
static int ubi_get_ctx_size(void) { return sizeof(struct ubi_bdev_io); }

static int ubi_esnap_dev_create(void *bs_ctx, void *blob_ctx, struct spdk_blob *blob,
                                const void *esnap_id, uint32_t id_size,
                                struct spdk_bs_dev **bs_dev) {
    SPDK_WARNLOG("esnap_dev_create\n");
    struct ubi_bdev *ubi_bdev = bs_ctx;
    uint32_t cluster_size = spdk_bs_get_cluster_size(ubi_bdev->blobstore) / ubi_bdev->bdev.blocklen;
    *bs_dev = bs_dev_uring_create(ubi_bdev->image_path, ubi_bdev->snapshot_path,
                                  ubi_bdev->bdev.blocklen, cluster_size, ubi_bdev->directio);
    return 0;
}

static void ubi_blob_open_complete(void *arg1, struct spdk_blob *blob, int bserrno) {
    struct ubi_create_context *context = arg1;

    SPDK_WARNLOG("ubi_blob_open_complete, blobid: %lx, blob: %p\n",
                 spdk_blob_get_id(blob), blob);

    if (bserrno) {
        UBI_ERRLOG(context->ubi_bdev, "Could not open blob: %s\n",
                   spdk_strerror(-bserrno));
        ubi_finish_create(bserrno, context);
        return;
    }

    context->ubi_bdev->blob = blob;
    context->ubi_bdev->blobid = spdk_blob_get_id(blob);

    SPDK_ERRLOG("ubi_blob_open_complete: %lx\n", context->ubi_bdev->blobid);

    ubi_finish_create(0, context);
}

struct spdk_blob_xattr_opts g_xattrs2 = {0};

static void ubi_blob_snapshot_complete(void *arg1, spdk_blob_id blobid, int bserrno) {
    struct ubi_create_context *context = arg1;

    SPDK_ERRLOG("ubi_blob_snapshot_complete: %lx\n", blobid);

    if (bserrno) {
        UBI_ERRLOG(context->ubi_bdev, "Could not create snapshot: %s\n",
                   spdk_strerror(-bserrno));
        ubi_finish_create(bserrno, context);
        return;
    }

    spdk_bs_open_blob(context->ubi_bdev->blobstore, context->ubi_bdev->blobid,
                      ubi_blob_open_complete, context);
}

static void ubi_blob_create_complete(void *arg1, spdk_blob_id blobid, int bserrno) {
    struct ubi_create_context *context = arg1;
    SPDK_ERRLOG("ubi_blob_create_complete: %lx\n", blobid);

    if (bserrno) {
        UBI_ERRLOG(context->ubi_bdev, "Could not create blob: %s\n",
                   spdk_strerror(-bserrno));
        ubi_finish_create(bserrno, context);
        return;
    }

    context->ubi_bdev->blobid = blobid;
    spdk_bs_create_snapshot(context->ubi_bdev->blobstore, blobid, &g_xattrs2,
                            ubi_blob_snapshot_complete, context);
}

static void ubi_bs_init_complete(void *cb_arg, struct spdk_blob_store *bs, int bserrno) {
    struct ubi_create_context *context = cb_arg;

    if (bserrno) {
        UBI_ERRLOG(context->ubi_bdev, "Could not initialize blobstore: %s\n",
                   spdk_strerror(-bserrno));
        ubi_finish_create(bserrno, context);
        return;
    }

    context->ubi_bdev->blobstore = bs;

    if (!context->format_bdev) {
        spdk_bs_iter_first(bs, ubi_blob_open_complete, context);
        return;
    }

    spdk_blob_opts_init(&context->blob_opts, sizeof(context->blob_opts));
    context->blob_opts.thin_provision = true;
    context->blob_opts.num_clusters = spdk_bs_total_data_cluster_count(bs);
    context->blob_opts.esnap_id = UBI_URING_ESNAP_ID;
    context->blob_opts.esnap_id_len = sizeof(UBI_URING_ESNAP_ID);

    spdk_bs_create_blob_ext(bs, &context->blob_opts, ubi_blob_create_complete, context);
}

/*
 * bdev_ubi_create. Creates a ubi_bdev and registers it.
 *
 * This will always call the callback stored in context->done_fn with the
 * success status.
 */
void bdev_ubi_create(const struct spdk_ubi_bdev_opts *opts,
                     struct ubi_create_context *context) {
    struct ubi_bdev *ubi_bdev;
    int rc;

    if (!opts) {
        SPDK_ERRLOG("No options provided for Ubi bdev %s.\n", opts->name);
        ubi_finish_create(-EINVAL, context);
        return;
    }

    /*
     * By using calloc() we initialize the memory region to all 0, which also
     * ensures that metadata, strip_status, and metadata_dirty are all 0
     * initially.
     */
    ubi_bdev = calloc(1, sizeof(struct ubi_bdev));
    if (!ubi_bdev) {
        SPDK_ERRLOG("could not allocate ubi_bdev %s\n", opts->name);
        ubi_finish_create(-ENOMEM, context);
        return;
    }

    /*
     * Save ubi_bdev in context so we can either register it (on success) or
     * clean it up (on failure) in ubi_finish_create().
     */
    context->ubi_bdev = ubi_bdev;

    ubi_bdev->bdev.name = strdup(opts->name);
    if (!ubi_bdev->bdev.name) {
        SPDK_ERRLOG("could not duplicate name for ubi_bdev %s\n", opts->name);
        ubi_finish_create(-ENOMEM, context);
        return;
    }

    /* Save the thread where the base device is opened. */
    ubi_bdev->thread = spdk_get_thread();

    /*
     * Initialize variables that determine the layout of both metadata and
     * actual data on base bdev.
     */
    ubi_bdev->no_sync = opts->no_sync;

    strncpy(ubi_bdev->image_path, opts->image_path, UBI_PATH_LEN);
    ubi_bdev->image_path[UBI_PATH_LEN - 1] = 0;

    if (opts->snapshot_path) {
        strncpy(ubi_bdev->snapshot_path, opts->snapshot_path, UBI_PATH_LEN);
        ubi_bdev->snapshot_path[UBI_PATH_LEN - 1] = 0;
    } else {
        ubi_bdev->snapshot_path[0] = 0;
    }

    rc = spdk_bdev_create_bs_dev_ext(opts->base_bdev_name, ubi_handle_base_bdev_event,
                                     NULL, &ubi_bdev->bs_dev);
    if (rc) {
        UBI_ERRLOG(ubi_bdev, "Could not create blob bdev: %s\n", spdk_strerror(-rc));
        ubi_finish_create(rc, context);
        return;
    }

    /* Copy some properties from the underlying base bdev. */
    ubi_bdev->bdev.blocklen = ubi_bdev->bs_dev->blocklen;
    ubi_bdev->bdev.blockcnt = ubi_bdev->bs_dev->blockcnt;
    ubi_bdev->bdev.write_cache = 0;

    ubi_bdev->bdev.product_name = "Ubi_disk";
    ubi_bdev->bdev.ctxt = ubi_bdev;
    ubi_bdev->bdev.fn_table = &ubi_fn_table;
    ubi_bdev->bdev.module = &ubi_if;

    ubi_bdev->alignment_bytes = 4096;
    ubi_bdev->bdev.required_alignment = spdk_u32log2(ubi_bdev->alignment_bytes);

    spdk_io_device_register(ubi_bdev, ubi_create_channel_cb, ubi_destroy_channel_cb,
                            sizeof(struct ubi_io_channel), ubi_bdev->bdev.name);
    context->registerd = true;
    context->format_bdev = opts->format_bdev;

    spdk_bs_opts_init(&context->bs_opts, sizeof(context->bs_opts));
    context->bs_opts.esnap_bs_dev_create = ubi_esnap_dev_create;
    context->bs_opts.esnap_ctx = ubi_bdev;

    if (opts->format_bdev) {
        spdk_bs_init(ubi_bdev->bs_dev, &context->bs_opts, ubi_bs_init_complete, context);
    } else {
        spdk_bs_load(ubi_bdev->bs_dev, &context->bs_opts, ubi_bs_init_complete, context);
    }
}

static void ubi_destruct_bs_unload_cb(void *cb_arg, int bserrno) {
    struct ubi_bdev *ubi_bdev = cb_arg;

    if (bserrno) {
        UBI_ERRLOG(ubi_bdev, "Could not unload blobstore: %s\n", spdk_strerror(-bserrno));
    }
}

static void ubi_destruct_blob_close_cb(void *cb_arg, int bserrno) {
    struct ubi_bdev *ubi_bdev = cb_arg;

    if (bserrno) {
        UBI_ERRLOG(ubi_bdev, "Could not close blob: %s\n", spdk_strerror(-bserrno));
    }

    if (ubi_bdev->blobstore) {
        spdk_bs_unload(ubi_bdev->blobstore, ubi_destruct_bs_unload_cb, ubi_bdev);
    } else {
        ubi_destruct_bs_unload_cb(ubi_bdev, 0);
    }
}

/*
 * ubi_finish_create is called when the ubi_bdev creation flow is done, either
 * by success or failure. Non-zero "status" means a failure.
 */
static void ubi_finish_create(int status, struct ubi_create_context *context) {
    struct ubi_bdev *ubi_bdev = context->ubi_bdev;

    if (status == 0) {
        ubi_bdev->bdev.blockcnt = spdk_blob_get_num_io_units(ubi_bdev->blob);
        ubi_bdev->bdev.blocklen = spdk_bs_get_io_unit_size(ubi_bdev->blobstore);
        ubi_bdev->bdev.optimal_io_boundary = (4 * 1024) / ubi_bdev->bdev.blocklen;
        ubi_bdev->bdev.split_on_optimal_io_boundary = true;
        SPDK_WARNLOG(
            "ubi_bdev %s created with %" PRIu64 " blocks of size %" PRIu32 " bytes\n",
            ubi_bdev->bdev.name, ubi_bdev->bdev.blockcnt, ubi_bdev->bdev.blocklen);

        status = spdk_bdev_register(&ubi_bdev->bdev);
        if (status != 0) {
            UBI_ERRLOG(ubi_bdev, "could not register ubi_bdev\n");
            spdk_bdev_module_release_bdev(&ubi_bdev->bdev);
        } else {
            TAILQ_INSERT_TAIL(&g_ubi_bdev_head, ubi_bdev, tailq);
        }
    }

    if (status != 0 && ubi_bdev) {
        if (context->registerd) {
            spdk_io_device_unregister(ubi_bdev, NULL);
        }

        if (ubi_bdev->blob) {
            spdk_blob_close(ubi_bdev->blob, ubi_destruct_blob_close_cb, ubi_bdev);
        } else {
            ubi_destruct_blob_close_cb(ubi_bdev, 0);
        }

        // todo: where should we free these?
        // free(ubi_bdev->bdev.name);
        // free(ubi_bdev);
    }

    context->done_fn(context->done_arg, &ubi_bdev->bdev, status);
    free(context);
}

/*
 * bdev_ubi_delete. Finds and unregisters a given bdev name.
 */
void bdev_ubi_delete(const char *bdev_name, spdk_delete_ubi_complete cb_fn,
                     void *cb_arg) {
    int rc;
    rc = spdk_bdev_unregister_by_name(bdev_name, &ubi_if, cb_fn, cb_arg);
    if (rc != 0) {
        cb_fn(cb_arg, rc);
    }
}

/* Callback for unregistering the IO device. */
static void _device_unregister_cb(void *io_device) {
    struct ubi_bdev *ubi_bdev = io_device;

    /* Done with this ubi_bdev. */
    free(ubi_bdev->bdev.name);
    free(ubi_bdev);
}

/*
 * ubi_destruct. Given a pointer to a ubi_bdev, destruct it.
 */
static int ubi_destruct(void *ctx) {
    struct ubi_bdev *ubi_bdev = ctx;

    TAILQ_REMOVE(&g_ubi_bdev_head, ubi_bdev, tailq);

    if (ubi_bdev->blob) {
        spdk_blob_close(ubi_bdev->blob, ubi_destruct_blob_close_cb, ubi_bdev);
    } else {
        ubi_destruct_blob_close_cb(ubi_bdev, 0);
    }

    /* Unregister the io_device. */
    spdk_io_device_unregister(ubi_bdev, _device_unregister_cb);

    return 0;
}

/*
 * ubi_write_config_json writes out config parameters for the given bdev to a
 * json writer.
 */
static void ubi_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w) {
    struct ubi_bdev *ubi_bdev = bdev->ctxt;

    spdk_json_write_object_begin(w);

    spdk_json_write_named_string(w, "method", "bdev_ubi_create");

    spdk_json_write_named_object_begin(w, "params");
    spdk_json_write_named_string(w, "name", bdev->name);
    spdk_json_write_named_string(w, "image_path", ubi_bdev->image_path);
    spdk_json_write_object_end(w);

    spdk_json_write_object_end(w);
}

/*
 * ubi_io_type_supported determines which I/O operations are supported.
 */
static bool ubi_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type) {
    /*
     * According to https://spdk.io/doc/bdev_module.html, only READ and WRITE
     * are necessary. We also support FLUSH to provide crash recovery.
     */
    switch (io_type) {
    case SPDK_BDEV_IO_TYPE_READ:
    case SPDK_BDEV_IO_TYPE_WRITE:
    case SPDK_BDEV_IO_TYPE_FLUSH:
        return true;
    case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
        /*
         * Write zeros to given address range. We don't support it explicitly
         * yet. Generic bdev code is capable if emulating this by sending
         * regular writes.
         */
    case SPDK_BDEV_IO_TYPE_RESET:
        /*
         * Request to abort all I/O and return the underlying device to its
         * initial state.
         *
         * Not supported yet.
         */
    case SPDK_BDEV_IO_TYPE_UNMAP:
        /*
         * Often referred to as "trim" or "deallocate", and is a request to
         * mark a set of blocks as no longer containing valid data.
         *
         * Not supported yet.
         */
    default:
        return false;
    }
}

static void ubi_blob_io_complete(void *cb_arg, int bserrno) {
    struct spdk_bdev_io *bdev_io = cb_arg;
    spdk_bdev_io_complete(bdev_io, bserrno ? SPDK_BDEV_IO_STATUS_FAILED
                                           : SPDK_BDEV_IO_STATUS_SUCCESS);
}

/*
 * ubi_submit_request is called when an I/O request arrives. It will enqueue
 * an stripe fetch if necessary, and then enqueue the I/O request so it is
 * served in the poller.
 */
static void ubi_submit_request(struct spdk_io_channel *_ch,
                               struct spdk_bdev_io *bdev_io) {
    struct ubi_io_channel *ch = spdk_io_channel_get_ctx(_ch);
    struct ubi_bdev *ubi_bdev = bdev_io->bdev->ctxt;
    struct spdk_blob *blob = ubi_bdev->blob;
    struct spdk_io_channel *blob_ch = ch->bs_channel;

    uint64_t offset = bdev_io->u.bdev.offset_blocks;
    uint64_t length = bdev_io->u.bdev.num_blocks;

    switch (bdev_io->type) {
    case SPDK_BDEV_IO_TYPE_READ:
        spdk_blob_io_readv(blob, blob_ch, bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
                           offset, length, ubi_blob_io_complete, bdev_io);
        break;
    case SPDK_BDEV_IO_TYPE_WRITE:
        spdk_blob_io_writev(blob, blob_ch, bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
                            offset, length, ubi_blob_io_complete, bdev_io);
        break;
    case SPDK_BDEV_IO_TYPE_FLUSH:
        if (ubi_bdev->no_sync) {
            spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
        } else {
            spdk_blob_sync_md(blob, ubi_blob_io_complete, bdev_io);
            // TODO: is this enough?
        }
        break;
    default:
        UBI_ERRLOG(ubi_bdev, "Unsupported I/O type %d\n", bdev_io->type);
        spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
        return;
    }
}

/*
 * ubi_get_io_channel return I/O channel pointer for the given ubi bdev.
 */
static struct spdk_io_channel *ubi_get_io_channel(void *ctx) {
    struct bdev_ubi *bdev_ubi = ctx;

    return spdk_get_io_channel(bdev_ubi);
}

/*
 * ubi_handle_base_bdev_event is callback which is called when of base
 * bdevs trigger an event, e.g. when they're removed or resized.
 */
static void ubi_handle_base_bdev_event(enum spdk_bdev_event_type type,
                                       struct spdk_bdev *bdev, void *event_ctx) {
    switch (type) {
    default:
        SPDK_NOTICELOG("Unsupported bdev event: type %d, bdev: %s\n", type, bdev->name);
        break;
    }
}

SPDK_LOG_REGISTER_COMPONENT(bdev_ubi)
