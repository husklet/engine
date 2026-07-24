// hl/linux_abi -- checkpoint SOURCE: the read side of the sink, and the only way the restore driver is
// allowed to obtain image bytes.
//
// Streaming a checkpoint out is half a feature if it cannot be streamed back in. Restore, however, does not
// consume the image as a stream: it opens objects by name, seeks inside them, and enumerates the workspace to
// discover the process tree. So the source is deliberately NOT the mirror image of the sink's byte stream --
// it is a small random-access, enumerable interface:
//
//   size(name)                 -> object length, or -1 when it does not exist
//   read(name, offset, out, n) -> bytes, short at end of object
//   list(prefix)               -> the object names under a prefix (what opendir provided)
//   digest()                   -> the image digest, to authenticate the manifest
//
// Two implementations: the directory source (the historical workspace, unchanged on disk) and the streaming
// source, which asks the embedder over the same channel the sink writes to.
//
// THE FILE* SEAM. The restore driver is ~40 call sites of fopen/fread/fseek over image paths. Rewriting all
// of them into an explicit cursor API would be a large, risky, behaviour-preserving-by-inspection change, so
// instead a source object is materialised into memory and handed back as a FILE* over that memory. For the
// directory source this is still a plain fopen, so the historical path is byte-for-byte what it always was.
// For the streaming source it means one object at a time is held in the restoring process's address space --
// which is the honest cost of a store that cannot be mapped or seeked, and is bounded by the largest single
// object (a process's `pages` image).

#ifndef HL_LINUX_ABI_CKPT_SOURCE_H
#define HL_LINUX_ABI_CKPT_SOURCE_H

#include "ckpt_sink.h"

struct ckpt_source;

typedef struct ckpt_source_vtable {
    int64_t (*size)(struct ckpt_source *source, const char *name);
    int64_t (*read)(struct ckpt_source *source, const char *name, uint64_t offset, void *out, size_t size);
    // Writes NUL-terminated names into `out`; returns the count, or -1.
    int (*list)(struct ckpt_source *source, const char *prefix, char *out, size_t capacity);
    int (*digest)(struct ckpt_source *source, uint64_t *hash, uint64_t *files, uint64_t *bytes);
} ckpt_source_vtable;

struct ckpt_source {
    const ckpt_source_vtable *ops;
    char root[1024]; // directory source: the workspace. Empty for the streaming source.
};

static struct ckpt_source g_ckpt_source;

static struct ckpt_source *ckpt_source_current(void) {
    return g_ckpt_source.ops ? &g_ckpt_source : NULL;
}

// Image paths are built throughout the restore driver as "<workspace>/<object>". Both sources are addressed
// by OBJECT NAME, so a path is reduced by stripping the workspace prefix; for the streaming source the
// "workspace" is the sentinel, which is never a real path and therefore cannot collide with one.
// Returns NULL when `path` is not an image path at all -- the restore driver also touches guest paths, and
// those must keep going to the host filesystem untouched.
static const char *ckpt_source_relative(const char *path) {
    static char normalized[1400];
    const char *root = g_ckpt_source.root[0] ? g_ckpt_source.root : HL_CKPT_STREAM_SENTINEL;
    size_t length = strlen(root);
    if (strncmp(path, root, length) != 0 || path[length] != '/') return NULL;
    const char *name = path + length + 1;
    // A couple of call sites address a workspace-level object from inside a group as "proc.<gpid>/../<name>".
    // Collapse that here rather than teaching every implementation about "..".
    const char *up = strstr(name, "/../");
    if (up == NULL) return name;
    snprintf(normalized, sizeof normalized, "%s", up + 4);
    return normalized;
}

// ---------------------------------------------------------------- directory source

static int64_t ckpt_source_dir_size(struct ckpt_source *source, const char *name) {
    char path[1400];
    struct stat status;
    snprintf(path, sizeof path, "%s/%s", source->root, name);
    if (stat(path, &status) != 0 || !S_ISREG(status.st_mode)) return -1;
    return (int64_t)status.st_size;
}

static int64_t ckpt_source_dir_read(struct ckpt_source *source, const char *name, uint64_t offset, void *out,
                                    size_t size) {
    char path[1400];
    snprintf(path, sizeof path, "%s/%s", source->root, name);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    size_t done = 0;
    while (done < size) {
        ssize_t count = pread(fd, (char *)out + done, size - done, (off_t)(offset + done));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) {
            close(fd);
            return -1;
        }
        if (count == 0) break;
        done += (size_t)count;
    }
    close(fd);
    return (int64_t)done;
}

static int ckpt_source_dir_list(struct ckpt_source *source, const char *prefix, char *out, size_t capacity) {
    DIR *directory = opendir(source->root);
    struct dirent *entry;
    size_t used = 0;
    int count = 0;
    if (!directory) return -1;
    size_t length = strlen(prefix);
    while ((entry = readdir(directory)) != NULL) {
        if (strncmp(entry->d_name, prefix, length) != 0) continue;
        size_t size = strlen(entry->d_name) + 1;
        if (used + size > capacity) {
            closedir(directory);
            return -1;
        }
        memcpy(out + used, entry->d_name, size);
        used += size;
        count++;
    }
    closedir(directory);
    return count;
}

static int ckpt_source_dir_digest(struct ckpt_source *source, uint64_t *hash, uint64_t *files,
                                  uint64_t *bytes) {
    return ckpt_image_digest(source->root, hash, files, bytes);
}

static const ckpt_source_vtable g_ckpt_source_dir_ops = {
    .size = ckpt_source_dir_size,
    .read = ckpt_source_dir_read,
    .list = ckpt_source_dir_list,
    .digest = ckpt_source_dir_digest,
};

// ---------------------------------------------------------------- streaming source

static int64_t ckpt_source_stream_size(struct ckpt_source *source, const char *name) {
    hl_ckpt_reply reply;
    (void)source;
    int status = ckpt_stream_call(HL_CKPT_OP_SOURCE_SIZE, name, 0, 0, 0, NULL, 0, &reply, NULL, 0);
    if (status != HL_CKPT_STATUS_OK) return -1;
    return (int64_t)reply.value;
}

static int64_t ckpt_source_stream_read(struct ckpt_source *source, const char *name, uint64_t offset,
                                       void *out, size_t size) {
    size_t done = 0;
    (void)source;
    while (done < size) {
        hl_ckpt_reply reply;
        size_t chunk = size - done;
        if (chunk > HL_CKPT_STREAM_PAYLOAD_MAX) chunk = HL_CKPT_STREAM_PAYLOAD_MAX;
        if (ckpt_stream_call(HL_CKPT_OP_SOURCE_READ, name, 0, offset + done, 0, NULL, chunk, &reply,
                             (char *)out + done, chunk) != HL_CKPT_STATUS_OK)
            return -1;
        if (reply.length == 0) break; // end of object
        done += (size_t)reply.length;
    }
    return (int64_t)done;
}

static int ckpt_source_stream_list(struct ckpt_source *source, const char *prefix, char *out,
                                   size_t capacity) {
    hl_ckpt_reply reply;
    (void)source;
    if (ckpt_stream_call(HL_CKPT_OP_SOURCE_LIST, prefix, 0, 0, 0, NULL, 0, &reply, out, capacity) !=
        HL_CKPT_STATUS_OK)
        return -1;
    return (int)reply.value;
}

static int ckpt_source_stream_digest(struct ckpt_source *source, uint64_t *hash, uint64_t *files,
                                     uint64_t *bytes) {
    hl_ckpt_stream_digest digest = {0};
    hl_ckpt_reply reply;
    (void)source;
    if (ckpt_stream_call(HL_CKPT_OP_DIGEST, NULL, 0, 0, 0, NULL, 0, &reply, &digest, sizeof digest) !=
            HL_CKPT_STATUS_OK ||
        reply.length != sizeof digest)
        return -1;
    *hash = digest.hash;
    *files = digest.files;
    *bytes = digest.bytes;
    return 0;
}

static const ckpt_source_vtable g_ckpt_source_stream_ops = {
    .size = ckpt_source_stream_size,
    .read = ckpt_source_stream_read,
    .list = ckpt_source_stream_list,
    .digest = ckpt_source_stream_digest,
};

// ---------------------------------------------------------------- binding and the FILE* seam

static struct ckpt_source *ckpt_source_bind(const char *directory) {
    if (!strcmp(directory, HL_CKPT_STREAM_SENTINEL)) {
        if (hl_ckpt_channel_broker() < 0) return NULL;
        g_ckpt_source.ops = &g_ckpt_source_stream_ops;
        g_ckpt_source.root[0] = '\0';
        return &g_ckpt_source;
    }
    g_ckpt_source.ops = &g_ckpt_source_dir_ops;
    snprintf(g_ckpt_source.root, sizeof g_ckpt_source.root, "%s", directory);
    return &g_ckpt_source;
}

static int64_t ckpt_source_object_size(const char *path) {
    struct ckpt_source *source = ckpt_source_current();
    const char *name = source ? ckpt_source_relative(path) : NULL;
    return name ? source->ops->size(source, name) : -1;
}

// Whole-object load, replacing hl_host_file_load over an image path. `size` bytes exactly, or -1.
static int ckpt_source_load(const char *path, void *out, size_t size) {
    struct ckpt_source *source = ckpt_source_current();
    const char *name = source ? ckpt_source_relative(path) : NULL;
    if (!name) return -1;
    int64_t actual = source->ops->size(source, name);
    if (actual < 0 || (uint64_t)actual < size) return -1;
    return source->ops->read(source, name, 0, out, size) == (int64_t)size ? 0 : -1;
}

static int ckpt_source_list(const char *prefix, char *out, size_t capacity) {
    struct ckpt_source *source = ckpt_source_current();
    return source ? source->ops->list(source, prefix, out, capacity) : -1;
}

static int ckpt_source_digest(uint64_t *hash, uint64_t *files, uint64_t *bytes) {
    struct ckpt_source *source = ckpt_source_current();
    return source ? source->ops->digest(source, hash, files, bytes) : -1;
}

// The materialised objects handed out as FILE*. One entry per open image object; the restore driver never
// holds more than a handful at once.
#define CKPT_SOURCE_OPEN_MAX 64
static struct {
    FILE *file;
    void *bytes;
} g_ckpt_source_open[CKPT_SOURCE_OPEN_MAX];

// Open an image object for reading. Mirrors fopen(path, "rb") and is a plain fopen on the directory source.
static FILE *ckpt_source_fopen(const char *path) {
    struct ckpt_source *source = ckpt_source_current();
    if (!source) return NULL;
    const char *name = ckpt_source_relative(path);
    if (source->ops == &g_ckpt_source_dir_ops) {
        char full[1400];
        if (!name) return fopen(path, "rb"); // not an image object: a guest path, opened as it always was
        snprintf(full, sizeof full, "%s/%s", source->root, name);
        return fopen(full, "rb");
    }
    if (!name) return NULL;
    int64_t size = source->ops->size(source, name);
    if (size < 0) return NULL;
    void *bytes = malloc((size_t)size == 0 ? 1 : (size_t)size);
    if (!bytes) return NULL;
    if (size != 0 && source->ops->read(source, name, 0, bytes, (size_t)size) != size) {
        free(bytes);
        return NULL;
    }
    FILE *file = fmemopen(bytes, (size_t)size, "rb");
    if (!file) {
        free(bytes);
        return NULL;
    }
    for (int index = 0; index < CKPT_SOURCE_OPEN_MAX; ++index)
        if (g_ckpt_source_open[index].file == NULL) {
            g_ckpt_source_open[index].file = file;
            g_ckpt_source_open[index].bytes = bytes;
            return file;
        }
    fclose(file);
    free(bytes);
    return NULL;
}

// Close a handle from ckpt_source_fopen, releasing the materialised bytes if there were any.
static int ckpt_source_fclose(FILE *file) {
    if (!file) return 0;
    for (int index = 0; index < CKPT_SOURCE_OPEN_MAX; ++index)
        if (g_ckpt_source_open[index].file == file) {
            void *bytes = g_ckpt_source_open[index].bytes;
            g_ckpt_source_open[index].file = NULL;
            g_ckpt_source_open[index].bytes = NULL;
            int result = fclose(file);
            free(bytes);
            return result;
        }
    return fclose(file);
}

#endif
