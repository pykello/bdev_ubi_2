
#include "bdev_ubi_internal.h"

#include "spdk/likely.h"
#include "spdk/log.h"

/*
 * Static function forward declarations
 */
static int ubi_io_poll(void *arg);
static void ubi_complete_io(struct ubi_bdev_io *ubi_io, bool success);

/*
 * ubi_create_channel_cb is called when an I/O channel needs to be created. In
 * the VM world this can happen for example when VMM's firmware needs to use the
 * disk, or when the virtio device is initiated by the operating system.
 */
int ubi_create_channel_cb(void *io_device, void *ctx_buf) {
    struct ubi_bdev *ubi_bdev = io_device;
    struct ubi_io_channel *ch = ctx_buf;

    SPDK_WARNLOG("Creating I/O channel for %s\n", ubi_bdev->bdev.name);

    ch->ubi_bdev = ubi_bdev;
    TAILQ_INIT(&ch->io);
    ch->poller = SPDK_POLLER_REGISTER(ubi_io_poll, ch, 0);

    ch->bs_channel = spdk_bs_alloc_io_channel(ubi_bdev->blobstore);
    if (ch->bs_channel == NULL) {
        UBI_ERRLOG(ubi_bdev, "could not allocate blobstore channel\n");
        return -ENOMEM;
    }

    return 0;
}

/*
 * ubi_destroy_channel_cb when an I/O channel needs to be destroyed.
 */
void ubi_destroy_channel_cb(void *io_device, void *ctx_buf) {
    struct ubi_io_channel *ch = ctx_buf;
    SPDK_WARNLOG("Destroying I/O channel for %s\n", ch->ubi_bdev->bdev.name);
    spdk_poller_unregister(&ch->poller);

    spdk_bs_free_io_channel(ch->bs_channel);
}

/*
 * ubi_io_poll is the poller function that is called regularly by SPDK.
 */
static int ubi_io_poll(void *arg) {
    struct ubi_io_channel *ch = arg;
    struct ubi_bdev *ubi_bdev = ch->ubi_bdev;

    struct spdk_io_channel *uring_ch = spdk_io_channel_from_ctx(ubi_bdev->parent_bs_dev);
    bs_dev_uring_poll(uring_ch);

    return SPDK_POLLER_BUSY;
}

/*
 * ubi_io_completion_cb cleans up and marks the I/O request as completed.
 */
void ubi_io_completion_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg) {
    spdk_bdev_free_io(bdev_io);

    struct ubi_bdev_io *ubi_io = cb_arg;
    ubi_complete_io(ubi_io, success);
}

static void ubi_complete_io(struct ubi_bdev_io *ubi_io, bool success) {
    struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(ubi_io);

    spdk_bdev_io_complete(bdev_io, success ? SPDK_BDEV_IO_STATUS_SUCCESS
                                           : SPDK_BDEV_IO_STATUS_FAILED);
}
