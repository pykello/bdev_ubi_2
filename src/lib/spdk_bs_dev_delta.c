#include "bdev_ubi_internal.h"
#include "spdk/blob.h"
#include "spdk/env.h"
#include "spdk/log.h"
#include "spdk/thread.h"
#include <liburing.h>

#define UBI_URING_QUEUE_SIZE 128

struct bs_dev_delta_io_channel {
    int delta_file_fd;
    struct io_uring image_file_ring;
    struct spdk_poller *poller;

    uint64_t cluster_map[1024 * 1024];
    bool initialized;
};

struct bs_dev_delta {
    struct spdk_bs_dev base;
    char filename[1024];
    enum bs_dev_delta_direction direction;
    bool directio;
    uint32_t cluster_size;
};

int ubi_read_cluster_map(const char *filename, uint64_t *cluster_map) {
    int fd = open(filename, O_RDONLY);
    if (fd < 0) {
        SPDK_ERRLOG("could not open %s: %s\n", filename, strerror(errno));
        return -1;
    }

    ssize_t n = read(fd, cluster_map, sizeof(cluster_map));
    if (n < 0) {
        SPDK_ERRLOG("could not read cluster_map: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

int bs_dev_delta_poll(void *arg) {
    struct bs_dev_delta_io_channel *ch = arg;
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

static int bs_dev_delta_create_channel_cb(void *io_device, void *ctx_buf) {
    struct bs_dev_delta *delta_dev = io_device;
    struct bs_dev_delta_io_channel *ch = ctx_buf;

    if (delta_dev->direction == BS_DEV_DELTA_WRITE) {
        ch->delta_file_fd = open(delta_dev->filename, O_RDWR | O_CREAT, 0644);
    } else {
        ch->delta_file_fd = open(delta_dev->filename, O_RDONLY);
    }
    if (ch->delta_file_fd < 0) {
        SPDK_ERRLOG("could not open %s: %s\n", delta_dev->filename, strerror(errno));
        free(ch);
        return -1;
    }

    memset(ch->cluster_map, 0, sizeof(ch->cluster_map));
    ch->initialized = false;

    // read cluster_map using linux read
    if (delta_dev->direction == BS_DEV_DELTA_READ) {
        ssize_t n = read(ch->delta_file_fd, ch->cluster_map, sizeof(ch->cluster_map));
        if (n < 0) {
            SPDK_ERRLOG("could not read cluster_map: %s\n", strerror(errno));
            close(ch->delta_file_fd);
            free(ch);
            return -1;
        }
        ch->initialized = true;
    }

    if (delta_dev->direction == BS_DEV_DELTA_WRITE) {
        // reserve the space for the cluster_map using linux write
        ssize_t n = write(ch->delta_file_fd, ch->cluster_map, sizeof(ch->cluster_map));
        if (n < 0) {
            SPDK_ERRLOG("could not write cluster_map: %s\n", strerror(errno));
            close(ch->delta_file_fd);
            free(ch);
            return -1;
        }
    }

    struct io_uring_params io_uring_params;
    memset(&io_uring_params, 0, sizeof(io_uring_params));
    int rc = io_uring_queue_init(UBI_URING_QUEUE_SIZE, &ch->image_file_ring, 0);
    if (rc != 0) {
        SPDK_ERRLOG("Unable to setup io_uring: %s\n", strerror(-rc));
        close(ch->delta_file_fd);
        free(ch);
        return -1;
    }

    ch->poller = SPDK_POLLER_REGISTER(bs_dev_delta_poll, ch, 0);

    return 0;
}

static void bs_dev_delta_destroy_channel_cb(void *io_device, void *ctx_buf) {
    struct bs_dev_delta_io_channel *ch = ctx_buf;
    struct bs_dev_delta *delta_dev = io_device;

    // write cluster_map using linux write
    if (delta_dev->direction == BS_DEV_DELTA_WRITE) {
        int ret = lseek(ch->delta_file_fd, 0, SEEK_SET);
        if (ret < 0) {
            SPDK_ERRLOG("could not seek to beginning of file: %s\n", strerror(errno));
        }
        ssize_t n = write(ch->delta_file_fd, ch->cluster_map, sizeof(ch->cluster_map));
        if (n < 0) {
            SPDK_ERRLOG("could not write cluster_map: %s\n", strerror(errno));
        }
    }
    io_uring_queue_exit(&ch->image_file_ring);
    close(ch->delta_file_fd);
    spdk_poller_unregister(&ch->poller);
}

static struct spdk_io_channel *bs_dev_delta_create_channel(struct spdk_bs_dev *dev) {
    struct spdk_io_channel *result = spdk_get_io_channel(dev);
    return result;
}

static void bs_dev_delta_destroy_channel(struct spdk_bs_dev *dev,
                                         struct spdk_io_channel *channel) {
    spdk_put_io_channel(channel);
}

static void bs_dev_delta_destroy(struct spdk_bs_dev *dev) {
    spdk_io_device_unregister(dev, NULL);
    // TODO: free anything?
}

static void bs_dev_delta_read(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
                              void *payload, uint64_t lba, uint32_t lba_count,
                              struct spdk_bs_dev_cb_args *cb_args) {
    SPDK_ERRLOG("read lba=%lu lba_count=%u\n", lba, lba_count);
    // TODO
    cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -ENOTSUP);
}

static void bs_dev_delta_readv(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
                               struct iovec *iov, int iovcnt, uint64_t lba,
                               uint32_t lba_count, struct spdk_bs_dev_cb_args *cb_args) {
    SPDK_ERRLOG("readv lba=%lu lba_count=%u\n", lba, lba_count);
    struct bs_dev_delta_io_channel *ch = spdk_io_channel_get_ctx(channel);
    // check if the cluster is in the cluster_map
    // if not, read from the base device
    if (ch->cluster_map[lba] == 0) {
        // read from base device
        // TODO
        cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -ENOTSUP);
    } else {
        // read from delta device
        // TODO
        cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -ENOTSUP);
    }
    cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -ENOTSUP);
}

static void bs_dev_delta_readv_ext(struct spdk_bs_dev *dev,
                                   struct spdk_io_channel *channel, struct iovec *iov,
                                   int iovcnt, uint64_t lba, uint32_t lba_count,
                                   struct spdk_bs_dev_cb_args *cb_args,
                                   struct spdk_blob_ext_io_opts *ext_io_opts) {
    SPDK_ERRLOG("readv_ext lba=%lu lba_count=%u\n", lba, lba_count);
    bs_dev_delta_readv(dev, channel, iov, iovcnt, lba, lba_count, cb_args);
}

static void bs_dev_delta_write(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
                               void *payload, uint64_t lba, uint32_t lba_count,
                               struct spdk_bs_dev_cb_args *cb_args) {
    struct bs_dev_delta_io_channel *ch = spdk_io_channel_get_ctx(channel);
    uint64_t size = dev->blocklen * lba_count;
    SPDK_ERRLOG("write lba=%lu lba_count=%u, size=%lu\n", lba, lba_count, size);

    int ret = write(ch->delta_file_fd, payload, size);
    if (ret < 0) {
        SPDK_ERRLOG("could not write to delta file: %s\n", strerror(errno));
        cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -EIO);
        return;
    }
    // TODO
    cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, 0);
}

static void bs_dev_delta_writev(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
                                struct iovec *iov, int iovcnt, uint64_t lba,
                                uint32_t lba_count, struct spdk_bs_dev_cb_args *cb_args) {
    SPDK_ERRLOG("writev lba=%lu lba_count=%u, len: %lu\n", lba, lba_count,
                iov[0].iov_len);

    // TODO
    cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, 0);
}

static void bs_dev_delta_writev_ext(struct spdk_bs_dev *dev,
                                    struct spdk_io_channel *channel, struct iovec *iov,
                                    int iovcnt, uint64_t lba, uint32_t lba_count,
                                    struct spdk_bs_dev_cb_args *cb_args,
                                    struct spdk_blob_ext_io_opts *ext_io_opts) {
    SPDK_ERRLOG("writev_ext lba=%lu lba_count=%u\n", lba, lba_count);
    // TODO
    cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, 0);
}

static void bs_dev_delta_flush(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
                               struct spdk_bs_dev_cb_args *cb_args) {
    SPDK_ERRLOG("flush\n");
    // TODO
    cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -ENOTSUP);
}

static void bs_dev_delta_write_zeroes(struct spdk_bs_dev *dev,
                                      struct spdk_io_channel *channel, uint64_t lba,
                                      uint64_t lba_count,
                                      struct spdk_bs_dev_cb_args *cb_args) {
    SPDK_ERRLOG("write_zeroes not supported\n");
    cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -ENOTSUP);
}

static void bs_dev_delta_unmap(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
                               uint64_t lba, uint64_t lba_count,
                               struct spdk_bs_dev_cb_args *cb_args) {
    SPDK_ERRLOG("unmap not supported\n");
    cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -ENOTSUP);
}

static struct spdk_bdev *bs_dev_delta_get_base_bdev(struct spdk_bs_dev *dev) {
    return NULL;
}

static bool bs_dev_delta_is_range_valid(struct spdk_bs_dev *dev, uint64_t lba,
                                        uint64_t lba_count) {
    // TODO
    return true;
}

static bool bs_dev_delta_is_zeroes(struct spdk_bs_dev *dev, uint64_t lba,
                                   uint64_t lba_count) {
    return !bs_dev_delta_is_range_valid(dev, lba, lba_count);
}

static bool bs_dev_delta_translate_lba(struct spdk_bs_dev *dev, uint64_t lba,
                                       uint64_t *base_lba) {
    *base_lba = lba;
    return true;
}

static void bs_dev_delta_copy(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
                              uint64_t dst_lba, uint64_t src_lba, uint64_t lba_count,
                              struct spdk_bs_dev_cb_args *cb_args) {
    SPDK_ERRLOG("copy not supported\n");
    cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -ENOTSUP);
}

static bool bs_dev_delta_is_degraded(struct spdk_bs_dev *dev) { return false; }

struct spdk_bs_dev *bs_dev_delta_create(const char *filename, uint64_t blockcnt,
                                        uint32_t blocklen, uint32_t cluster_size,
                                        enum bs_dev_delta_direction direction) {
    struct bs_dev_delta *delta_dev = calloc(1, sizeof *delta_dev);
    if (delta_dev == NULL) {
        SPDK_ERRLOG("could not allocate delta_dev\n");
        return NULL;
    }

    delta_dev->base.blockcnt = blockcnt;
    delta_dev->base.blocklen = blocklen;
    delta_dev->cluster_size = cluster_size;
    delta_dev->direction = direction;

    SPDK_WARNLOG("creating delta device. filename=%s blockcnt=%lu blocklen=%u "
                 "cluster_size=%u direction=%d\n",
                 filename, blockcnt, blocklen, cluster_size, direction);

    strcpy(delta_dev->filename, filename);
    struct spdk_bs_dev *dev = &delta_dev->base;
    dev->create_channel = bs_dev_delta_create_channel;
    dev->destroy = bs_dev_delta_destroy;
    dev->destroy_channel = bs_dev_delta_destroy_channel;
    dev->read = bs_dev_delta_read;
    dev->write = bs_dev_delta_write;
    dev->readv = bs_dev_delta_readv;
    dev->writev = bs_dev_delta_writev;
    dev->readv_ext = bs_dev_delta_readv_ext;
    dev->writev_ext = bs_dev_delta_writev_ext;
    dev->flush = bs_dev_delta_flush;
    dev->write_zeroes = bs_dev_delta_write_zeroes;
    dev->unmap = bs_dev_delta_unmap;
    dev->get_base_bdev = bs_dev_delta_get_base_bdev;
    dev->is_zeroes = bs_dev_delta_is_zeroes;
    dev->is_range_valid = bs_dev_delta_is_range_valid;
    dev->translate_lba = bs_dev_delta_translate_lba;
    dev->copy = bs_dev_delta_copy;
    dev->is_degraded = bs_dev_delta_is_degraded;

    spdk_io_device_register(dev, bs_dev_delta_create_channel_cb,
                            bs_dev_delta_destroy_channel_cb,
                            sizeof(struct bs_dev_delta_io_channel), NULL);

    return dev;
}
