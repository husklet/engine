// hl/linux_abi -- the FILESYSTEM implementation of the checkpoint sink (see ckpt_sink.h).
//
// This is the historical on-disk checkpoint format, unchanged: a workspace directory holding one
// "proc.<gpid>" directory per process (staged as "<name>.tmp.<pid>" and renamed into place), workspace-level
// shared-object records published by temp+rename, ".claim" election markers, and a MANIFEST written LAST.
// Every byte now leaves the engine through hl_host_services->file.
//
// The one thing that is NOT expressible through the host contract is directory ENUMERATION-free removal of a
// stale staging directory; ckpt_rmrf (raw opendir/readdir) is reused for that and is listed as a known gap in
// docs/checkpoint-sink.md.

#ifndef HL_LINUX_ABI_CKPT_SINK_DIR_H
#define HL_LINUX_ABI_CKPT_SINK_DIR_H

#include "ckpt_sink.h"

static int ckpt_rmrf(const char *path);

#define CKPT_SINK_BUFFER 65536u

struct ckpt_sink_stream {
    struct ckpt_sink *sink;
    hl_host_handle handle;
    uint64_t position;      // logical end-of-stream (the sequential write cursor)
    size_t buffered;        // bytes held in `buffer`, logically at [position - buffered, position)
    uint32_t flags;
    int failed;
    char staging[1500];     // path actually written
    char published[1500];   // final path; empty when staging == published
    unsigned char buffer[CKPT_SINK_BUFFER];
};

static const hl_host_services *ckpt_sink_services(void) {
    const hl_host_services *services = effective_host_services();
    if (!services || !services->file || !services->file->open_relative || !services->file->write_at ||
        !services->file->sync || !services->file->close || !services->file->rename_relative ||
        !services->file->unlink_relative || !services->file->make_directory)
        return NULL;
    return services;
}

static int ckpt_sink_unlink_path(const char *path) {
    const hl_host_services *services = ckpt_sink_services();
    if (!services) return -1;
    return services->file->unlink_relative(services->context, HL_HOST_HANDLE_CWD, path, strlen(path)).status ==
                   HL_STATUS_OK
               ? 0
               : -1;
}

// fsync a directory so a rename that happened inside it is durable (the old ckpt_sync_dir).
static int ckpt_sink_sync_directory(const char *path) {
    const hl_host_services *services = ckpt_sink_services();
    if (!services) return -1;
    hl_host_result opened = services->file->open_relative(services->context, HL_HOST_HANDLE_CWD, path,
                                                          strlen(path), HL_HOST_FILE_READ, 0, 0);
    if (opened.status != HL_STATUS_OK) return -1;
    int failed = services->file->sync(services->context, opened.value).status != HL_STATUS_OK;
    if (services->file->close(services->context, opened.value).status != HL_STATUS_OK) failed = 1;
    return failed ? -1 : 0;
}

static int ckpt_sink_dir_make(const char *path, int tolerate_existing) {
    const hl_host_services *services = ckpt_sink_services();
    if (!services) return -1;
    hl_host_result made =
        services->file->make_directory(services->context, HL_HOST_HANDLE_CWD, path, strlen(path), 0700);
    if (made.status == HL_STATUS_OK) return 0;
    return tolerate_existing ? 0 : -1;
}

// Absolute path of `name` inside `group` (NULL == workspace root).
static void ckpt_sink_dir_path(struct ckpt_sink *sink, const char *group, const char *name, char *out,
                               size_t size) {
    if (group)
        snprintf(out, size, "%s/%s.tmp.%d/%s", sink->root, group, (int)getpid(), name);
    else
        snprintf(out, size, "%s/%s", sink->root, name);
}

static int ckpt_sink_dir_flush(struct ckpt_sink_stream *stream) {
    if (stream->failed) return -1;
    if (stream->buffered == 0) return 0;
    const hl_host_services *services = ckpt_sink_services();
    if (!services) return (stream->failed = 1), -1;
    uint64_t offset = stream->position - stream->buffered;
    size_t done = 0;
    while (done < stream->buffered) {
        hl_host_const_bytes input = {stream->buffer + done, stream->buffered - done};
        hl_host_result written =
            services->file->write_at(services->context, stream->handle, offset + done, input);
        if (written.status != HL_STATUS_OK || written.value == 0 ||
            written.value > stream->buffered - done) {
            stream->failed = 1;
            return -1;
        }
        done += (size_t)written.value;
    }
    stream->buffered = 0;
    return 0;
}

static int ckpt_sink_dir_write(struct ckpt_sink_stream *stream, const void *data, size_t size) {
    if (stream->failed) return -1;
    const unsigned char *bytes = data;
    while (size != 0) {
        if (stream->buffered == CKPT_SINK_BUFFER && ckpt_sink_dir_flush(stream) != 0) return -1;
        size_t room = CKPT_SINK_BUFFER - stream->buffered;
        size_t chunk = size < room ? size : room;
        memcpy(stream->buffer + stream->buffered, bytes, chunk);
        stream->buffered += chunk;
        stream->position += chunk;
        bytes += chunk;
        size -= chunk;
    }
    return 0;
}

// Patch bytes already emitted. The buffer is flushed first so the target range is always on the object.
static int ckpt_sink_dir_write_at(struct ckpt_sink_stream *stream, uint64_t offset, const void *data,
                                  size_t size) {
    if (stream->failed || ckpt_sink_dir_flush(stream) != 0) return -1;
    const hl_host_services *services = ckpt_sink_services();
    if (!services) return (stream->failed = 1), -1;
    size_t done = 0;
    while (done < size) {
        hl_host_const_bytes input = {(const unsigned char *)data + done, size - done};
        hl_host_result written =
            services->file->write_at(services->context, stream->handle, offset + done, input);
        if (written.status != HL_STATUS_OK || written.value == 0 || written.value > size - done) {
            stream->failed = 1;
            return -1;
        }
        done += (size_t)written.value;
    }
    if (offset + size > stream->position) stream->position = offset + size;
    return 0;
}

static int64_t ckpt_sink_dir_tell(struct ckpt_sink_stream *stream) {
    return stream->failed ? -1 : (int64_t)stream->position;
}

static void ckpt_sink_dir_close_handle(struct ckpt_sink_stream *stream, int *failed) {
    const hl_host_services *services = ckpt_sink_services();
    if (!services) {
        *failed = 1;
        return;
    }
    if (services->file->close(services->context, stream->handle).status != HL_STATUS_OK) *failed = 1;
}

static int ckpt_sink_dir_finish(struct ckpt_sink_stream *stream) {
    const hl_host_services *services = ckpt_sink_services();
    int failed = stream->failed || !services;
    if (!failed && ckpt_sink_dir_flush(stream) != 0) failed = 1;
    if (!failed && services->file->sync(services->context, stream->handle).status != HL_STATUS_OK) failed = 1;
    ckpt_sink_dir_close_handle(stream, &failed);
    if (!failed && stream->published[0]) {
        hl_host_result renamed = services->file->rename_relative(
            services->context, HL_HOST_HANDLE_CWD, stream->staging, strlen(stream->staging),
            HL_HOST_HANDLE_CWD, stream->published, strlen(stream->published));
        if (renamed.status != HL_STATUS_OK) failed = 1;
    }
    if (failed) ckpt_sink_unlink_path(stream->staging);
    free(stream);
    return failed ? -1 : 0;
}

static void ckpt_sink_dir_abort(struct ckpt_sink_stream *stream) {
    int ignored = 0;
    ckpt_sink_dir_close_handle(stream, &ignored);
    ckpt_sink_unlink_path(stream->staging);
    free(stream);
}

static int ckpt_sink_dir_begin(struct ckpt_sink *sink, const char *group, const char *name, uint32_t flags,
                               struct ckpt_sink_stream **out) {
    const hl_host_services *services = ckpt_sink_services();
    if (!services) return -1;
    struct ckpt_sink_stream *stream = calloc(1, sizeof *stream);
    if (!stream) return -1;
    stream->sink = sink;
    stream->flags = flags;
    char target[1500];
    ckpt_sink_dir_path(sink, group, name, target, sizeof target);
    if ((flags & CKPT_SINK_PUBLISH_ATOMIC) != 0) {
        snprintf(stream->published, sizeof stream->published, "%s", target);
        snprintf(stream->staging, sizeof stream->staging, "%s.tmp.%d", target, (int)getpid());
    } else {
        snprintf(stream->staging, sizeof stream->staging, "%s", target);
    }
    hl_host_result opened = services->file->open_relative(
        services->context, HL_HOST_HANDLE_CWD, stream->staging, strlen(stream->staging), HL_HOST_FILE_WRITE,
        HL_HOST_FILE_CREATE | HL_HOST_FILE_TRUNCATE, 0600);
    if (opened.status != HL_STATUS_OK) {
        free(stream);
        return -1;
    }
    stream->handle = opened.value;
    *out = stream;
    return 0;
}

static int ckpt_sink_dir_group_begin(struct ckpt_sink *sink, const char *group) {
    char staging[1400];
    ckpt_sink_dir_make(sink->root, 1); // idempotent: a peer may arrive before the coordinator
    snprintf(staging, sizeof staging, "%s/%s.tmp.%d", sink->root, group, (int)getpid());
    ckpt_rmrf(staging);
    if (ckpt_sink_dir_make(staging, 0) != 0) {
        fprintf(stderr, "[ckpt] mkdir %s: %s\n", staging, strerror(errno));
        return -1;
    }
    return 0;
}

static int ckpt_sink_dir_group_commit(struct ckpt_sink *sink, const char *group) {
    const hl_host_services *services = ckpt_sink_services();
    char staging[1400], published[1400];
    snprintf(staging, sizeof staging, "%s/%s.tmp.%d", sink->root, group, (int)getpid());
    snprintf(published, sizeof published, "%s/%s", sink->root, group);
    if (!services || ckpt_sink_sync_directory(staging) != 0) return -1;
    ckpt_rmrf(published);
    hl_host_result renamed =
        services->file->rename_relative(services->context, HL_HOST_HANDLE_CWD, staging, strlen(staging),
                                        HL_HOST_HANDLE_CWD, published, strlen(published));
    if (renamed.status != HL_STATUS_OK) {
        fprintf(stderr, "[ckpt] rename %s -> %s: %s\n", staging, published, strerror(errno));
        ckpt_rmrf(staging);
        return -1;
    }
    return ckpt_sink_sync_directory(sink->root);
}

static void ckpt_sink_dir_group_abort(struct ckpt_sink *sink, const char *group) {
    char staging[1400];
    snprintf(staging, sizeof staging, "%s/%s.tmp.%d", sink->root, group, (int)getpid());
    ckpt_rmrf(staging);
}

static int ckpt_sink_dir_claim(struct ckpt_sink *sink, const char *name) {
    const hl_host_services *services = ckpt_sink_services();
    if (!services) return -1;
    char path[1400];
    snprintf(path, sizeof path, "%s/%s.claim", sink->root, name);
    hl_host_result opened =
        services->file->open_relative(services->context, HL_HOST_HANDLE_CWD, path, strlen(path),
                                      HL_HOST_FILE_WRITE, HL_HOST_FILE_CREATE | HL_HOST_FILE_EXCLUSIVE, 0600);
    if (opened.status != HL_STATUS_OK) {
        // Exclusive create lost: either a peer already owns the marker (the normal race) or the workspace is
        // broken. Distinguish by existence rather than by errno, which the host backend owns.
        hl_host_result probe = services->file->open_relative(services->context, HL_HOST_HANDLE_CWD, path,
                                                            strlen(path), HL_HOST_FILE_READ, 0, 0);
        if (probe.status != HL_STATUS_OK) return -1;
        services->file->close(services->context, probe.value);
        return 1;
    }
    services->file->close(services->context, opened.value);
    return 0;
}

static void ckpt_sink_dir_unclaim(struct ckpt_sink *sink, const char *name) {
    char path[1400];
    snprintf(path, sizeof path, "%s/%s.claim", sink->root, name);
    ckpt_sink_unlink_path(path);
}

// Explicit completion: the manifest is the LAST object of the image, and its durability plus the workspace
// directory sync is what makes the checkpoint restorable.
static int ckpt_sink_dir_commit(struct ckpt_sink *sink, const void *manifest, size_t size) {
    if (ckpt_sink_put(sink, NULL, "MANIFEST", 0, manifest, size) != 0) return -1;
    return ckpt_sink_sync_directory(sink->root);
}

static const ckpt_sink_vtable g_ckpt_sink_dir_ops = {
    .begin = ckpt_sink_dir_begin,
    .write = ckpt_sink_dir_write,
    .write_at = ckpt_sink_dir_write_at,
    .tell = ckpt_sink_dir_tell,
    .finish = ckpt_sink_dir_finish,
    .abort = ckpt_sink_dir_abort,
    .group_begin = ckpt_sink_dir_group_begin,
    .group_commit = ckpt_sink_dir_group_commit,
    .group_abort = ckpt_sink_dir_group_abort,
    .claim = ckpt_sink_dir_claim,
    .unclaim = ckpt_sink_dir_unclaim,
    .commit = ckpt_sink_dir_commit,
};

static struct ckpt_sink g_ckpt_sink;

// Bind the process-wide checkpoint sink to a workspace directory. A later phase replaces this with a
// caller-supplied implementation; nothing else in checkpoint.c knows which one is installed.
static struct ckpt_sink *ckpt_sink_bind_directory(const char *root) {
    g_ckpt_sink.ops = &g_ckpt_sink_dir_ops;
    snprintf(g_ckpt_sink.root, sizeof g_ckpt_sink.root, "%s", root);
    return &g_ckpt_sink;
}

static struct ckpt_sink *ckpt_sink_current(void) {
    return g_ckpt_sink.ops ? &g_ckpt_sink : NULL;
}

#endif
