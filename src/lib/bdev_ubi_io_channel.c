
#include "bdev_ubi_internal.h"

#include "spdk/likely.h"
#include "spdk/log.h"

/*
 * Static function forward declarations
 */
static int ubi_io_poll(void *arg);

/*
 * ubi_create_channel_cb is called when an I/O channel needs to be created. In
 * the VM world this can happen for example when VMM's firmware needs to use the
 * disk, or when the virtio device is initiated by the operating system.
 */
int ubi_create_channel_cb(void *io_device, void *ctx_buf) {
    struct ubi_bdev *ubi_bdev = io_device;
    struct ubi_io_channel *ch = ctx_buf;

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
    spdk_poller_unregister(&ch->poller);
    spdk_bs_free_io_channel(ch->bs_channel);
}

/*
 * ubi_io_poll is the poller function that is called regularly by SPDK.
 */
static int ubi_io_poll(void *arg) { return SPDK_POLLER_IDLE; }
