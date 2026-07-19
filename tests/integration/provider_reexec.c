#define _POSIX_C_SOURCE 200809L
#include "hl/activation.h"
#include "hl/config.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

enum {
    HEADER = 32,
    HELLO = 1,
    READY = 2,
    REQUEST = 3,
    REPLY = 4,
    CANCEL = 5,
    NAMESPACE_INSTALL = 7,
    NAMESPACE_READY = 8,
    SUBSCRIBE = 9,
    UNSUBSCRIBE = 10,
    READINESS_EVENT = 11
};

enum { OPEN = 1, READ = 2, WRITE = 3, SEEK = 4, STAT = 5, POLL = 6, CLOSE = 7 };

static void put16(unsigned char *p, uint16_t v) {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

static void put32(unsigned char *p, uint32_t v) {
    put16(p, (uint16_t)v);
    put16(p + 2, (uint16_t)(v >> 16));
}

static void put64(unsigned char *p, uint64_t v) {
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

static uint16_t get16(const unsigned char *p) {
    return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}

static uint32_t get32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t get64(const unsigned char *p) {
    return (uint64_t)get32(p) | (uint64_t)get32(p + 4) << 32;
}

static int exact(int fd, void *p, size_t size, int writing) {
    size_t used = 0;
    while (used < size) {
        ssize_t count = writing ? write(fd, (char *)p + used, size - used) : read(fd, (char *)p + used, size - used);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return -1;
        used += (size_t)count;
    }
    return 0;
}

static int frame(int fd, uint16_t kind, uint64_t id, const void *payload, uint32_t size) {
    unsigned char h[HEADER];
    memset(h, 0, sizeof(h));
    put32(h, UINT32_C(0x484c5052));
    put16(h + 4, 1);
    put16(h + 6, kind);
    put32(h + 8, size);
    put64(h + 12, id);
    return exact(fd, h, sizeof(h), 1) || (size != 0 && exact(fd, (void *)(uintptr_t)payload, size, 1));
}

static int receive(int fd, uint16_t *kind, uint64_t *id, unsigned char *payload, uint32_t capacity, uint32_t *size) {
    unsigned char h[HEADER];
    if (exact(fd, h, sizeof(h), 0) != 0) return -1;
    if (get32(h) != UINT32_C(0x484c5052) || get16(h + 4) != 1) {
        fprintf(stderr, "provider bad frame magic=%x version=%u kind=%u size=%u\n", get32(h), get16(h + 4),
                get16(h + 6), get32(h + 8));
        return -1;
    }
    *kind = get16(h + 6);
    *size = get32(h + 8);
    *id = get64(h + 12);
    if (*size > capacity) return -1;
    return *size == 0 ? 0 : exact(fd, payload, *size, 0);
}

static int make_config(const char *root, char path[64]) {
    hl_launch_config launch = {0};
    unsigned char pool[4096] = {0};
    const char argument[] = "/provider-loopback";
    size_t argument_size = sizeof(argument), root_size = strlen(root) + 1, root_offset = 1 + argument_size + 1;
    int fd;
    if (root_offset + root_size > sizeof(pool)) return -1;
    strcpy(path, "/tmp/hl-provider-reexec.XXXXXX");
    fd = mkstemp(path);
    if (fd < 0) return -1;
    memcpy(pool + 1, argument, argument_size);
    memcpy(pool + root_offset, root, root_size);
    launch.magic = HL_CONFIG_MAGIC;
    launch.abi = HL_CONFIG_ABI;
    launch.header_size = sizeof(launch);
    launch.pool_size = (uint32_t)(root_offset + root_size);
    launch.uid = -1;
    launch.gid = -1;
    launch.arguments_offset = 1;
    launch.rootfs_offset = (uint32_t)root_offset;
    launch.process_domain[0] = 1;
    if (write(fd, &launch, sizeof(launch)) != sizeof(launch) || write(fd, pool, launch.pool_size) != launch.pool_size ||
        close(fd) != 0) {
        close(fd);
        unlink(path);
        return -1;
    }
    return 0;
}

typedef struct provider {
    int fd;
    char bytes[5];
    uint64_t offset;
    unsigned closes;
} provider;

static int reply(provider *p, uint64_t id, const unsigned char *request, uint32_t size) {
    unsigned char out[128] = {0};
    uint64_t offset, at;
    uint32_t count;
    if (size == 0) return -1;
    switch (request[0]) {
    case OPEN:
        if (size != 10 || get64(request + 1) != 77) return -1;
        out[0] = OPEN;
        put64(out + 1, 123);
        return frame(p->fd, REPLY, id, out, 9);
    case READ:
        if (size != 21 || get64(request + 1) != 123) return -1;
        at = get64(request + 9);
        offset = at == UINT64_MAX ? p->offset : at;
        count = get32(request + 17);
        if (offset > sizeof(p->bytes))
            count = 0;
        else if (count > sizeof(p->bytes) - offset)
            count = (uint32_t)(sizeof(p->bytes) - offset);
        out[0] = READ;
        put32(out + 1, count);
        memcpy(out + 5, p->bytes + offset, count);
        if (at == UINT64_MAX) p->offset += count;
        return frame(p->fd, REPLY, id, out, 5 + count);
    case WRITE:
        if (size < 21 || get64(request + 1) != 123 || get32(request + 17) != size - 21) return -1;
        at = get64(request + 9);
        offset = at == UINT64_MAX ? p->offset : at;
        count = size - 21;
        if (offset > sizeof(p->bytes) || count > sizeof(p->bytes) - offset) return -1;
        memcpy(p->bytes + offset, request + 21, count);
        if (at == UINT64_MAX) p->offset += count;
        out[0] = WRITE;
        put32(out + 1, count);
        return frame(p->fd, REPLY, id, out, 5);
    case SEEK: {
        int64_t delta;
        if (size != 18 || get64(request + 1) != 123) return -1;
        delta = (int64_t)get64(request + 9);
        if (request[17] == 0)
            p->offset = (uint64_t)delta;
        else if (request[17] == 1)
            p->offset = (uint64_t)((int64_t)p->offset + delta);
        else if (request[17] == 2)
            p->offset = (uint64_t)((int64_t)sizeof(p->bytes) + delta);
        else
            return -1;
        out[0] = SEEK;
        put64(out + 1, p->offset);
        return frame(p->fd, REPLY, id, out, 9);
    }
    case STAT:
        if (size != 9 || get64(request + 1) != 123) return -1;
        out[0] = STAT;
        put32(out + 1, 0660);
        put32(out + 5, 0);
        put32(out + 9, 0);
        put64(out + 13, sizeof(p->bytes));
        return frame(p->fd, REPLY, id, out, 25);
    case POLL:
        if (size != 10 || get64(request + 1) != 123) return -1;
        out[0] = POLL;
        out[1] = request[9] & 3;
        return frame(p->fd, REPLY, id, out, 2);
    case CLOSE:
        if (size != 9 || get64(request + 1) != 123) return -1;
        p->closes++;
        out[0] = CLOSE;
        return frame(p->fd, REPLY, id, out, 1);
    default: return -1;
    }
}

static unsigned char *namespace_entry(unsigned char *cursor, uint8_t kind, uint64_t service, uint32_t mode,
                                      const char *path, const char *target) {
    size_t path_size = strlen(path);
    size_t target_size = target == NULL ? 0 : strlen(target);
    *cursor++ = kind;
    put64(cursor, service);
    cursor += 8;
    put32(cursor, mode);
    cursor += 4;
    put32(cursor, 0);
    cursor += 4;
    put32(cursor, 0);
    cursor += 4;
    put16(cursor, (uint16_t)path_size);
    cursor += 2;
    memcpy(cursor, path, path_size);
    cursor += path_size;
    put16(cursor, (uint16_t)target_size);
    cursor += 2;
    if (target_size != 0) {
        memcpy(cursor, target, target_size);
        cursor += target_size;
    }
    return cursor;
}

static void *serve(void *opaque) {
    provider *p = opaque;
    unsigned char payload[65536], namespace[256] = {0}, ready[HEADER] = {0};
    uint16_t kind;
    uint64_t id;
    uint32_t size;
    unsigned char *cursor = namespace;
    if (receive(p->fd, &kind, &id, payload, sizeof(payload), &size) != 0) return (void *)11;
    if (kind != HELLO) return (void *)12;
    if (size != 0) return (void *)13;
    put32(ready, UINT32_C(0x484c5052));
    put16(ready + 4, 1);
    put16(ready + 6, READY);
    ready[20] = 1;
    if (exact(p->fd, ready, sizeof(ready), 1) != 0) return (void *)2;
    put32(cursor, UINT32_C(0x80000003));
    cursor += 4;
    cursor = namespace_entry(cursor, 2, 0, 0755, "/run/domain", NULL);
    cursor = namespace_entry(cursor, 1, 77, 0660, "/run/domain/control", NULL);
    cursor = namespace_entry(cursor, 3, 0, 0777, "/run/domain/current", "control");
    if (frame(p->fd, NAMESPACE_INSTALL, 0, namespace, (uint32_t)(cursor - namespace)) != 0) return (void *)3;
    if (receive(p->fd, &kind, &id, payload, sizeof(payload), &size) != 0 || kind != NAMESPACE_READY) return (void *)4;
    for (;;) {
        if (receive(p->fd, &kind, &id, payload, sizeof(payload), &size) != 0) return p->closes == 1 ? NULL : (void *)5;
        if (kind == CANCEL) continue;
        if (kind == SUBSCRIBE) {
            unsigned char event[2] = {POLL, 3};
            if (size != 10 || payload[0] != POLL || get64(payload + 1) != 123) return (void *)7;
        struct timespec delay = { .tv_sec = 0, .tv_nsec = 100000000L };
        while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
        }
            if (frame(p->fd, READINESS_EVENT, id, event, sizeof(event)) != 0) return (void *)8;
            continue;
        }
        if (kind == UNSUBSCRIBE) continue;
        if (kind != REQUEST || reply(p, id, payload, size) != 0) return (void *)6;
        if (payload[0] == CLOSE) return NULL;
    }
}

int main(int argc, char **argv) {
    int pair[2], output[2];
    char config[64], executable[4096], text[4096] = {0};
    hl_activation_process *process = NULL;
    hl_activation_stdio stdio;
    hl_engine_exit result = {.abi = HL_ENGINE_ABI, .size = sizeof(result)};
    provider state;
    pthread_t thread;
    void *server_result = (void *)99;
    ssize_t count;
    hl_status status;
    if (argc != 2 || realpath(argv[0], executable) == NULL || make_config(argv[1], config) != 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0 ||
        pipe(output) != 0)
        return 1;
    state = (provider){.fd = pair[1], .bytes = {'h', 'e', 'l', 'l', 'o'}};
    if (pthread_create(&thread, NULL, serve, &state) != 0) return 2;
    stdio = (hl_activation_stdio){.input = -1, .output = output[1], .error = output[1]};
    status = hl_activation_start_with_transport(executable, HL_GUEST_ISA_AARCH64, config, &stdio, pair[0], &process);
    close(pair[0]);
    close(output[1]);
    count = read(output[0], text, sizeof(text) - 1);
    close(output[0]);
    if (status == HL_STATUS_OK) status = hl_activation_wait(process, &result);
    hl_activation_process_destroy(process);
    pthread_join(thread, &server_result);
    close(pair[1]);
    unlink(config);
    if (status != HL_STATUS_OK || result.kind != HL_ENGINE_EXIT_CODE || result.guest_status != 0 ||
        server_result != NULL || state.closes != 1 || count <= 0 || strstr(text, "provider-loopback ok") == NULL) {
        fprintf(stderr, "provider reexec status=%d kind=%u guest=%d server=%p closes=%u output=%s\n", status,
                result.kind, result.guest_status, server_result, state.closes, text);
        return 3;
    }
    return 0;
}
