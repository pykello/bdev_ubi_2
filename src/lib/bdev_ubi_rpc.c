#include "spdk/bdev_module.h"
#include "spdk/log.h"
#include "spdk/rpc.h"
#include "spdk/string.h"
#include "spdk/util.h"

#include "bdev_ubi.h"
#include "bdev_ubi_internal.h"

struct rpc_construct_ubi {
    char *name;
    char *image_path;
    char *snapshot_path;
    char *base_bdev_name;
    bool no_sync;
    bool format_bdev;
    // deperacated options
    uint32_t stripe_size_kb;
    bool copy_on_read;
    bool directio;
};

static void free_rpc_construct_ubi(struct rpc_construct_ubi *req) {
    free(req->name);
    free(req->image_path);
    free(req->base_bdev_name);
}

static const struct spdk_json_object_decoder rpc_construct_ubi_decoders[] = {
    {"name", offsetof(struct rpc_construct_ubi, name), spdk_json_decode_string},
    {"image_path", offsetof(struct rpc_construct_ubi, image_path),
     spdk_json_decode_string},
    {"base_bdev", offsetof(struct rpc_construct_ubi, base_bdev_name),
     spdk_json_decode_string},
    {"format_bdev", offsetof(struct rpc_construct_ubi, format_bdev),
     spdk_json_decode_bool, true},
    {"no_sync", offsetof(struct rpc_construct_ubi, no_sync), spdk_json_decode_bool, true},
    {"directio", offsetof(struct rpc_construct_ubi, directio), spdk_json_decode_bool,
     true},
    {"snapshot_path", offsetof(struct rpc_construct_ubi, snapshot_path),
     spdk_json_decode_string, true},
    // deperacated options: stripe_size_kb, copy_on_read, directio
    {"stripe_size_kb", offsetof(struct rpc_construct_ubi, stripe_size_kb),
     spdk_json_decode_uint32, true},
    {"copy_on_read", offsetof(struct rpc_construct_ubi, copy_on_read),
     spdk_json_decode_bool, true}};

static void bdev_ubi_create_done(void *cb_arg, struct spdk_bdev *bdev, int status) {
    struct spdk_jsonrpc_request *request = cb_arg;
    if (status < 0) {
        spdk_jsonrpc_send_error_response(request, status, spdk_strerror(-status));
    } else if (status > 0) {
        spdk_jsonrpc_send_error_response_fmt(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
                                             "error code: %d.", status);
    } else {
        struct spdk_json_write_ctx *w = spdk_jsonrpc_begin_result(request);
        spdk_json_write_string(w, bdev->name);
        spdk_jsonrpc_end_result(request, w);
    }
}

/*
 * rpc_bdev_ubi_create handles an rpc request to create a bdev_ubi.
 */
static void rpc_bdev_ubi_create(struct spdk_jsonrpc_request *request,
                                const struct spdk_json_val *params) {
    struct rpc_construct_ubi req = {};
    struct spdk_ubi_bdev_opts opts = {};

    // set optional parameters. spdk_json_decode_object will overwrite if
    // provided.
    req.no_sync = false;
    req.copy_on_read = true;
    req.directio = true;
    req.format_bdev = true;

    if (spdk_json_decode_object(params, rpc_construct_ubi_decoders,
                                SPDK_COUNTOF(rpc_construct_ubi_decoders), &req)) {
        SPDK_DEBUGLOG(bdev_ubi, "spdk_json_decode_object failed\n");
        spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
                                         "spdk_json_decode_object failed");
        free_rpc_construct_ubi(&req);
        return;
    }

    opts.name = req.name;
    opts.image_path = req.image_path;
    opts.base_bdev_name = req.base_bdev_name;
    opts.no_sync = req.no_sync;
    opts.format_bdev = req.format_bdev;
    opts.directio = req.directio;
    opts.snapshot_path = req.snapshot_path;

    struct ubi_create_context *context = calloc(1, sizeof(struct ubi_create_context));
    context->done_fn = bdev_ubi_create_done;
    context->done_arg = request;

    bdev_ubi_create(&opts, context);
    free_rpc_construct_ubi(&req);
}
SPDK_RPC_REGISTER("bdev_ubi_create", rpc_bdev_ubi_create, SPDK_RPC_RUNTIME)

struct rpc_delete_ubi {
    char *name;
};

static const struct spdk_json_object_decoder rpc_delete_ubi_decoders[] = {
    {"name", offsetof(struct rpc_delete_ubi, name), spdk_json_decode_string},
};

static void rpc_bdev_ubi_delete_cb(void *cb_arg, int bdeverrno) {
    struct spdk_jsonrpc_request *request = cb_arg;

    if (bdeverrno == 0) {
        spdk_jsonrpc_send_bool_response(request, true);
    } else {
        spdk_jsonrpc_send_error_response(request, bdeverrno, spdk_strerror(-bdeverrno));
    }
}

static void rpc_bdev_ubi_delete(struct spdk_jsonrpc_request *request,
                                const struct spdk_json_val *params) {
    struct rpc_delete_ubi req = {NULL};

    if (spdk_json_decode_object(params, rpc_delete_ubi_decoders,
                                SPDK_COUNTOF(rpc_delete_ubi_decoders), &req)) {
        spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
                                         "spdk_json_decode_object failed");
        return;
    }

    bdev_ubi_delete(req.name, rpc_bdev_ubi_delete_cb, request);
    free(req.name);
}
SPDK_RPC_REGISTER("bdev_ubi_delete", rpc_bdev_ubi_delete, SPDK_RPC_RUNTIME)

struct rpc_snapshot_ubi {
    char *name;
    char *path;
};

static const struct spdk_json_object_decoder rpc_snapshot_ubi_decoders[] = {
    {"name", offsetof(struct rpc_snapshot_ubi, name), spdk_json_decode_string},
    {"path", offsetof(struct rpc_snapshot_ubi, path), spdk_json_decode_string},
};

static void rpc_bdev_ubi_snapshot_cb(void *cb_arg, int bdeverrno) {
    struct spdk_jsonrpc_request *request = cb_arg;

    if (bdeverrno == 0) {
        spdk_jsonrpc_send_bool_response(request, true);
    } else {
        spdk_jsonrpc_send_error_response(request, bdeverrno, spdk_strerror(-bdeverrno));
    }
}

static void rpc_bdev_ubi_snapshot(struct spdk_jsonrpc_request *request,
                                  const struct spdk_json_val *params) {
    struct rpc_snapshot_ubi req = {NULL};

    if (spdk_json_decode_object(params, rpc_snapshot_ubi_decoders,
                                SPDK_COUNTOF(rpc_snapshot_ubi_decoders), &req)) {
        spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
                                         "spdk_json_decode_object failed");
        return;
    }

    bdev_ubi_snapshot(req.name, req.path, rpc_bdev_ubi_snapshot_cb, request);
    free(req.name);
    free(req.path);
}
SPDK_RPC_REGISTER("bdev_ubi_snapshot", rpc_bdev_ubi_snapshot, SPDK_RPC_RUNTIME)

struct rpc_snapshot_ubi_status {
    char *name;
};

static const struct spdk_json_object_decoder rpc_snapshot_ubi_status_decoders[] = {
    {"name", offsetof(struct rpc_snapshot_ubi, name), spdk_json_decode_string}};

static void rpc_bdev_ubi_snapshot_status(struct spdk_jsonrpc_request *request,
                                         const struct spdk_json_val *params) {
    struct rpc_snapshot_ubi_status req = {NULL};
    if (spdk_json_decode_object(params, rpc_snapshot_ubi_status_decoders,
                                SPDK_COUNTOF(rpc_snapshot_ubi_status_decoders), &req)) {
        spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
                                         "spdk_json_decode_object failed");
        return;
    }

    struct spdk_bdev *bdev = spdk_bdev_get_by_name(req.name);
    if (bdev == NULL) {
        spdk_jsonrpc_send_error_response(request, -ENOENT, "bdev not found");
        return;
    }

    struct ubi_bdev *ubi_bdev = SPDK_CONTAINEROF(bdev, struct ubi_bdev, bdev);

    struct spdk_json_write_ctx *w;
    w = spdk_jsonrpc_begin_result(request);
    spdk_json_write_object_begin(w);
    spdk_json_write_named_string(w, "name", req.name);
    spdk_json_write_named_bool(w, "in_progress", ubi_bdev->snapshot_status.in_progress);
    spdk_json_write_named_int32(w, "result", ubi_bdev->snapshot_status.result);
    spdk_json_write_named_uint64(w, "copied_clusters",
                                 ubi_bdev->snapshot_status.copied_clusters);
    spdk_json_write_named_uint64(w, "total_clusters",
                                 ubi_bdev->snapshot_status.total_clusters);
    spdk_json_write_object_end(w);
    spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("bdev_ubi_snapshot_status", rpc_bdev_ubi_snapshot_status,
                  SPDK_RPC_RUNTIME)
