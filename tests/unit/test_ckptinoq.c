// Repro: hl_linux_inotify_import_at() accepts a checkpoint inotify image whose event queue blob is not
// self-consistent (it only validates the TOTAL queue_size, never walks the 16-byte record framing).
// inotify_read() then trusts the per-record `length` field, walks past the end of the heap queue buffer,
// memcpy()s out-of-bounds and underflows object->queue_size -> SIGSEGV / heap disclosure.
//
// src/linux_abi/inotify.c:625 (import: `memcpy(object->queue, cursor, queue_size)` with no framing check)
// src/linux_abi/inotify.c:170-186 (inotify_read: record = 16u + length, unbounded vs queue_size)

#include "test.h"

#include "hl/fake.h"
#include "hl/linux_abi.h"
#include "../../src/linux_abi/inotify.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct image_header {
    uint64_t magic;
    uint32_t version;
    uint32_t watch_count;
    int32_t next_wd;
    uint32_t nonblocking;
    uint64_t queue_size;
} image_header;

static hl_status op_add(void *c, const char *p, size_t n, uint64_t t, uint32_t m) {
    (void)c;
    (void)p;
    (void)n;
    (void)t;
    (void)m;
    return HL_STATUS_OK;
}

static hl_status op_modify(void *c, uint64_t t, uint32_t m) {
    (void)c;
    (void)t;
    (void)m;
    return HL_STATUS_OK;
}

static hl_status op_remove(void *c, uint64_t t) {
    (void)c;
    (void)t;
    return HL_STATUS_OK;
}

static hl_status op_drain(void *c, hl_linux_inotify_provider_event *e, uint32_t cap, uint32_t *out) {
    (void)c;
    (void)e;
    (void)cap;
    (void)out;
    return HL_STATUS_WOULD_BLOCK;
}

static hl_status op_wait(void *c) {
    (void)c;
    return HL_STATUS_INTERRUPTED;
}

static hl_host_result op_wait_handle(void *c) {
    (void)c;
    return (hl_host_result){HL_STATUS_NOT_SUPPORTED, 0, HL_HOST_HANDLE_INVALID, 0};
}

static uint32_t op_ready(void *c) {
    (void)c;
    return 0;
}

static hl_status op_subscribe(void *c, void (*n)(void *, uint64_t), void *o, uint64_t t) {
    (void)c;
    (void)n;
    (void)o;
    (void)t;
    return HL_STATUS_OK;
}

static void op_unsubscribe(void *c, void *o, uint64_t t) {
    (void)c;
    (void)o;
    (void)t;
}

static hl_status op_clone(void *c, void **out) {
    (void)c;
    *out = (void *)1;
    return HL_STATUS_OK;
}

static hl_status op_close(void *c) {
    (void)c;
    return HL_STATUS_OK;
}

static const hl_linux_inotify_provider_ops ops = {op_add,         op_modify,      op_remove, op_drain,
                                                  op_wait,        op_wait_handle, op_ready,  op_subscribe,
                                                  op_unsubscribe, op_clone,       op_close};

static hl_host_result unused_file_clone(void *context, hl_host_handle handle) {
    (void)context;
    (void)handle;
    return (hl_host_result){HL_STATUS_NOT_SUPPORTED, 0, HL_HOST_HANDLE_INVALID, 0};
}

static hl_host_result unused_file_close(void *context, hl_host_handle handle) {
    (void)context;
    (void)handle;
    return (hl_host_result){HL_STATUS_NOT_SUPPORTED, 0, HL_HOST_HANDLE_INVALID, 0};
}

int main(void) {
    hl_fake_host fake;
    hl_host_services services;
    hl_linux_abi abi;
    hl_linux_fd_entry fds[16] = {0};
    hl_linux_ofd_entry ofds[16] = {0};
    unsigned char image[sizeof(image_header) + 16];
    image_header header;
    unsigned char record[16];
    uint32_t field;
    unsigned char out[8192];
    int64_t imported, got;

    hl_fake_host_init(&fake, &services);
    {
        static const hl_host_file_services files = {.abi = HL_HOST_FILE_ABI,
                                                    .size = sizeof(files),
                                                    .clone_for_fork = unused_file_clone,
                                                    .close = unused_file_close};
        services.capabilities |= HL_HOST_CAP_FILE;
        services.file = &files;
    }
    HL_CHECK(hl_linux_abi_init(&abi, &services, fds, HL_ARRAY_COUNT(fds), ofds, HL_ARRAY_COUNT(ofds)) == HL_STATUS_OK);

    memset(&header, 0, sizeof header);
    header.magic = UINT64_C(0x484c494e4f544659);
    header.version = 1;
    header.watch_count = 0;
    header.next_wd = 1;
    header.nonblocking = 0;
    header.queue_size = 16; // exactly one 16-byte event header, no name payload

    // The single queued record CLAIMS a 4096-byte name payload that is not in the blob.
    memset(record, 0, sizeof record);
    field = 1;
    memcpy(record + 0, &field, 4); // wd
    field = 0x100;
    memcpy(record + 4, &field, 4); // mask (IN_CREATE)
    field = 0;
    memcpy(record + 8, &field, 4); // cookie
    field = 4096;
    memcpy(record + 12, &field, 4); // len  <-- lie

    memcpy(image, &header, sizeof header);
    memcpy(image + sizeof header, record, sizeof record);

    imported = hl_linux_inotify_import_at(&abi, (hl_linux_fd)5, &ops, (void *)1, 0, 0, image, sizeof image);
    fprintf(stderr, "import_at returned %lld (expected: rejection, i.e. negative)\n", (long long)imported);
    if (imported < 0) {
        fprintf(stderr, "OK: malformed queue rejected\n");
        return 0;
    }
    fprintf(stderr, "BUG: malformed queue accepted; reading it now...\n");
    got = hl_linux_read(&abi, (hl_linux_fd)5, out, sizeof out);
    fprintf(stderr, "read returned %lld\n", (long long)got);
    return 0;
}
