#include "bdev_ubi_internal.h"
#include "spdk/blob.h"
#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk/thread.h"
#include <liburing.h>

#define UBI_URING_QUEUE_SIZE 128

struct bs_dev_uring_io_channel {
    int image_file_fd;
    struct io_uring image_file_ring;
    struct spdk_poller *poller;
};

struct bs_dev_uring {
    struct spdk_bs_dev base;
    char filename[1024];
    bool directio;
};

int bs_dev_uring_poll(void *arg) {
    struct bs_dev_uring_io_channel *ch = arg;
    struct io_uring *ring = &ch->image_file_ring;

    struct io_uring_cqe *cqe[64];

    int ret = io_uring_peek_batch_cqe(ring, cqe, 64);
    if (ret == -EAGAIN) {
        return SPDK_POLLER_BUSY;
    } else if (ret < 0) {
        SPDK_ERRLOG("io_uring_peek_cqe: %s\n", strerror(-ret));
        return SPDK_POLLER_BUSY;
    }

    for (int i = 0; i < ret; i++) {
        struct spdk_bs_dev_cb_args *cb_args = io_uring_cqe_get_data(cqe[i]);
        if (cqe[i]->res < 0) {
            SPDK_ERRLOG("io_uring error: %s\n", strerror(-cqe[i]->res));
            cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EIO);
        } else {
            cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, 0);
        }

        /* Mark the completion as seen. */
        io_uring_cqe_seen(ring, cqe[i]);
    }
    return SPDK_POLLER_BUSY;
}

static int bs_dev_uring_create_channel_cb(void *io_device, void *ctx_buf) {
    struct bs_dev_uring *uring_dev = io_device;
    struct bs_dev_uring_io_channel *ch = ctx_buf;

    int open_flags = O_RDONLY;
    if (uring_dev->directio)
        open_flags |= O_DIRECT;
    ch->image_file_fd = open(uring_dev->filename, open_flags);
    if (ch->image_file_fd < 0) {
        SPDK_ERRLOG("could not open %s: %s\n", uring_dev->filename, strerror(errno));
        free(ch);
        return -1;
    }

    struct io_uring_params io_uring_params;
    memset(&io_uring_params, 0, sizeof(io_uring_params));
    int rc = io_uring_queue_init(UBI_URING_QUEUE_SIZE, &ch->image_file_ring, 0);
    if (rc != 0) {
        SPDK_ERRLOG("Unable to setup io_uring: %s\n", strerror(-rc));
        close(ch->image_file_fd);
        free(ch);
        return -1;
    }

    ch->poller = SPDK_POLLER_REGISTER(bs_dev_uring_poll, ch, 0);

    return 0;
}

static void bs_dev_uring_destroy_channel_cb(void *io_device, void *ctx_buf) {
    struct bs_dev_uring_io_channel *ch = ctx_buf;
    io_uring_queue_exit(&ch->image_file_ring);
    close(ch->image_file_fd);
    spdk_poller_unregister(&ch->poller);
}

static struct spdk_io_channel *bs_dev_uring_create_channel(struct spdk_bs_dev *dev) {
    struct spdk_io_channel *result = spdk_get_io_channel(dev);
    return result;
}

static void bs_dev_uring_destroy_channel(struct spdk_bs_dev *dev,
                                         struct spdk_io_channel *channel) {
    spdk_put_io_channel(channel);
}

static void bs_dev_uring_destroy(struct spdk_bs_dev *dev) {
    SPDK_WARNLOG("unregistering uring_dev: %p\n", dev);
    spdk_io_device_unregister(dev, NULL);
    // TODO: free anything?
}

static void bs_dev_uring_read(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
                              void *payload, uint64_t lba, uint32_t lba_count,
                              struct spdk_bs_dev_cb_args *cb_args) {
    struct bs_dev_uring_io_channel *ch = spdk_io_channel_get_ctx(channel);
    struct io_uring *ring = &ch->image_file_ring;

    if (lba >= dev->blockcnt) {
        cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, 0);
        return;
    }

    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    io_uring_prep_read(sqe, ch->image_file_fd, payload, lba_count * dev->blocklen,
                       lba * dev->blocklen);
    io_uring_sqe_set_data(sqe, cb_args);

    int ret = io_uring_submit(ring);
    if (ret < 0) {
        SPDK_ERRLOG("io_uring_submit error: %s\n", strerror(-ret));
        cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EIO);
    }
}

static void bs_dev_uring_readv(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
                               struct iovec *iov, int iovcnt, uint64_t lba,
                               uint32_t lba_count, struct spdk_bs_dev_cb_args *cb_args) {
    struct bs_dev_uring_io_channel *ch = spdk_io_channel_get_ctx(channel);
    struct io_uring *ring = &ch->image_file_ring;

    if (lba >= dev->blockcnt) {
        cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, 0);
        return;
    }

    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    io_uring_prep_readv(sqe, ch->image_file_fd, iov, iovcnt, lba * dev->blocklen);
    io_uring_sqe_set_data(sqe, cb_args);

    int ret = io_uring_submit(ring);
    if (ret < 0) {
        SPDK_ERRLOG("io_uring_submit error: %s\n", strerror(-ret));
        cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EIO);
    }
}

static void bs_dev_uring_readv_ext(struct spdk_bs_dev *dev,
                                   struct spdk_io_channel *channel, struct iovec *iov,
                                   int iovcnt, uint64_t lba, uint32_t lba_count,
                                   struct spdk_bs_dev_cb_args *cb_args,
                                   struct spdk_blob_ext_io_opts *ext_io_opts) {
    bs_dev_uring_readv(dev, channel, iov, iovcnt, lba, lba_count, cb_args);
}

static void bs_dev_uring_write(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
                               void *payload, uint64_t lba, uint32_t lba_count,
                               struct spdk_bs_dev_cb_args *cb_args) {
    SPDK_ERRLOG("write not supported\n");
    cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -ENOTSUP);
}

static void bs_dev_uring_writev(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
                                struct iovec *iov, int iovcnt, uint64_t lba,
                                uint32_t lba_count, struct spdk_bs_dev_cb_args *cb_args) {
    SPDK_ERRLOG("writev not supported\n");
    cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -ENOTSUP);
}

static void bs_dev_uring_writev_ext(struct spdk_bs_dev *dev,
                                    struct spdk_io_channel *channel, struct iovec *iov,
                                    int iovcnt, uint64_t lba, uint32_t lba_count,
                                    struct spdk_bs_dev_cb_args *cb_args,
                                    struct spdk_blob_ext_io_opts *ext_io_opts) {
    SPDK_ERRLOG("writev_ext not supported\n");
    cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -ENOTSUP);
}

static void bs_dev_uring_flush(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
                               struct spdk_bs_dev_cb_args *cb_args) {
    SPDK_ERRLOG("flush not supported\n");
    cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -ENOTSUP);
}

static void bs_dev_uring_write_zeroes(struct spdk_bs_dev *dev,
                                      struct spdk_io_channel *channel, uint64_t lba,
                                      uint64_t lba_count,
                                      struct spdk_bs_dev_cb_args *cb_args) {
    SPDK_ERRLOG("write_zeroes not supported\n");
    cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -ENOTSUP);
}

static void bs_dev_uring_unmap(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
                               uint64_t lba, uint64_t lba_count,
                               struct spdk_bs_dev_cb_args *cb_args) {
    SPDK_ERRLOG("unmap not supported\n");
    cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -ENOTSUP);
}

static struct spdk_bdev *bs_dev_uring_get_base_bdev(struct spdk_bs_dev *dev) {
    return NULL;
}

static bool bs_dev_uring_is_range_valid(struct spdk_bs_dev *dev, uint64_t lba,
                                        uint64_t lba_count) {
    struct bs_dev_uring *uring_dev = (struct bs_dev_uring *)dev;
    if (lba >= uring_dev->base.blockcnt) {
        return false;
    } else if (lba + lba_count > uring_dev->base.blockcnt) {
        SPDK_ERRLOG("Entire range must be within the bs_dev bounds for CoW.\n"
                    "lba(lba_count): %lu(%lu), num_blks: %lu\n",
                    lba, lba_count, uring_dev->base.blockcnt);
        return false;
    }

    return true;
}

static bool bs_dev_uring_is_zeroes(struct spdk_bs_dev *dev, uint64_t lba,
                                   uint64_t lba_count) {
    return !bs_dev_uring_is_range_valid(dev, lba, lba_count);
}

static bool bs_dev_uring_translate_lba(struct spdk_bs_dev *dev, uint64_t lba,
                                       uint64_t *base_lba) {
    *base_lba = lba;
    return true;
}

static void bs_dev_uring_copy(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
                              uint64_t dst_lba, uint64_t src_lba, uint64_t lba_count,
                              struct spdk_bs_dev_cb_args *cb_args) {
    SPDK_ERRLOG("copy not supported\n");
    cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -ENOTSUP);
}

static bool bs_dev_uring_is_degraded(struct spdk_bs_dev *dev) { return false; }

struct spdk_bs_dev *bs_dev_uring_create(const char *filename, uint32_t blocklen,
                                        bool directio) {
    struct bs_dev_uring *uring_dev = calloc(1, sizeof *uring_dev);
    if (uring_dev == NULL) {
        SPDK_ERRLOG("could not allocate uring_dev\n");
        return NULL;
    }

    struct stat statBuffer;
    if (stat(filename, &statBuffer) != 0) {
        SPDK_ERRLOG("could not stat %s: %s\n", filename, strerror(errno));
        free(uring_dev);
        return NULL;
    }

    uring_dev->base.blockcnt = statBuffer.st_size / blocklen;
    uring_dev->base.blocklen = blocklen;

    strcpy(uring_dev->filename, filename);
    uring_dev->directio = directio;
    struct spdk_bs_dev *dev = &uring_dev->base;
    dev->create_channel = bs_dev_uring_create_channel;
    dev->destroy = bs_dev_uring_destroy;
    dev->destroy_channel = bs_dev_uring_destroy_channel;
    dev->read = bs_dev_uring_read;
    dev->write = bs_dev_uring_write;
    dev->readv = bs_dev_uring_readv;
    dev->writev = bs_dev_uring_writev;
    dev->readv_ext = bs_dev_uring_readv_ext;
    dev->writev_ext = bs_dev_uring_writev_ext;
    dev->flush = bs_dev_uring_flush;
    dev->write_zeroes = bs_dev_uring_write_zeroes;
    dev->unmap = bs_dev_uring_unmap;
    dev->get_base_bdev = bs_dev_uring_get_base_bdev;
    dev->is_zeroes = bs_dev_uring_is_zeroes;
    dev->is_range_valid = bs_dev_uring_is_range_valid;
    dev->translate_lba = bs_dev_uring_translate_lba;
    dev->copy = bs_dev_uring_copy;
    dev->is_degraded = bs_dev_uring_is_degraded;

    SPDK_WARNLOG("registering uring_dev: %p\n", dev);

    spdk_io_device_register(dev, bs_dev_uring_create_channel_cb,
                            bs_dev_uring_destroy_channel_cb,
                            sizeof(struct bs_dev_uring_io_channel), NULL);

    return dev;
}
