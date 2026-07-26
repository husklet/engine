#define _GNU_SOURCE /* memfd_create for the shared trigger word */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700 /* nftw() is XSI; needed for the scratch-tree cleanup below */

#include <ftw.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { TIMEOUT_SECONDS = 15 };

/* ---------------------------------------------------------------- checkpoint store server
 *
 * The engine no longer writes a workspace: every image byte crosses a UNIX socket
 * (include/hl/checkpoint_stream.h). This runner therefore holds the store. It is the C twin of
 * pkgs/rust/src/checkpoint_stream.rs -- same protocol, same staging rules, same digest -- kept in memory so
 * the corruption cases can damage a committed image the way they used to damage files.
 *
 * There is no server thread: the runner's waiting loops call store_pump() instead of sleeping, which polls
 * the broker plus every open channel and services whatever is ready. An engine process therefore never waits
 * longer than one poll for its reply, and the runner never needs -pthread.
 */

#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>

#include "../../include/hl/checkpoint_stream.h"

#define STORE_OBJECT_MAX 4096
#define STORE_CHANNEL_MAX 64
#define STORE_GROUP_MAX 512
#define STORE_OPEN_MAX 256

struct store_object {
    char name[HL_CKPT_STREAM_NAME_MAX];
    unsigned char *bytes;
    size_t size;
    /* Set once the object has been handed to the store, so the digest never counts it twice. */
    uint64_t hash;
    int hashed;
};

struct store_open {
    int channel;
    uint64_t stream;
    struct store_object object;
    int used;
};

static struct store_object g_objects[STORE_OBJECT_MAX];
static int g_object_count;
static struct store_open g_open[STORE_OPEN_MAX];
/* Objects finished inside a group, invisible until the group commits. */
static struct store_object g_staged[STORE_OBJECT_MAX];
static char g_staged_group[STORE_OBJECT_MAX][HL_CKPT_STREAM_NAME_MAX];
static int g_staged_count;
static char g_open_groups[STORE_GROUP_MAX][HL_CKPT_STREAM_NAME_MAX];
static int g_open_group_count;
static char g_committed_groups[STORE_GROUP_MAX][HL_CKPT_STREAM_NAME_MAX];
static int g_committed_group_count;
static char g_claims[STORE_GROUP_MAX][HL_CKPT_STREAM_NAME_MAX];
static int g_claim_count;
static int g_broker = -1;
static int g_channels[STORE_CHANNEL_MAX];
static int g_channel_count;
static int g_store_committed;

#define STORE_HASH_BASIS UINT64_C(14695981039346656037)
#define STORE_HASH_PRIME UINT64_C(1099511628211)

static uint64_t store_hash_bytes(uint64_t hash, const void *data, size_t size) {
    const unsigned char *bytes = data;
    for (size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= STORE_HASH_PRIME;
    }
    return hash;
}

static uint64_t store_object_hash(const char *name, const unsigned char *data, size_t size) {
    uint64_t value = store_hash_bytes(STORE_HASH_BASIS, name, strlen(name) + 1);
    uint64_t length = (uint64_t)size;
    value = store_hash_bytes(value, &length, sizeof length);
    return store_hash_bytes(value, data, size);
}

/* The manifest cannot authenticate itself, and the recovery journal is written after the image is complete. */
static int store_digested(const char *name) {
    return strcmp(name, "MANIFEST") != 0 && strcmp(name, "RECOVERY.jsonl") != 0 &&
           strncmp(name, ".RECOVERY.jsonl.tmp.", 20) != 0;
}

static int store_name_compare(const void *left, const void *right) {
    return strcmp(*(const char *const *)left, *(const char *const *)right);
}

static void store_digest(uint64_t *hash, uint64_t *files, uint64_t *bytes) {
    const char *names[STORE_OBJECT_MAX];
    int count = 0;
    for (int index = 0; index < g_object_count; ++index)
        if (store_digested(g_objects[index].name)) names[count++] = g_objects[index].name;
    qsort(names, (size_t)count, sizeof *names, store_name_compare);
    *hash = STORE_HASH_BASIS;
    *files = (uint64_t)count;
    *bytes = 0;
    for (int index = 0; index < count; ++index)
        for (int object = 0; object < g_object_count; ++object)
            if (g_objects[object].name == names[index]) {
                uint64_t folded = g_objects[object].hash;
                *hash = store_hash_bytes(*hash, names[index], strlen(names[index]) + 1);
                *hash = store_hash_bytes(*hash, &folded, sizeof folded);
                *bytes += (uint64_t)g_objects[object].size;
                break;
            }
}

static struct store_object *store_find(const char *name) {
    for (int index = 0; index < g_object_count; ++index)
        if (strcmp(g_objects[index].name, name) == 0) return &g_objects[index];
    return NULL;
}

/* Hand a finished object to the store, replacing any earlier object of the same name. */
static int store_publish(struct store_object *object) {
    struct store_object *existing = store_find(object->name);
    if (existing != NULL) {
        free(existing->bytes);
    } else {
        if (g_object_count == STORE_OBJECT_MAX) return -1;
        existing = &g_objects[g_object_count++];
    }
    *existing = *object;
    existing->hash = store_object_hash(existing->name, existing->bytes, existing->size);
    existing->hashed = 1;
    if (strcmp(existing->name, "MANIFEST") == 0) g_store_committed = 1;
    return 0;
}

static int store_group_of(const char *name, char *out, size_t capacity) {
    const char *slash = strchr(name, '/');
    size_t length;
    if (slash == NULL) return 0;
    length = (size_t)(slash - name);
    if (length + 1 > capacity) return 0;
    memcpy(out, name, length);
    out[length] = 0;
    return 1;
}

static int store_group_is_open(const char *group) {
    for (int index = 0; index < g_open_group_count; ++index)
        if (strcmp(g_open_groups[index], group) == 0) return 1;
    return 0;
}

static void store_group_close(const char *group) {
    for (int index = 0; index < g_open_group_count; ++index)
        if (strcmp(g_open_groups[index], group) == 0) {
            g_open_groups[index][0] = 0;
            memmove(&g_open_groups[index], &g_open_groups[index + 1],
                    (size_t)(g_open_group_count - index - 1) * sizeof g_open_groups[0]);
            g_open_group_count--;
            return;
        }
}

static void store_drop_staged(const char *group) {
    int kept = 0;
    for (int index = 0; index < g_staged_count; ++index) {
        if (strcmp(g_staged_group[index], group) == 0) {
            free(g_staged[index].bytes);
            continue;
        }
        g_staged[kept] = g_staged[index];
        memcpy(g_staged_group[kept], g_staged_group[index], sizeof g_staged_group[0]);
        kept++;
    }
    g_staged_count = kept;
}

static struct store_open *store_open_find(int channel, uint64_t stream) {
    for (int index = 0; index < STORE_OPEN_MAX; ++index)
        if (g_open[index].used && g_open[index].channel == channel && g_open[index].stream == stream)
            return &g_open[index];
    return NULL;
}

static int store_object_write(struct store_object *object, uint64_t offset, const unsigned char *data,
                              size_t size) {
    size_t end = (size_t)offset + size;
    if (end > object->size) {
        unsigned char *grown = realloc(object->bytes, end == 0 ? 1 : end);
        if (grown == NULL) return -1;
        memset(grown + object->size, 0, end - object->size);
        object->bytes = grown;
        object->size = end;
    }
    if (size != 0) memcpy(object->bytes + offset, data, size);
    return 0;
}

static int store_read_all(int descriptor, void *data, size_t size) {
    char *bytes = data;
    size_t done = 0;
    while (done < size) {
        ssize_t count = read(descriptor, bytes + done, size - done);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return -1;
        done += (size_t)count;
    }
    return 0;
}

static int store_write_all(int descriptor, const void *data, size_t size) {
    const char *bytes = data;
    size_t done = 0;
    while (done < size) {
        ssize_t count = write(descriptor, bytes + done, size - done);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return -1;
        done += (size_t)count;
    }
    return 0;
}

/* Service exactly one request on `channel`. Returns 0 to keep the channel, -1 when it has closed. */
static int store_serve(int channel) {
    hl_ckpt_request request;
    hl_ckpt_reply reply;
    char name[HL_CKPT_STREAM_NAME_MAX];
    unsigned char *payload = NULL;
    unsigned char *out = NULL;
    size_t out_size = 0;
    int result = 0;

    if (store_read_all(channel, &request, sizeof request) != 0) return -1;
    if (request.magic != HL_CKPT_STREAM_MAGIC_REQUEST || request.abi != HL_CKPT_STREAM_ABI ||
        request.name_size > sizeof name || request.length > HL_CKPT_STREAM_PAYLOAD_MAX)
        return -1;
    name[0] = 0;
    if (request.name_size != 0 && store_read_all(channel, name, request.name_size) != 0) return -1;
    if (request.length != 0 && request.op != HL_CKPT_OP_SOURCE_READ) {
        payload = malloc((size_t)request.length);
        if (payload == NULL || store_read_all(channel, payload, (size_t)request.length) != 0) {
            free(payload);
            return -1;
        }
    }

    memset(&reply, 0, sizeof reply);
    reply.magic = HL_CKPT_STREAM_MAGIC_REPLY;
    reply.abi = HL_CKPT_STREAM_ABI;
    reply.status = HL_CKPT_STATUS_OK;

    switch (request.op) {
        case HL_CKPT_OP_OBJECT_BEGIN: {
            struct store_open *slot = NULL;
            for (int index = 0; index < STORE_OPEN_MAX; ++index)
                if (!g_open[index].used) {
                    slot = &g_open[index];
                    break;
                }
            if (slot == NULL) {
                reply.status = HL_CKPT_STATUS_ERROR;
                break;
            }
            memset(slot, 0, sizeof *slot);
            slot->used = 1;
            slot->channel = channel;
            slot->stream = request.stream;
            snprintf(slot->object.name, sizeof slot->object.name, "%s", name);
            break;
        }
        case HL_CKPT_OP_OBJECT_WRITE:
        case HL_CKPT_OP_OBJECT_WRITE_AT: {
            struct store_open *slot = store_open_find(channel, request.stream);
            uint64_t offset = request.op == HL_CKPT_OP_OBJECT_WRITE ? (uint64_t)0 : request.offset;
            if (request.op == HL_CKPT_OP_OBJECT_WRITE && slot != NULL) offset = slot->object.size;
            if (slot == NULL || store_object_write(&slot->object, offset, payload, (size_t)request.length) != 0)
                reply.status = HL_CKPT_STATUS_ERROR;
            break;
        }
        case HL_CKPT_OP_OBJECT_TELL: {
            struct store_open *slot = store_open_find(channel, request.stream);
            if (slot == NULL)
                reply.status = HL_CKPT_STATUS_ERROR;
            else
                reply.value = (uint64_t)slot->object.size;
            break;
        }
        case HL_CKPT_OP_OBJECT_FINISH: {
            struct store_open *slot = store_open_find(channel, request.stream);
            char group[HL_CKPT_STREAM_NAME_MAX];
            if (slot == NULL) {
                reply.status = HL_CKPT_STATUS_ERROR;
                break;
            }
            slot->used = 0;
            if (store_group_of(slot->object.name, group, sizeof group) && store_group_is_open(group)) {
                if (g_staged_count == STORE_OBJECT_MAX) {
                    free(slot->object.bytes);
                    reply.status = HL_CKPT_STATUS_ERROR;
                    break;
                }
                g_staged[g_staged_count] = slot->object;
                snprintf(g_staged_group[g_staged_count], sizeof g_staged_group[0], "%s", group);
                g_staged_count++;
                break;
            }
            if (store_publish(&slot->object) != 0) reply.status = HL_CKPT_STATUS_ERROR;
            break;
        }
        case HL_CKPT_OP_OBJECT_ABORT: {
            struct store_open *slot = store_open_find(channel, request.stream);
            if (slot != NULL) {
                free(slot->object.bytes);
                slot->used = 0;
            }
            break;
        }
        case HL_CKPT_OP_GROUP_BEGIN:
            store_drop_staged(name);
            if (!store_group_is_open(name) && g_open_group_count < STORE_GROUP_MAX)
                snprintf(g_open_groups[g_open_group_count++], sizeof g_open_groups[0], "%s", name);
            break;
        case HL_CKPT_OP_GROUP_COMMIT: {
            store_group_close(name);
            for (int index = 0; index < g_staged_count; ++index)
                if (strcmp(g_staged_group[index], name) == 0 && store_publish(&g_staged[index]) != 0)
                    reply.status = HL_CKPT_STATUS_ERROR;
            for (int index = 0; index < g_staged_count; ++index)
                if (strcmp(g_staged_group[index], name) == 0) g_staged_group[index][0] = 0;
            {
                int kept = 0;
                for (int index = 0; index < g_staged_count; ++index)
                    if (g_staged_group[index][0] != 0) {
                        g_staged[kept] = g_staged[index];
                        memcpy(g_staged_group[kept], g_staged_group[index], sizeof g_staged_group[0]);
                        kept++;
                    }
                g_staged_count = kept;
            }
            if (reply.status == HL_CKPT_STATUS_OK && g_committed_group_count < STORE_GROUP_MAX)
                snprintf(g_committed_groups[g_committed_group_count++], sizeof g_committed_groups[0], "%s", name);
            break;
        }
        case HL_CKPT_OP_GROUP_ABORT:
            store_group_close(name);
            store_drop_staged(name);
            break;
        case HL_CKPT_OP_CLAIM: {
            for (int index = 0; index < g_claim_count; ++index)
                if (strcmp(g_claims[index], name) == 0) {
                    reply.status = HL_CKPT_STATUS_ALREADY;
                    break;
                }
            if (reply.status == HL_CKPT_STATUS_OK && g_claim_count < STORE_GROUP_MAX)
                snprintf(g_claims[g_claim_count++], sizeof g_claims[0], "%s", name);
            break;
        }
        case HL_CKPT_OP_UNCLAIM:
            for (int index = 0; index < g_claim_count; ++index)
                if (strcmp(g_claims[index], name) == 0) {
                    memmove(&g_claims[index], &g_claims[index + 1],
                            (size_t)(g_claim_count - index - 1) * sizeof g_claims[0]);
                    g_claim_count--;
                    break;
                }
            break;
        case HL_CKPT_OP_GROUP_PRESENT:
            for (int index = 0; index < g_committed_group_count; ++index)
                if (strcmp(g_committed_groups[index], name) == 0) reply.value = 1;
            break;
        case HL_CKPT_OP_GROUP_COUNT:
            for (int index = 0; index < g_committed_group_count; ++index)
                if (strncmp(g_committed_groups[index], name, strlen(name)) == 0) reply.value++;
            break;
        case HL_CKPT_OP_DIGEST: {
            hl_ckpt_stream_digest digest;
            store_digest(&digest.hash, &digest.files, &digest.bytes);
            out = malloc(sizeof digest);
            if (out == NULL) {
                reply.status = HL_CKPT_STATUS_ERROR;
                break;
            }
            memcpy(out, &digest, sizeof digest);
            out_size = sizeof digest;
            break;
        }
        case HL_CKPT_OP_COMMIT: {
            struct store_object manifest;
            memset(&manifest, 0, sizeof manifest);
            snprintf(manifest.name, sizeof manifest.name, "MANIFEST");
            if (store_object_write(&manifest, 0, payload, (size_t)request.length) != 0 ||
                store_publish(&manifest) != 0)
                reply.status = HL_CKPT_STATUS_ERROR;
            break;
        }
        case HL_CKPT_OP_SOURCE_LIST: {
            char seen[STORE_GROUP_MAX][HL_CKPT_STREAM_NAME_MAX];
            int count = 0;
            out = malloc(64 * 1024);
            if (out == NULL) {
                reply.status = HL_CKPT_STATUS_ERROR;
                break;
            }
            for (int index = 0; index < g_object_count; ++index) {
                char entry[HL_CKPT_STREAM_NAME_MAX];
                int duplicate = 0;
                if (!store_group_of(g_objects[index].name, entry, sizeof entry))
                    memcpy(entry, g_objects[index].name, sizeof entry);
                if (strncmp(entry, name, strlen(name)) != 0) continue;
                for (int held = 0; held < count; ++held)
                    if (strcmp(seen[held], entry) == 0) duplicate = 1;
                if (duplicate || count == STORE_GROUP_MAX) continue;
                memcpy(seen[count++], entry, sizeof entry);
            }
            for (int index = 0; index < count; ++index) {
                size_t length = strlen(seen[index]) + 1;
                if (out_size + length > 64 * 1024) break;
                memcpy(out + out_size, seen[index], length);
                out_size += length;
            }
            reply.value = (uint64_t)count;
            break;
        }
        case HL_CKPT_OP_SOURCE_SIZE: {
            struct store_object *object = store_find(name);
            if (object == NULL)
                reply.status = HL_CKPT_STATUS_ALREADY; /* absent is not a failure */
            else
                reply.value = (uint64_t)object->size;
            break;
        }
        case HL_CKPT_OP_SOURCE_READ: {
            struct store_object *object = store_find(name);
            size_t offset = (size_t)request.offset;
            size_t length;
            if (object == NULL) {
                reply.status = HL_CKPT_STATUS_ERROR;
                break;
            }
            if (offset >= object->size) break;
            length = object->size - offset;
            if (length > (size_t)request.length) length = (size_t)request.length;
            out = malloc(length == 0 ? 1 : length);
            if (out == NULL) {
                reply.status = HL_CKPT_STATUS_ERROR;
                break;
            }
            memcpy(out, object->bytes + offset, length);
            out_size = length;
            break;
        }
        default:
            reply.status = HL_CKPT_STATUS_ERROR;
            break;
    }

    reply.length = (uint64_t)out_size;
    if (store_write_all(channel, &reply, sizeof reply) != 0 ||
        (out_size != 0 && store_write_all(channel, out, out_size) != 0))
        result = -1;
    free(payload);
    free(out);
    return result;
}

/* Accept one engine process announcing itself on the broker. */
static void store_accept(void) {
    hl_ckpt_hello hello;
    struct iovec vector = {.iov_base = &hello, .iov_len = sizeof hello};
    char control[CMSG_SPACE(sizeof(int))];
    struct msghdr message = {0};
    struct cmsghdr *header;
    int channel = -1;
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof control;
    if (recvmsg(g_broker, &message, 0) != (ssize_t)sizeof hello) return;
    header = CMSG_FIRSTHDR(&message);
    if (header == NULL || header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS) return;
    memcpy(&channel, CMSG_DATA(header), sizeof channel);
    if (hello.magic != HL_CKPT_STREAM_MAGIC_HELLO || hello.abi != HL_CKPT_STREAM_ABI ||
        g_channel_count == STORE_CHANNEL_MAX) {
        if (channel >= 0) close(channel);
        return;
    }
    g_channels[g_channel_count++] = channel;
}

/* One turn of the store's event loop: wait up to `timeout_ms` and service everything ready. Replaces the
 * sleep in every wait loop, so an engine process is never blocked on a reply for longer than one poll. */
static void store_pump(int timeout_ms) {
    struct pollfd waiting[STORE_CHANNEL_MAX + 1];
    int count = 0;
    int ready;
    if (g_broker >= 0) {
        waiting[count].fd = g_broker;
        waiting[count].events = POLLIN;
        waiting[count].revents = 0;
        count++;
    }
    for (int index = 0; index < g_channel_count; ++index) {
        waiting[count].fd = g_channels[index];
        waiting[count].events = POLLIN;
        waiting[count].revents = 0;
        count++;
    }
    if (count == 0) {
        struct timespec tick = {0, (long)timeout_ms * 1000000L};
        nanosleep(&tick, NULL);
        return;
    }
    do {
        ready = poll(waiting, (nfds_t)count, timeout_ms);
    } while (ready < 0 && errno == EINTR);
    if (ready <= 0) return;
    for (int index = 0; index < count; ++index) {
        if (waiting[index].revents == 0) continue;
        if (waiting[index].fd == g_broker) {
            store_accept();
            continue;
        }
        if (store_serve(waiting[index].fd) != 0) {
            close(waiting[index].fd);
            for (int slot = 0; slot < g_channel_count; ++slot)
                if (g_channels[slot] == waiting[index].fd) {
                    memmove(&g_channels[slot], &g_channels[slot + 1],
                            (size_t)(g_channel_count - slot - 1) * sizeof g_channels[0]);
                    g_channel_count--;
                    break;
                }
        }
    }
}

/* Drop every channel between launches: the next engine tree announces fresh ones. */
static void store_reset_channels(void) {
    while (g_channel_count > 0) close(g_channels[--g_channel_count]);
    for (int index = 0; index < STORE_OPEN_MAX; ++index)
        if (g_open[index].used) {
            free(g_open[index].object.bytes);
            g_open[index].used = 0;
        }
    for (int index = 0; index < g_staged_count; ++index) free(g_staged[index].bytes);
    g_staged_count = 0;
    g_open_group_count = 0;
    g_claim_count = 0;
}

/* --- corruption injection -------------------------------------------------
 * The corrupt-image cases used to edit files in the workspace. The image lives in this process now, so they
 * edit it here instead -- same five damage shapes, applied after COMMIT, and restore must still refuse. */

static int store_damage(const char *name, unsigned char mask) {
    struct store_object *object = store_find(name);
    if (object == NULL || object->size == 0) return -1;
    object->bytes[0] ^= mask;
    object->hash = store_object_hash(object->name, object->bytes, object->size);
    return 0;
}

static int store_truncate(const char *name) {
    struct store_object *object = store_find(name);
    if (object == NULL || object->size < 1) return -1;
    object->size--;
    object->hash = store_object_hash(object->name, object->bytes, object->size);
    return 0;
}

static int store_remove(const char *name) {
    for (int index = 0; index < g_object_count; ++index)
        if (strcmp(g_objects[index].name, name) == 0) {
            free(g_objects[index].bytes);
            memmove(&g_objects[index], &g_objects[index + 1],
                    (size_t)(g_object_count - index - 1) * sizeof g_objects[0]);
            g_object_count--;
            return 0;
        }
    return -1;
}

static int store_add(const char *name, const char *data, size_t size) {
    struct store_object object;
    memset(&object, 0, sizeof object);
    snprintf(object.name, sizeof object.name, "%s", name);
    if (store_object_write(&object, 0, (const unsigned char *)data, size) != 0) return -1;
    return store_publish(&object);
}

/* Does a store object contain `needle`? The recovery journal is an image object now, not a file. */
static int store_has(const char *name, const char *needle) {
    struct store_object *object = store_find(name);
    char *copy;
    int found;
    if (object == NULL) return 0;
    copy = malloc(object->size + 1);
    if (copy == NULL) return 0;
    memcpy(copy, object->bytes, object->size);
    copy[object->size] = 0;
    found = strstr(copy, needle) != NULL;
    free(copy);
    return found;
}

static int output_has(const char *path, const char *needle) {
    char data[16384];
    int fd = open(path, O_RDONLY);
    ssize_t size;
    if (fd < 0) return 0;
    size = read(fd, data, sizeof(data) - 1);
    close(fd);
    if (size < 0) return 0;
    data[size] = 0;
    return strstr(data, needle) != NULL;
}

static int output_count(const char *path, const char *needle) {
    char data[16384];
    int fd = open(path, O_RDONLY), count = 0;
    ssize_t size;
    if (fd < 0) return 0;
    size = read(fd, data, sizeof(data) - 1);
    close(fd);
    if (size < 0) return 0;
    data[size] = 0;
    for (char *cursor = data; (cursor = strstr(cursor, needle)) != NULL; cursor += strlen(needle)) count++;
    return count;
}

/* Capture completion is the explicit COMMIT, not a file appearing. */
static int wait_for_commit(time_t deadline) {
    while (time(NULL) < deadline) {
        if (g_store_committed) return 0;
        store_pump(10);
    }
    return -1;
}

static int wait_for_output(const char *path, const char *needle, time_t deadline) {
    while (time(NULL) < deadline) {
        if (output_has(path, needle)) return 0;
        store_pump(10);
    }
    return -1;
}

static int wait_for_output_count(const char *path, const char *needle, int count, time_t deadline) {
    while (time(NULL) < deadline) {
        if (output_count(path, needle) >= count) return 0;
        store_pump(10);
    }
    return -1;
}

static int wait_for_ready(const char *path, int processes, time_t deadline) {
    while (time(NULL) < deadline) {
        if (output_has(path, "READY 1") && (processes == 1 || output_has(path, "READY 2")) &&
            (processes < 3 || output_has(path, "READY 3"))) return 0;
        store_pump(10);
    }
    return -1;
}

static int wait_for_restored(const char *path, int pipe_case, int deleted_case, int threads_case, int memfd_case,
                             int eventfd_case, int timerfd_case, int inotify_case, time_t deadline) {
    while (time(NULL) < deadline) {
        if (inotify_case == 5) {
            if (output_has(path, "CONNECTED-SOCKET-RESTORED")) return 0;
        } else if (inotify_case == 6) {
            if (output_has(path, "SIGNAL-RESTORED")) return 0;
        } else if (inotify_case == 4) {
            if (output_has(path, "SOCKET-STATE-RESTORED")) return 0;
        } else if (inotify_case == 3) {
            if (output_has(path, "SOCKETPAIR-RESTORED")) return 0;
        } else if (inotify_case == 2) {
            if (output_has(path, "EPOLL-RESTORED")) return 0;
        } else if (inotify_case) {
            if (output_has(path, "INOTIFY-RESTORED")) return 0;
        } else if (timerfd_case) {
            if (output_has(path, "TIMERFD-RESTORED")) return 0;
        } else if (eventfd_case) {
            if (output_has(path, "EVENTFD-RESTORED")) return 0;
        } else if (memfd_case) {
            if (output_has(path, "MEMFD-RESTORED")) return 0;
        } else if (threads_case) {
            if (output_has(path, "THREADS-RESTORED")) return 0;
        } else if (deleted_case) {
            if (output_has(path, "DELETED-RESTORED")) return 0;
        } else if (pipe_case) {
            if (output_has(path, "PIPE-RESTORED")) return 0;
        } else if (output_has(path, "RESTORED 1 ") && output_has(path, "RESTORED 2 ") &&
                   output_has(path, "RESTORED 3 ") && output_has(path, "TREE-RESTORED ")) {
            return 0;
        }
        store_pump(10);
    }
    return -1;
}

static int wait_child(pid_t pid, time_t deadline) {
    int status;
    while (time(NULL) < deadline) {
        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid) return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
        if (result < 0 && errno != EINTR) return 125;
        store_pump(10);
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return 124;
}

/* The engine's end of the store channel: the broker socket it announces itself on, and the shared
 * generation word a capture request bumps. Both are inherited across exec and named by number. */
static int g_broker_child = -1;
static int g_trigger_descriptor = -1;
static volatile uint32_t *g_trigger;

/* `policy` is the --restore-policy argument, or NULL to leave the policy unset (the permissive default). */
static pid_t launch(const char *engine, const char *guest, const char *release,
                    const char *output, int restore, const char *policy,
                    const char *guest_mode) {
    char broker[16], trigger[16];
    pid_t pid;
    snprintf(broker, sizeof broker, "%d", g_broker_child);
    snprintf(trigger, sizeof trigger, "%d", g_trigger_descriptor);
    pid = fork();
    if (pid != 0) return pid;
    if (!restore && setsid() < 0) _exit(126);
    {
        int fd = open(output, O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (fd < 0 || dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0) _exit(126);
        if (fd != STDOUT_FILENO) close(fd);
    }
    /* The engine inherits these two; everything else the runner holds stays close-on-exec. */
    if (fcntl(g_broker_child, F_SETFD, 0) != 0 || fcntl(g_trigger_descriptor, F_SETFD, 0) != 0) _exit(126);
    if (restore && policy)
        execl(engine, engine, "--restore-policy", policy, "--restore", "--checkpoint-store", broker, trigger,
              (char *)NULL);
    else if (restore)
        execl(engine, engine, "--restore", "--checkpoint-store", broker, trigger, (char *)NULL);
    else if (policy && guest_mode)
        execl(engine, engine, "--restore-policy", policy, "--checkpoint", "--checkpoint-store", broker,
              trigger, guest, release, guest_mode, (char *)NULL);
    else if (policy)
        execl(engine, engine, "--restore-policy", policy, "--checkpoint", "--checkpoint-store", broker,
              trigger, guest, release, (char *)NULL);
    else if (guest_mode)
        execl(engine, engine, "--checkpoint", "--checkpoint-store", broker, trigger, guest, release,
              guest_mode, (char *)NULL);
    else
        execl(engine, engine, "--checkpoint", "--checkpoint-store", broker, trigger, guest, release,
              (char *)NULL);
    _exit(127);
}

/* Create the store channel and the shared trigger word. Both live for the whole runner, across the capture
 * launch and the restore launch that reads the image back. */
static int store_channel_open(void) {
    int pair[2];
    void *mapping;
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) != 0) return -1;
    if (fcntl(pair[0], F_SETFD, FD_CLOEXEC) != 0 || fcntl(pair[1], F_SETFD, FD_CLOEXEC) != 0) return -1;
    g_broker = pair[0];
    g_broker_child = pair[1];
#if defined(__linux__)
    g_trigger_descriptor = memfd_create("hl-checkpoint-trigger", MFD_CLOEXEC);
#else
    {
        char name[64];
        snprintf(name, sizeof name, "/hl-ckpt-runner-%d", (int)getpid());
        g_trigger_descriptor = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (g_trigger_descriptor >= 0) shm_unlink(name);
    }
#endif
    if (g_trigger_descriptor < 0 || ftruncate(g_trigger_descriptor, 4) != 0) return -1;
    mapping = mmap(NULL, 4, PROT_READ | PROT_WRITE, MAP_SHARED, g_trigger_descriptor, 0);
    if (mapping == MAP_FAILED) return -1;
    g_trigger = mapping;
    return 0;
}

/* --- scratch-tree cleanup (see the atexit registration in main) ----------- */
static char g_scratch_root[4096];

static int scratch_remove_entry(const char *path, const struct stat *info, int flag, struct FTW *ftw) {
    (void)info;
    (void)flag;
    (void)ftw;
    remove(path); /* best effort: FTW_DEPTH gives us children before parents */
    return 0;
}

static void scratch_cleanup(void) {
    if (g_scratch_root[0] == '\0') return;
    (void)nftw(g_scratch_root, scratch_remove_entry, 16, FTW_DEPTH | FTW_PHYS);
    g_scratch_root[0] = '\0';
}

int main(int argc, char **argv) {
    char temporary[1024];
    char output[512], release[512], release_error[520];
    pid_t child;
    int fd, result;
    int pipe_case = argc == 4 && strcmp(argv[3], "pipe") == 0;
    int deleted_case = argc == 4 && strcmp(argv[3], "deleted") == 0;
    int threads_case = argc == 4 && strcmp(argv[3], "threads") == 0;
    int memfd_case = argc == 4 && strcmp(argv[3], "memfd") == 0;
    int eventfd_case = argc == 4 && strcmp(argv[3], "eventfd") == 0;
    int timerfd_case = argc == 4 && strcmp(argv[3], "timerfd") == 0;
    int inotify_case = argc == 4 && strcmp(argv[3], "inotify") == 0;
    int epoll_case = argc == 4 && strcmp(argv[3], "epoll-checkpoint") == 0;
    int socketpair_case = argc == 4 && strcmp(argv[3], "socketpair") == 0;
    int socket_state_case = argc == 4 && strcmp(argv[3], "socket-state") == 0;
    int connected_socket_case = argc == 4 && strcmp(argv[3], "connected-socket") == 0;
    int signal_case = argc == 4 && strcmp(argv[3], "signal-state") == 0;
    int connecting_refusal_case = argc == 4 && strcmp(argv[3], "connecting-refusal") == 0;
    int connecting_fallback_case = argc == 4 && strcmp(argv[3], "connecting-fallback") == 0;
    int corrupt_magic_case = argc == 4 && strcmp(argv[3], "corrupt-magic") == 0;
    int corrupt_truncated_case = argc == 4 && strcmp(argv[3], "corrupt-truncated") == 0;
    int corrupt_content_case = argc == 4 && strcmp(argv[3], "corrupt-content") == 0;
    int corrupt_missing_case = argc == 4 && strcmp(argv[3], "corrupt-missing") == 0;
    int corrupt_extra_case = argc == 4 && strcmp(argv[3], "corrupt-extra") == 0;
    int permissive_case = argc == 4 &&
                          (strcmp(argv[3], "missing-external") == 0 || connecting_fallback_case);
    int modified_external_case = argc == 4 && strcmp(argv[3], "modified-external") == 0;
    int io_case = argc == 4 && (!strcmp(argv[3], "io-replace") || !strcmp(argv[3], "io-recreate") ||
                                !strcmp(argv[3], "io-directory") || !strcmp(argv[3], "io-duplicate") ||
                                !strcmp(argv[3], "io-device") || !strcmp(argv[3], "io-type-change") ||
                                !strcmp(argv[3], "io-permission") || !strcmp(argv[3], "io-missing-root") ||
                                !strcmp(argv[3], "io-append") || !strcmp(argv[3], "io-shortened") ||
                                !strcmp(argv[3], "io-repeat") || !strcmp(argv[3], "io-directory-change") ||
                                !strcmp(argv[3], "io-missing-child-strict") ||
                                !strcmp(argv[3], "io-missing-child-default") ||
                                !strcmp(argv[3], "io-fifo-refusal") ||
                                !strcmp(argv[3], "io-queued-device") || !strcmp(argv[3], "io-queued-missing"));
    int backward_v2_case = argc == 5 && !strcmp(argv[3], "backward-v2");
    const char *capture_engine = backward_v2_case ? argv[4] : argv[1];
    const char *guest_mode = io_case ? argv[3] + 3 : NULL;
    int io_capture_refusal = io_case && !strcmp(guest_mode, "fifo-refusal");
    int io_strict_restore = io_case && !strcmp(guest_mode, "missing-child-strict");
    /* Same image as the strict case, restored with no policy at all: the default must prune, not refuse. */
    int io_default_restore = io_case && !strcmp(guest_mode, "missing-child-default");
    if (io_case && !io_capture_refusal && !io_strict_restore && !io_default_restore) permissive_case = 1;
    if ((argc != 3 && !backward_v2_case && !pipe_case && !deleted_case && !threads_case && !memfd_case && !eventfd_case &&
         !timerfd_case && !inotify_case && !epoll_case && !socketpair_case && !socket_state_case &&
         !connected_socket_case && !signal_case && !connecting_refusal_case && !connecting_fallback_case && !corrupt_magic_case &&
         !corrupt_truncated_case && !corrupt_content_case && !corrupt_missing_case && !corrupt_extra_case &&
         !permissive_case && !modified_external_case &&
         !io_case) ||
        getcwd(temporary, sizeof temporary) == NULL ||
        strlen(temporary) + sizeof("/build/hl-checkpoint-tree.XXXXXX") > sizeof temporary)
        return 2;
    strcat(temporary, "/build/hl-checkpoint-tree.XXXXXX");
    if (mkdtemp(temporary) == NULL) return 2;
    /* The scratch tree is created here but the runner has ~87 return paths, so
     * cleaning up at each one is unmaintainable -- and skipping it leaked a
     * build/hl-checkpoint-tree.XXXXXX directory per invocation (hundreds had
     * accumulated). Register one atexit hook that removes the whole tree. */
    snprintf(g_scratch_root, sizeof g_scratch_root, "%s", temporary);
    atexit(scratch_cleanup);
    if (getenv("HL_KEEP_CHECKPOINT_FIXTURE")) g_scratch_root[0] = '\0';
    snprintf(output, sizeof output, "%s/release.output", temporary);
    snprintf(release, sizeof release, "%s/release", temporary);
    snprintf(release_error, sizeof release_error, "%s.error", release);

    if (store_channel_open() != 0) return 2;
    child = launch(capture_engine, argv[2], release, output, 0,
                   permissive_case ? "discard-optional" : NULL, guest_mode);
    if (child < 0) return 3;
    if (wait_for_ready(output, (deleted_case || threads_case || memfd_case || inotify_case || epoll_case ||
                                signal_case || connecting_refusal_case || connecting_fallback_case || modified_external_case ||
                                (io_case && strcmp(guest_mode, "type-change") && strcmp(guest_mode, "permission") &&
                                 strcmp(guest_mode, "directory-change") && strcmp(guest_mode, "missing-child-strict") &&
                                 strcmp(guest_mode, "missing-child-default"))) ? 1 :
                                   (socketpair_case || connected_socket_case) ? 2 : socket_state_case ? 1 :
                                   (pipe_case || eventfd_case || timerfd_case || permissive_case || io_strict_restore ||
                                    io_default_restore) ? 2 : 3,
                       time(NULL) + TIMEOUT_SECONDS) != 0) {
        fprintf(stderr, "checkpoint runner: readiness timeout one=%d two=%d three=%d\n",
                output_has(output, "READY 1"), output_has(output, "READY 2"), output_has(output, "READY 3"));
        kill(child, SIGKILL);
        waitpid(child, NULL, 0);
        return 3;
    }
    *g_trigger = *g_trigger + 1u;
#ifdef SIGINFO
    if (kill(child, SIGINFO) != 0) return 5;
#else
    if (kill(child, SIGURG) != 0) return 5;
#endif
    result = wait_child(child, time(NULL) + TIMEOUT_SECONDS);
    if (io_capture_refusal) {
        if (result != 70 || g_store_committed || !output_has(output, "incomplete")) return 6;
        printf("checkpoint io named-fifo refusal: ok\n");
        return 0;
    }
    if (connecting_refusal_case) {
        if (result != 70 || g_store_committed || !output_has(release_error, "connected/in-progress socket")) {
            fprintf(stderr, "checkpoint runner: connecting refusal result=%d committed=%d diagnostic=%d\n",
                    result, g_store_committed, output_has(release_error, "connected/in-progress socket"));
            return 6;
        }
        printf("checkpoint connecting-socket refusal: ok\n");
        return 0;
    }
    if (result != 0 || wait_for_commit(time(NULL) + TIMEOUT_SECONDS) != 0) return 6;

    if (permissive_case && !connecting_fallback_case && !io_case) {
        char external[640];
        snprintf(external, sizeof external, "%s.external", release);
        if (unlink(external) != 0) return 7;
    }
    if (modified_external_case) {
        char external[640];
        snprintf(external, sizeof external, "%s.external", release);
        fd = open(external, O_WRONLY | O_TRUNC);
        if (fd < 0 || write(fd, "after", 5) != 5 || fsync(fd) != 0) return 7;
        close(fd);
    }
    if (io_case) {
        char external[640], replacement[660], nested[680];
        snprintf(external, sizeof external, "%s.external", release);
        if (!strcmp(guest_mode, "replace")) {
            snprintf(replacement, sizeof replacement, "%s.new", external);
            fd = open(replacement, O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (fd < 0 || write(fd, "replacement", 11) != 11 || fsync(fd) != 0 || close(fd) != 0 ||
                rename(replacement, external) != 0)
                return 7;
        } else if (!strcmp(guest_mode, "recreate")) {
            if (unlink(external) != 0) return 7;
            fd = open(external, O_WRONLY | O_CREAT | O_EXCL, 0600);
            if (fd < 0 || write(fd, "recreated", 9) != 9 || fsync(fd) != 0 || close(fd) != 0) return 7;
        } else if (!strcmp(guest_mode, "directory")) {
            snprintf(nested, sizeof nested, "%s/current", external);
            fd = open(nested, O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (fd < 0 || write(fd, "current", 7) != 7 || fsync(fd) != 0 || close(fd) != 0) return 7;
        } else if (!strcmp(guest_mode, "type-change")) {
            if (unlink(external) != 0 || mkdir(external, 0700) != 0) return 7;
        } else if (!strcmp(guest_mode, "directory-change")) {
            if (rmdir(external) != 0) return 7;
            fd = open(external, O_WRONLY | O_CREAT | O_EXCL, 0600);
            if (fd < 0 || close(fd) != 0) return 7;
        } else if (!strcmp(guest_mode, "permission")) {
            if (chmod(external, 0000) != 0) return 7;
        } else if (!strcmp(guest_mode, "missing-root")) {
            if (unlink(external) != 0) return 7;
        } else if (!strcmp(guest_mode, "queued-missing")) {
            if (unlink(external) != 0) return 7;
        } else if (!strcmp(guest_mode, "missing-child-strict") ||
                   !strcmp(guest_mode, "missing-child-default")) {
            if (unlink(external) != 0) return 7;
        } else if (!strcmp(guest_mode, "append")) {
            fd = open(external, O_WRONLY | O_APPEND);
            if (fd < 0 || write(fd, "host", 4) != 4 || fsync(fd) != 0 || close(fd) != 0) return 7;
        } else if (!strcmp(guest_mode, "shortened")) {
            if (truncate(external, 2) != 0) return 7;
        }
    }

    fd = open(release, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0 || fsync(fd) != 0) return 7;
    close(fd);
    fd = open(temporary, O_RDONLY);
    if (fd < 0 || fsync(fd) != 0) return 7;
    close(fd);
    if (corrupt_magic_case && store_damage("MANIFEST", 0xffu) != 0) return 7;
    if (corrupt_truncated_case && store_truncate("proc.1/fds") != 0) return 7;
    if (corrupt_content_case && store_damage("proc.1/pages", 0x5au) != 0) return 7;
    if (corrupt_missing_case && store_remove("proc.1/signals") != 0) return 7;
    if (corrupt_extra_case && store_add("unexpected", "unexpected", 10) != 0) return 7;
    store_reset_channels();
    child = launch(argv[1], argv[2], release, output, 1,
                   io_strict_restore ? "refuse" : permissive_case ? "discard-optional" : NULL, NULL);
    if (child < 0) return 8;
    result = wait_child(child, time(NULL) + TIMEOUT_SECONDS);
    if (io_default_restore) {
        if (result != 0 || wait_for_output(output, "IO-PARENT-RESTORED", time(NULL) + TIMEOUT_SECONDS) != 0 ||
            output_has(output, "IO-CHILD-RESTORED") || !store_has("RECOVERY.jsonl", "\"outcome\":\"stopped\"") ||
            !store_has("RECOVERY.jsonl", "required external")) {
            fprintf(stderr,
                    "checkpoint runner: default-policy partial restore result=%d parent=%d child=%d stopped=%d\n",
                    result, output_has(output, "IO-PARENT-RESTORED"), output_has(output, "IO-CHILD-RESTORED"),
                    store_has("RECOVERY.jsonl", "\"outcome\":\"stopped\""));
            return 8;
        }
        printf("checkpoint io default-policy partial restore: ok\n");
        return 0;
    }
    if (io_case && (!strcmp(guest_mode, "missing-root") || !strcmp(guest_mode, "queued-missing"))) {
        if (result == 0 || result == 124 ||
            (!store_has("RECOVERY.jsonl", "container init") && !store_has("RECOVERY.jsonl", "required external") &&
             !store_has("RECOVERY.jsonl", "queued external")))
            return 8;
        printf("checkpoint io %s refusal: ok\n", guest_mode);
        return 0;
    }
    if (io_strict_restore) {
        if (result == 0 || result == 124 || !store_has("RECOVERY.jsonl", "required external")) return 8;
        printf("checkpoint io explicit-refuse missing-child refusal: ok\n");
        return 0;
    }
    if (corrupt_magic_case || corrupt_truncated_case || corrupt_content_case || corrupt_missing_case ||
        corrupt_extra_case) {
        if (result == 0 || result == 124 || output_has(output, "TREE-RESTORED")) {
            fprintf(stderr, "checkpoint runner: corrupt image accepted mode=%s result=%d restored=%d\n",
                    corrupt_magic_case ? "magic" : corrupt_truncated_case ? "truncated" :
                    corrupt_content_case ? "content" : corrupt_missing_case ? "missing" : "extra", result,
                    output_has(output, "TREE-RESTORED"));
            return 8;
        }
        printf("checkpoint corrupt-%s rejection: ok\n",
               corrupt_magic_case ? "magic" : corrupt_truncated_case ? "truncated" :
               corrupt_content_case ? "content" : corrupt_missing_case ? "missing" : "extra");
        return 0;
    }
    if (result != 0) {
        fprintf(stderr, "checkpoint runner: restore exited %d\n", result);
        return 8;
    }
    if (io_case) {
        char marker[128];
        if (!strcmp(guest_mode, "type-change") || !strcmp(guest_mode, "permission") ||
            !strcmp(guest_mode, "directory-change")) {
            if (wait_for_output(output, "IO-PARENT-RESTORED", time(NULL) + TIMEOUT_SECONDS) != 0 ||
                output_has(output, "IO-CHILD-RESTORED") ||
                !store_has("RECOVERY.jsonl", "\"outcome\":\"stopped\""))
                return 9;
        } else {
            snprintf(marker, sizeof marker, "IO-%s-RESTORED", guest_mode);
            for (char *p = marker; *p; ++p)
                if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 'a' + 'A');
            if (wait_for_output(output, marker, time(NULL) + TIMEOUT_SECONDS) != 0 ||
                !store_has("RECOVERY.jsonl", "\"outcome\":\"reconnected\""))
                return 9;
        }
        if (!strcmp(guest_mode, "repeat")) {
            store_reset_channels();
            child = launch(argv[1], argv[2], release, output, 1,
                           permissive_case ? "discard-optional" : NULL, NULL);
            if (child < 0 || wait_child(child, time(NULL) + TIMEOUT_SECONDS) != 0 ||
                wait_for_output_count(output, "IO-REPEAT-RESTORED", 2, time(NULL) + TIMEOUT_SECONDS) != 0)
                return 9;
        }
        printf("checkpoint io %s: ok\n", guest_mode);
        return 0;
    }
    if (connecting_fallback_case) {
        if (wait_for_output(output, "CONNECTING-FALLBACK-RESTORED", time(NULL) + TIMEOUT_SECONDS) != 0 ||
            !store_has("RECOVERY.jsonl", "\"outcome\":\"reconnected\""))
            return 9;
        printf("checkpoint connecting-socket fallback: ok\n");
        return 0;
    }
    if (permissive_case) {
        if (wait_for_output(output, "PARENT-RESTORED", time(NULL) + TIMEOUT_SECONDS) != 0 ||
            output_has(output, "CHILD-RESTORED") ||
            !store_has("RECOVERY.jsonl", "\"outcome\":\"stopped\"") || !store_has("RECOVERY.jsonl", "required external path"))
            return 9;
        printf("checkpoint missing-external pruning: ok\n");
        return 0;
    }
    if (modified_external_case) {
        if (wait_for_output(output, "MODIFIED-EXTERNAL-RESTORED", time(NULL) + TIMEOUT_SECONDS) != 0 ||
            !store_has("RECOVERY.jsonl", "\"outcome\":\"reconnected\""))
            return 9;
        printf("checkpoint modified-external current-state restore: ok\n");
        return 0;
    }
    if (wait_for_restored(output, pipe_case, deleted_case, threads_case, memfd_case, eventfd_case, timerfd_case,
                          signal_case ? 6 : connected_socket_case ? 5 : socket_state_case ? 4 : socketpair_case ? 3 :
                          epoll_case ? 2 : inotify_case,
                          time(NULL) + TIMEOUT_SECONDS) != 0)
        return 9;
    printf("checkpoint %s restore: ok\n",
           signal_case ? "signal" : connected_socket_case ? "connected-socket" : socket_state_case ? "socket-state" :
           socketpair_case ? "socketpair" : epoll_case ? "epoll" : inotify_case ? "inotify" : timerfd_case ? "timerfd" : eventfd_case ? "eventfd" : memfd_case ? "memfd" : threads_case ? "threads" : deleted_case ? "deleted" : pipe_case ? "pipe" : "tree");
    return 0;
}
