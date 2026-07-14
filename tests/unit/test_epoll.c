#include "test.h"

#include "hl/fake.h"
#include "hl/linux_abi.h"
#include "../../src/linux_abi/epoll.h"

#include <stdatomic.h>
#include <pthread.h>
#include <string.h>

typedef struct ready_object {
    atomic_flag guard;
    _Atomic uint32_t ready;
    uint32_t closes;
    uint32_t clones;
    hl_status clone_status;
    void (*notifies[8])(void *observer, uint64_t token);
    void *observers[8];
    uint64_t tokens[8];
} ready_object;

static void ready_lock(ready_object *object) {
    while (atomic_flag_test_and_set_explicit(&object->guard, memory_order_acquire)) {}
}

static void ready_unlock(ready_object *object) {
    atomic_flag_clear_explicit(&object->guard, memory_order_release);
}

static hl_status ready_subscribe(void *opaque, void (*notify)(void *observer, uint64_t token), void *observer,
                                 uint64_t token) {
    ready_object *object = opaque;
    uint32_t index;
    ready_lock(object);
    for (index = 0; index < HL_ARRAY_COUNT(object->notifies); ++index) {
        if (object->notifies[index] == NULL) {
            object->notifies[index] = notify;
            object->observers[index] = observer;
            object->tokens[index] = token;
            ready_unlock(object);
            return HL_STATUS_OK;
        }
    }
    ready_unlock(object);
    return HL_STATUS_RESOURCE_LIMIT;
}

static void ready_unsubscribe(void *opaque, void *observer, uint64_t token) {
    ready_object *object = opaque;
    uint32_t index;
    ready_lock(object);
    for (index = 0; index < HL_ARRAY_COUNT(object->notifies); ++index) {
        if (object->observers[index] == observer && object->tokens[index] == token) {
            object->notifies[index] = NULL;
            object->observers[index] = NULL;
            object->tokens[index] = 0;
            ready_unlock(object);
            return;
        }
    }
    ready_unlock(object);
}

static void ready_set(ready_object *object, uint32_t ready) {
    uint32_t index;
    atomic_store_explicit(&object->ready, ready, memory_order_release);
    ready_lock(object);
    for (index = 0; index < HL_ARRAY_COUNT(object->notifies); ++index)
        if (object->notifies[index] != NULL) object->notifies[index](object->observers[index], object->tokens[index]);
    ready_unlock(object);
}

static uint32_t ready_read(void *opaque, uint32_t interests) {
    ready_object *object = opaque;
    return atomic_load_explicit(&object->ready, memory_order_acquire) != 0 ? interests & HL_LINUX_READY_READ : 0;
}

static hl_status ready_clone(void *opaque, void **child) {
    ready_object *object = opaque;
    object->clones++;
    if (object->clone_status != HL_STATUS_OK) return object->clone_status;
    *child = object;
    return HL_STATUS_OK;
}

static hl_status ready_close(void *opaque) {
    ready_object *object = opaque;
    object->closes++;
    ready_lock(object);
    memset(object->notifies, 0, sizeof(object->notifies));
    memset(object->observers, 0, sizeof(object->observers));
    memset(object->tokens, 0, sizeof(object->tokens));
    ready_unlock(object);
    return HL_STATUS_OK;
}

static const hl_linux_object_ops ready_ops = {.readiness = ready_read,
                                              .subscribe = ready_subscribe,
                                              .unsubscribe = ready_unsubscribe,
                                              .clone = ready_clone,
                                              .close = ready_close};

typedef struct test_fixture {
    hl_fake_host fake;
    hl_host_services host;
    hl_linux_abi abi;
    hl_linux_fd_entry fds[32];
    hl_linux_ofd_entry ofds[32];
    pthread_mutex_t event_mutex;
    pthread_cond_t event_cond;
    _Atomic uint32_t event_waiting;
    uint32_t event_epoch;
    uint32_t event_next;
    uint32_t event_closes;
} test_fixture;

static hl_host_result unused_file(void *context, hl_host_handle file) {
    (void)context;
    (void)file;
    return (hl_host_result){HL_STATUS_OK, 0, 1, 0};
}

static const hl_host_file_services fork_files = {
    .abi = HL_HOST_FILE_ABI, .size = sizeof(fork_files), .close = unused_file, .clone_for_fork = unused_file};

static hl_host_result blocking_create(void *context) {
    test_fixture *fx = context;
    return (hl_host_result){HL_STATUS_OK, 0, ++fx->event_next, 0};
}

static hl_host_result blocking_wait(void *context, hl_host_handle pollset, hl_host_event_record *events,
                                    size_t capacity, uint64_t deadline) {
    test_fixture *fx = context;
    uint32_t epoch;
    (void)pollset;
    (void)events;
    (void)capacity;
    (void)deadline;
    pthread_mutex_lock(&fx->event_mutex);
    epoch = fx->event_epoch;
    atomic_store_explicit(&fx->event_waiting, 1, memory_order_release);
    while (epoch == fx->event_epoch)
        pthread_cond_wait(&fx->event_cond, &fx->event_mutex);
    pthread_mutex_unlock(&fx->event_mutex);
    return (hl_host_result){HL_STATUS_OK, 0, 0, 0};
}

static hl_host_result blocking_wake(void *context, hl_host_handle pollset) {
    test_fixture *fx = context;
    (void)pollset;
    pthread_mutex_lock(&fx->event_mutex);
    fx->event_epoch++;
    pthread_cond_broadcast(&fx->event_cond);
    pthread_mutex_unlock(&fx->event_mutex);
    return (hl_host_result){HL_STATUS_OK, 0, 0, 0};
}

static hl_host_result blocking_close(void *context, hl_host_handle pollset) {
    test_fixture *fx = context;
    fx->event_closes++;
    return blocking_wake(context, pollset);
}

static const hl_host_event_services blocking_events = {.abi = HL_HOST_EVENT_ABI,
                                                       .size = sizeof(blocking_events),
                                                       .create = blocking_create,
                                                       .wait = blocking_wait,
                                                       .wake = blocking_wake,
                                                       .close = blocking_close};

static int fixture_init(test_fixture *fx) {
    memset(fx, 0, sizeof(*fx));
    hl_fake_host_init(&fx->fake, &fx->host);
    fx->host.capabilities |= HL_HOST_CAP_FILE;
    fx->host.file = &fork_files;
    pthread_mutex_init(&fx->event_mutex, NULL);
    pthread_cond_init(&fx->event_cond, NULL);
    HL_CHECK((hl_linux_abi_init(&fx->abi, &fx->host, fx->fds, 32, fx->ofds, 32)) == (HL_STATUS_OK));
    return 0;
}

static hl_linux_fd install(test_fixture *fx, ready_object *object) {
    hl_linux_fd fd = UINT32_MAX;
    atomic_flag_clear_explicit(&object->guard, memory_order_release);
    HL_CHECK((hl_linux_object_install(&fx->abi, &ready_ops, object, 77, 0, 0, &fd)) == (HL_STATUS_OK));
    return fd;
}

static int close_fd(test_fixture *fx, hl_linux_fd fd) {
    HL_CHECK((hl_linux_fd_close(&fx->abi, fd, NULL)) == (HL_STATUS_OK));
    return 0;
}

static int test_alias_and_reuse(void) {
    test_fixture fx;
    ready_object first = {0};
    ready_object replacement = {0};
    hl_linux_epoll_event events[4];
    hl_linux_fd fd;
    hl_linux_fd alias;
    int64_t epoll;
    HL_CHECK(fixture_init(&fx) == 0);
    fd = install(&fx, &first);
    HL_CHECK((hl_linux_fd_dup(&fx.abi, fd, 0, &alias)) == (HL_STATUS_OK));
    epoll = hl_linux_epoll_create(&fx.abi, 0);
    HL_CHECK(epoll >= 0);
    HL_CHECK(hl_linux_epoll_control(&fx.abi, (hl_linux_fd)epoll, HL_LINUX_EPOLL_ADD, fd, HL_LINUX_READY_READ, 11) == 0);
    HL_CHECK(close_fd(&fx, fd) == 0);
    atomic_store_explicit(&first.ready, 1, memory_order_release);
    HL_CHECK((hl_linux_epoll_wait(&fx.abi, (hl_linux_fd)epoll, events, 4, 0)) == (1));
    HL_CHECK((events[0].data) == (11));
    HL_CHECK(close_fd(&fx, alias) == 0);
    atomic_flag_clear_explicit(&replacement.guard, memory_order_release);
    HL_CHECK((hl_linux_object_install_at(&fx.abi, fd, &ready_ops, &replacement, 77, 0, 0)) == (HL_STATUS_OK));
    ready_set(&replacement, 1);
    HL_CHECK((hl_linux_epoll_wait(&fx.abi, (hl_linux_fd)epoll, events, 4, 0)) == (0));
    ready_set(&replacement, 0);
    ready_set(&replacement, 1);
    HL_CHECK((hl_linux_epoll_wait(&fx.abi, (hl_linux_fd)epoll, events, 4, 0)) == (0));
    HL_CHECK(close_fd(&fx, fd) == 0);
    HL_CHECK(close_fd(&fx, (hl_linux_fd)epoll) == 0);
    HL_CHECK((first.closes) == (1));
    HL_CHECK((replacement.closes) == (1));
    return 0;
}

static int test_distinct_aliases_edge_and_oneshot(void) {
    test_fixture fx;
    ready_object object = {0};
    hl_linux_epoll_event events[4];
    hl_linux_fd fd;
    hl_linux_fd alias;
    int64_t epoll;
    HL_CHECK(fixture_init(&fx) == 0);
    fd = install(&fx, &object);
    HL_CHECK((hl_linux_fd_dup(&fx.abi, fd, 0, &alias)) == (HL_STATUS_OK));
    epoll = hl_linux_epoll_create(&fx.abi, 0);
    HL_CHECK(epoll >= 0);
    HL_CHECK(hl_linux_epoll_control(&fx.abi, (hl_linux_fd)epoll, HL_LINUX_EPOLL_ADD, fd,
                                    HL_LINUX_READY_READ | HL_LINUX_EPOLL_EDGE, 21) == 0);
    HL_CHECK(hl_linux_epoll_control(&fx.abi, (hl_linux_fd)epoll, HL_LINUX_EPOLL_ADD, alias,
                                    HL_LINUX_READY_READ | HL_LINUX_EPOLL_ONESHOT, 22) == 0);
    atomic_store_explicit(&object.ready, 1, memory_order_release);
    HL_CHECK((hl_linux_epoll_wait(&fx.abi, (hl_linux_fd)epoll, events, 4, 0)) == (2));
    HL_CHECK((events[0].data == 21 && events[1].data == 22) || (events[0].data == 22 && events[1].data == 21));
    HL_CHECK((hl_linux_epoll_wait(&fx.abi, (hl_linux_fd)epoll, events, 4, 0)) == (0));
    atomic_store_explicit(&object.ready, 0, memory_order_release);
    HL_CHECK((hl_linux_epoll_wait(&fx.abi, (hl_linux_fd)epoll, events, 4, 0)) == (0));
    atomic_store_explicit(&object.ready, 1, memory_order_release);
    HL_CHECK((hl_linux_epoll_wait(&fx.abi, (hl_linux_fd)epoll, events, 4, 0)) == (1));
    HL_CHECK((events[0].data) == (21));
    HL_CHECK(hl_linux_epoll_control(&fx.abi, (hl_linux_fd)epoll, HL_LINUX_EPOLL_MODIFY, alias,
                                    HL_LINUX_READY_READ | HL_LINUX_EPOLL_ONESHOT, 23) == 0);
    HL_CHECK((hl_linux_epoll_wait(&fx.abi, (hl_linux_fd)epoll, events, 4, 0)) == (1));
    HL_CHECK((events[0].data) == (23));
    HL_CHECK(close_fd(&fx, alias) == 0);
    HL_CHECK(close_fd(&fx, fd) == 0);
    HL_CHECK(close_fd(&fx, (hl_linux_fd)epoll) == 0);
    return 0;
}

static int test_fork_clone(void) {
    test_fixture fx;
    ready_object object = {0};
    hl_linux_fork_record records[32];
    hl_linux_fork_plan plan = {HL_LINUX_ABI_VERSION, sizeof(plan), records, 32, 0, 0, 0};
    hl_linux_fd fd;
    int64_t epoll;
    HL_CHECK(fixture_init(&fx) == 0);
    fd = install(&fx, &object);
    epoll = hl_linux_epoll_create(&fx.abi, 0);
    HL_CHECK(epoll >= 0);
    HL_CHECK(hl_linux_epoll_control(&fx.abi, (hl_linux_fd)epoll, HL_LINUX_EPOLL_ADD, fd, HL_LINUX_READY_READ, 31) == 0);
    {
        hl_status status = hl_linux_abi_fork_prepare(&fx.abi, &plan);
        if (status != HL_STATUS_OK) fprintf(stderr, "fork prepare: %d\n", status);
        HL_CHECK(status == HL_STATUS_OK);
    }
    HL_CHECK((object.clones) == (1));
    HL_CHECK((hl_linux_abi_fork_parent(&fx.abi, &plan)) == (HL_STATUS_OK));
    HL_CHECK(close_fd(&fx, fd) == 0);
    HL_CHECK(close_fd(&fx, (hl_linux_fd)epoll) == 0);
    return 0;
}

static int test_control_errors_and_epoll_dup(void) {
    test_fixture fx;
    ready_object object = {0};
    hl_linux_epoll_event event;
    hl_linux_fd target;
    hl_linux_fd duplicate;
    int64_t epoll;
    int64_t nested;
    HL_CHECK(fixture_init(&fx) == 0);
    target = install(&fx, &object);
    epoll = hl_linux_epoll_create(&fx.abi, 0);
    HL_CHECK(epoll >= 0);
    nested = hl_linux_epoll_create(&fx.abi, 0);
    HL_CHECK(nested >= 0);
    HL_CHECK(hl_linux_epoll_control(&fx.abi, (hl_linux_fd)epoll, HL_LINUX_EPOLL_ADD, (hl_linux_fd)nested,
                                    HL_LINUX_READY_READ, 9) == -HL_LINUX_EINVAL);
    HL_CHECK(hl_linux_epoll_control(&fx.abi, (hl_linux_fd)epoll, HL_LINUX_EPOLL_MODIFY, target, HL_LINUX_READY_READ,
                                    1) == -HL_LINUX_ENOENT);
    HL_CHECK(hl_linux_epoll_control(&fx.abi, (hl_linux_fd)epoll, HL_LINUX_EPOLL_DELETE, target, 0, 0) ==
             -HL_LINUX_ENOENT);
    HL_CHECK(hl_linux_epoll_control(&fx.abi, (hl_linux_fd)epoll, HL_LINUX_EPOLL_ADD, target, HL_LINUX_READY_READ, 1) ==
             0);
    HL_CHECK(hl_linux_epoll_control(&fx.abi, (hl_linux_fd)epoll, HL_LINUX_EPOLL_ADD, target, HL_LINUX_READY_READ, 2) ==
             -HL_LINUX_EEXIST);
    HL_CHECK(hl_linux_epoll_control(&fx.abi, (hl_linux_fd)epoll, HL_LINUX_EPOLL_ADD, (hl_linux_fd)epoll,
                                    HL_LINUX_READY_READ, 3) == -HL_LINUX_EINVAL);
    HL_CHECK(hl_linux_fd_dup(&fx.abi, (hl_linux_fd)epoll, 0, &duplicate) == HL_STATUS_OK);
    HL_CHECK(close_fd(&fx, (hl_linux_fd)epoll) == 0);
    atomic_store_explicit(&object.ready, 1, memory_order_release);
    HL_CHECK(hl_linux_epoll_wait(&fx.abi, duplicate, &event, 1, 0) == 1 && event.data == 1);
    HL_CHECK(close_fd(&fx, duplicate) == 0);
    HL_CHECK(close_fd(&fx, (hl_linux_fd)nested) == 0);
    HL_CHECK(close_fd(&fx, target) == 0);
    return 0;
}

static int test_fork_clone_rollback(void) {
    test_fixture fx;
    ready_object failure = {.clone_status = HL_STATUS_OUT_OF_MEMORY};
    hl_linux_fork_record records[32];
    hl_linux_fork_plan plan = {
        .abi = HL_LINUX_ABI_VERSION, .size = sizeof(plan), .records = records, .capacity = HL_ARRAY_COUNT(records)};
    hl_linux_fd target;
    int64_t epoll;
    HL_CHECK(fixture_init(&fx) == 0);
    fx.host.event = &blocking_events;
    epoll = hl_linux_epoll_create(&fx.abi, 0);
    HL_CHECK(epoll >= 0);
    target = install(&fx, &failure);
    HL_CHECK(hl_linux_abi_fork_prepare(&fx.abi, &plan) == HL_STATUS_OUT_OF_MEMORY);
    HL_CHECK(plan.count == 0 && failure.clones == 1 && fx.event_closes == 1);
    HL_CHECK(hl_linux_abi_validate_fds(&fx.abi) == HL_STATUS_OK);
    HL_CHECK(close_fd(&fx, target) == 0);
    HL_CHECK(close_fd(&fx, (hl_linux_fd)epoll) == 0);
    HL_CHECK(fx.event_closes == 2);
    return 0;
}

typedef struct wait_call {
    test_fixture *fx;
    hl_linux_fd epoll;
    int64_t result;
    hl_linux_epoll_event event;
} wait_call;

static void *wait_thread(void *opaque) {
    wait_call *call = opaque;
    call->result = hl_linux_epoll_wait(&call->fx->abi, call->epoll, &call->event, 1, HL_HOST_DEADLINE_INFINITE);
    return NULL;
}

static int test_fork_clone_while_waiting(void) {
    test_fixture fx;
    hl_linux_fork_record records[32];
    hl_linux_fork_plan plan = {
        .abi = HL_LINUX_ABI_VERSION, .size = sizeof(plan), .records = records, .capacity = HL_ARRAY_COUNT(records)};
    wait_call call;
    pthread_t thread;
    int64_t epoll;
    HL_CHECK(fixture_init(&fx) == 0);
    fx.host.event = &blocking_events;
    epoll = hl_linux_epoll_create(&fx.abi, 0);
    HL_CHECK(epoll >= 0);
    call = (wait_call){.fx = &fx, .epoll = (hl_linux_fd)epoll};
    HL_CHECK(pthread_create(&thread, NULL, wait_thread, &call) == 0);
    while (atomic_load_explicit(&fx.event_waiting, memory_order_acquire) == 0) {}
    HL_CHECK(hl_linux_abi_fork_prepare(&fx.abi, &plan) == HL_STATUS_OK);
    HL_CHECK(hl_linux_abi_fork_parent(&fx.abi, &plan) == HL_STATUS_OK);
    HL_CHECK(close_fd(&fx, (hl_linux_fd)epoll) == 0);
    HL_CHECK(pthread_join(thread, NULL) == 0);
    HL_CHECK(call.result == 0 || call.result == -HL_LINUX_EBADF);
    return 0;
}

static int test_wait_wakes_on_control_and_close(void) {
    test_fixture fx;
    ready_object object = {0};
    wait_call call;
    pthread_t thread;
    hl_linux_fd target;
    int64_t epoll;
    HL_CHECK(fixture_init(&fx) == 0);
    fx.host.event = &blocking_events;
    target = install(&fx, &object);
    epoll = hl_linux_epoll_create(&fx.abi, 0);
    HL_CHECK(epoll >= 0);
    memset(&call, 0, sizeof(call));
    call.fx = &fx;
    call.epoll = (hl_linux_fd)epoll;
    HL_CHECK(hl_linux_epoll_control(&fx.abi, (hl_linux_fd)epoll, HL_LINUX_EPOLL_ADD, target, HL_LINUX_READY_READ, 41) ==
             0);
    HL_CHECK(pthread_create(&thread, NULL, wait_thread, &call) == 0);
    while (atomic_load_explicit(&fx.event_waiting, memory_order_acquire) == 0) {}
    ready_set(&object, 1);
    HL_CHECK(pthread_join(thread, NULL) == 0);
    HL_CHECK(call.result == 1 && call.event.data == 41);
    ready_set(&object, 0);
    atomic_store_explicit(&fx.event_waiting, 0, memory_order_release);
    HL_CHECK(pthread_create(&thread, NULL, wait_thread, &call) == 0);
    while (atomic_load_explicit(&fx.event_waiting, memory_order_acquire) == 0) {}
    HL_CHECK(close_fd(&fx, (hl_linux_fd)epoll) == 0);
    HL_CHECK(pthread_join(thread, NULL) == 0);
    HL_CHECK(call.result == -HL_LINUX_EBADF);
    HL_CHECK(close_fd(&fx, target) == 0);
    return 0;
}

typedef struct race_call {
    test_fixture *fx;
    hl_linux_fd epoll;
    hl_linux_fd target;
    int64_t result;
} race_call;

static void *delete_thread(void *opaque) {
    race_call *call = opaque;
    call->result = hl_linux_epoll_control(&call->fx->abi, call->epoll, HL_LINUX_EPOLL_DELETE, call->target, 0, 0);
    return NULL;
}

static void *close_thread(void *opaque) {
    race_call *call = opaque;
    call->result = (int64_t)hl_linux_fd_close(&call->fx->abi, call->target, NULL);
    return NULL;
}

static int test_delete_final_close_wait_race(void) {
    test_fixture fx;
    ready_object object = {0};
    wait_call waiter;
    race_call deletion;
    race_call closing;
    pthread_t wait_id;
    pthread_t delete_id;
    pthread_t close_id;
    hl_linux_fd target;
    int64_t epoll;
    HL_CHECK(fixture_init(&fx) == 0);
    fx.host.event = &blocking_events;
    target = install(&fx, &object);
    epoll = hl_linux_epoll_create(&fx.abi, 0);
    HL_CHECK(epoll >= 0);
    HL_CHECK(hl_linux_epoll_control(&fx.abi, (hl_linux_fd)epoll, HL_LINUX_EPOLL_ADD, target, HL_LINUX_READY_READ, 51) ==
             0);
    waiter = (wait_call){.fx = &fx, .epoll = (hl_linux_fd)epoll};
    deletion = (race_call){.fx = &fx, .epoll = (hl_linux_fd)epoll, .target = target};
    closing = deletion;
    HL_CHECK(pthread_create(&wait_id, NULL, wait_thread, &waiter) == 0);
    while (atomic_load_explicit(&fx.event_waiting, memory_order_acquire) == 0) {}
    HL_CHECK(pthread_create(&delete_id, NULL, delete_thread, &deletion) == 0);
    HL_CHECK(pthread_create(&close_id, NULL, close_thread, &closing) == 0);
    HL_CHECK(pthread_join(delete_id, NULL) == 0);
    HL_CHECK(pthread_join(close_id, NULL) == 0);
    HL_CHECK(deletion.result == 0 || deletion.result == -HL_LINUX_EBADF);
    HL_CHECK(closing.result == HL_STATUS_OK);
    HL_CHECK(close_fd(&fx, (hl_linux_fd)epoll) == 0);
    HL_CHECK(pthread_join(wait_id, NULL) == 0);
    HL_CHECK(waiter.result == -HL_LINUX_EBADF);
    HL_CHECK(object.closes == 1);
    return 0;
}

typedef struct notify_call {
    ready_object *object;
    _Atomic uint32_t stop;
    _Atomic uint32_t iterations;
} notify_call;

static void *notify_thread(void *opaque) {
    notify_call *call = opaque;
    uint32_t value = 0;
    while (atomic_load_explicit(&call->stop, memory_order_acquire) == 0) {
        value ^= 1u;
        ready_set(call->object, value);
        atomic_fetch_add_explicit(&call->iterations, 1, memory_order_release);
    }
    return NULL;
}

static int test_target_close_quiesces_notifications(void) {
    test_fixture fx;
    ready_object object = {0};
    notify_call notifier = {.object = &object};
    wait_call waiter;
    pthread_t notify_id;
    pthread_t wait_id;
    hl_linux_fd target;
    int64_t epoll;
    HL_CHECK(fixture_init(&fx) == 0);
    fx.host.event = &blocking_events;
    target = install(&fx, &object);
    epoll = hl_linux_epoll_create(&fx.abi, 0);
    HL_CHECK(epoll >= 0);
    HL_CHECK(hl_linux_epoll_control(&fx.abi, (hl_linux_fd)epoll, HL_LINUX_EPOLL_ADD, target, HL_LINUX_READY_READ, 61) ==
             0);
    waiter = (wait_call){.fx = &fx, .epoll = (hl_linux_fd)epoll};
    HL_CHECK(pthread_create(&wait_id, NULL, wait_thread, &waiter) == 0);
    while (atomic_load_explicit(&fx.event_waiting, memory_order_acquire) == 0) {}
    HL_CHECK(pthread_create(&notify_id, NULL, notify_thread, &notifier) == 0);
    while (atomic_load_explicit(&notifier.iterations, memory_order_acquire) < 100) {}
    HL_CHECK(close_fd(&fx, target) == 0);
    HL_CHECK(object.closes == 1);
    while (atomic_load_explicit(&notifier.iterations, memory_order_acquire) < 200) {}
    atomic_store_explicit(&notifier.stop, 1, memory_order_release);
    HL_CHECK(pthread_join(notify_id, NULL) == 0);
    HL_CHECK(hl_linux_epoll_control(&fx.abi, (hl_linux_fd)epoll, HL_LINUX_EPOLL_DELETE, target, 0, 0) ==
             -HL_LINUX_EBADF);
    HL_CHECK(close_fd(&fx, (hl_linux_fd)epoll) == 0);
    HL_CHECK(pthread_join(wait_id, NULL) == 0);
    HL_CHECK(waiter.result == 0 || waiter.result == 1 || waiter.result == -HL_LINUX_EBADF);
    return 0;
}

int main(void) {
    HL_CHECK(test_alias_and_reuse() == 0);
    HL_CHECK(test_distinct_aliases_edge_and_oneshot() == 0);
    HL_CHECK(test_fork_clone() == 0);
    HL_CHECK(test_fork_clone_while_waiting() == 0);
    HL_CHECK(test_control_errors_and_epoll_dup() == 0);
    HL_CHECK(test_fork_clone_rollback() == 0);
    HL_CHECK(test_wait_wakes_on_control_and_close() == 0);
    HL_CHECK(test_delete_final_close_wait_race() == 0);
    HL_CHECK(test_target_close_quiesces_notifications() == 0);
    return 0;
}
