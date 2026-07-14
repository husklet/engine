#include "test.h"

#include "hl/fake.h"
#include "hl/linux_abi.h"
#include "../../src/linux_abi/inotify.h"
#include "../../src/linux_abi/epoll.h"

#include <stdlib.h>
#include <string.h>

typedef struct provider_root {
    uint32_t adds;
    uint32_t modifies;
    uint32_t removes;
    uint32_t clones;
    uint32_t closes;
    uint32_t last_mask;
} provider_root;

typedef struct provider {
    provider_root *root;
    hl_linux_inotify_provider_event events[16];
    char names[16][32];
    uint32_t count;
    void (*notify)(void *, uint64_t);
    void *observer;
    uint64_t observer_token;
} provider;

static hl_status provider_add(void *opaque, const char *path, size_t size, uint64_t token, uint32_t mask) {
    provider *state = opaque;
    (void)path;
    (void)size;
    (void)token;
    state->root->adds++;
    state->root->last_mask = mask;
    return HL_STATUS_OK;
}

static hl_status provider_modify(void *opaque, uint64_t token, uint32_t mask) {
    provider *state = opaque;
    (void)token;
    state->root->modifies++;
    state->root->last_mask = mask;
    return HL_STATUS_OK;
}

static hl_status provider_remove(void *opaque, uint64_t token) {
    provider *state = opaque;
    (void)token;
    state->root->removes++;
    return HL_STATUS_OK;
}

static hl_status provider_drain(void *opaque, hl_linux_inotify_provider_event *events, uint32_t capacity,
                                uint32_t *out_count) {
    provider *state = opaque;
    uint32_t count = capacity < state->count ? capacity : state->count;
    if (count == 0) return HL_STATUS_WOULD_BLOCK;
    memcpy(events, state->events, (size_t)count * sizeof(*events));
    state->count -= count;
    if (state->count != 0) memmove(state->events, state->events + count, state->count * sizeof(*state->events));
    *out_count = count;
    return HL_STATUS_OK;
}

static uint32_t provider_ready(void *opaque) {
    return ((provider *)opaque)->count != 0;
}

static hl_status provider_wait(void *opaque) {
    return ((provider *)opaque)->count != 0 ? HL_STATUS_OK : HL_STATUS_INTERRUPTED;
}

static hl_host_result provider_wait_handle(void *opaque) {
    (void)opaque;
    return (hl_host_result){HL_STATUS_NOT_SUPPORTED, 0, HL_HOST_HANDLE_INVALID, 0};
}

static hl_status provider_subscribe(void *opaque, void (*notify)(void *, uint64_t), void *observer, uint64_t token) {
    provider *state = opaque;
    state->notify = notify;
    state->observer = observer;
    state->observer_token = token;
    return HL_STATUS_OK;
}

static void provider_unsubscribe(void *opaque, void *observer, uint64_t token) {
    provider *state = opaque;
    if (state->observer == observer && state->observer_token == token) {
        state->notify = NULL;
        state->observer = NULL;
    }
}

static hl_status provider_clone(void *opaque, void **out_context) {
    provider *source = opaque;
    provider *copy = calloc(1, sizeof(*copy));
    if (copy == NULL) return HL_STATUS_OUT_OF_MEMORY;
    *copy = *source;
    copy->notify = NULL;
    copy->observer = NULL;
    copy->root->clones++;
    *out_context = copy;
    return HL_STATUS_OK;
}

static hl_status provider_close(void *opaque) {
    provider *state = opaque;
    state->root->closes++;
    free(state);
    return HL_STATUS_OK;
}

static const hl_linux_inotify_provider_ops provider_ops = {
    provider_add,   provider_modify,    provider_remove,      provider_drain, provider_wait, provider_wait_handle,
    provider_ready, provider_subscribe, provider_unsubscribe, provider_clone, provider_close};

static hl_host_result unused_file_clone(void *context, hl_host_handle handle) {
    (void)context;
    (void)handle;
    return (hl_host_result){HL_STATUS_NOT_SUPPORTED, 0, HL_HOST_HANDLE_INVALID, 0};
}

static hl_host_result unused_file_close(void *context, hl_host_handle handle) {
    (void)context;
    (void)handle;
    return (hl_host_result){HL_STATUS_OK, 0, 0, 0};
}

static void emit(provider *state, uint64_t token, uint32_t mask, uint32_t cookie, const char *name) {
    uint32_t index = state->count++;
    size_t size = name == NULL ? 0 : strlen(name);
    if (name != NULL) memcpy(state->names[index], name, size + 1u);
    state->events[index] = (hl_linux_inotify_provider_event){token, mask, cookie, state->names[index], size};
    if (state->notify != NULL) state->notify(state->observer, state->observer_token);
}

int main(void) {
    hl_fake_host fake;
    hl_host_services services;
    hl_linux_abi abi;
    hl_linux_fd_entry fds[16];
    hl_linux_ofd_entry ofds[16];
    provider_root root = {0};
    provider *state = calloc(1, sizeof(*state));
    int64_t fd;
    int64_t alias;
    int64_t wd;
    int64_t epoll_fd;
    hl_linux_epoll_event epoll_event;
    unsigned char bytes[128];
    uint32_t value;
    hl_linux_fork_record records[16];
    hl_linux_fork_plan plan = {
        .abi = HL_LINUX_ABI_VERSION, .size = sizeof(plan), .records = records, .capacity = HL_ARRAY_COUNT(records)};
    HL_CHECK(state != NULL);
    state->root = &root;
    hl_fake_host_init(&fake, &services);
    {
        static const hl_host_file_services files = {
            .abi = HL_HOST_FILE_ABI,
            .size = sizeof(files),
            .clone_for_fork = unused_file_clone,
            .close = unused_file_close,
        };
        services.capabilities |= HL_HOST_CAP_FILE;
        services.file = &files;
    }
    HL_CHECK(hl_linux_abi_init(&abi, &services, fds, HL_ARRAY_COUNT(fds), ofds, HL_ARRAY_COUNT(ofds)) == HL_STATUS_OK);

    fd = hl_linux_inotify_create(&abi, &provider_ops, state, 0, 00004000u);
    HL_CHECK(fd >= 0);
    wd = hl_linux_inotify_add(&abi, (hl_linux_fd)fd, "/tmp/watch", 10, HL_LINUX_IN_CREATE);
    HL_CHECK(wd > 0 && root.adds == 1 && root.last_mask == HL_LINUX_IN_CREATE);
    HL_CHECK(hl_linux_inotify_add(&abi, (hl_linux_fd)fd, "/tmp/watch", 10, HL_LINUX_IN_DELETE | HL_LINUX_IN_MASK_ADD) ==
             wd);
    HL_CHECK(root.modifies == 1 &&
             (root.last_mask & (HL_LINUX_IN_CREATE | HL_LINUX_IN_DELETE)) == (HL_LINUX_IN_CREATE | HL_LINUX_IN_DELETE));
    HL_CHECK(hl_linux_inotify_add(&abi, (hl_linux_fd)fd, "/tmp/watch", 10,
                                  HL_LINUX_IN_CREATE | HL_LINUX_IN_MASK_CREATE) == -HL_LINUX_EEXIST);
    epoll_fd = hl_linux_epoll_create(&abi, 0);
    HL_CHECK(epoll_fd >= 0);
    HL_CHECK(hl_linux_epoll_control(&abi, (hl_linux_fd)epoll_fd, HL_LINUX_EPOLL_ADD, (hl_linux_fd)fd,
                                    HL_LINUX_READY_READ, 0xabcdu) == 0);
    emit(state, 1, HL_LINUX_IN_CREATE | HL_LINUX_IN_ISDIR, 91, "child");
    HL_CHECK(hl_linux_epoll_wait(&abi, (hl_linux_fd)epoll_fd, &epoll_event, 1, 0) == 1);
    HL_CHECK(epoll_event.data == 0xabcdu && (epoll_event.readiness & HL_LINUX_READY_READ) != 0);
    HL_CHECK(hl_linux_close(&abi, (hl_linux_fd)epoll_fd) == 0);
    HL_CHECK(hl_linux_read(&abi, (hl_linux_fd)fd, bytes, 16) == -HL_LINUX_EINVAL);
    HL_CHECK(hl_linux_read(&abi, (hl_linux_fd)fd, bytes, sizeof(bytes)) == 24);
    memcpy(&value, bytes + 4, 4);
    HL_CHECK((value & (HL_LINUX_IN_CREATE | HL_LINUX_IN_ISDIR)) == (HL_LINUX_IN_CREATE | HL_LINUX_IN_ISDIR));
    memcpy(&value, bytes + 8, 4);
    HL_CHECK(value == 91 && !strcmp((char *)bytes + 16, "child"));

    alias = hl_linux_dup3(&abi, (hl_linux_fd)fd, 8, 0);
    HL_CHECK(alias == 8);
    HL_CHECK(hl_linux_close(&abi, (hl_linux_fd)fd) == 0 && root.closes == 0);
    HL_CHECK(hl_linux_inotify_remove(&abi, (hl_linux_fd)alias, 9999) == -HL_LINUX_EINVAL);
    HL_CHECK(hl_linux_inotify_remove(&abi, (hl_linux_fd)alias, (int32_t)wd) == 0 && root.removes == 1);
    HL_CHECK(hl_linux_read(&abi, (hl_linux_fd)alias, bytes, sizeof(bytes)) == 16);
    memcpy(&value, bytes + 4, 4);
    HL_CHECK(value == HL_LINUX_IN_IGNORED);

    wd = hl_linux_inotify_add(&abi, (hl_linux_fd)alias, "/tmp/once", 9, HL_LINUX_IN_MODIFY | HL_LINUX_IN_ONESHOT);
    HL_CHECK(wd > 0);
    emit(state, 2, HL_LINUX_IN_MODIFY, 0, NULL);
    HL_CHECK(hl_linux_read(&abi, (hl_linux_fd)alias, bytes, sizeof(bytes)) == 32);
    memcpy(&value, bytes + 4, 4);
    HL_CHECK(value == HL_LINUX_IN_MODIFY);
    memcpy(&value, bytes + 20, 4);
    HL_CHECK(value == HL_LINUX_IN_IGNORED && root.removes == 2);

    {
        hl_status fork_status = hl_linux_abi_fork_prepare(&abi, &plan);
        HL_CHECK(fork_status == HL_STATUS_OK);
        HL_CHECK(root.clones == 1);
    }
    HL_CHECK(hl_linux_abi_fork_parent(&abi, &plan) == HL_STATUS_OK && root.closes == 1);
    HL_CHECK(hl_linux_close(&abi, (hl_linux_fd)alias) == 0 && root.closes == 2);
    HL_CHECK(hl_linux_abi_destroy(&abi) == HL_STATUS_OK);
    return EXIT_SUCCESS;
}
