#define _POSIX_C_SOURCE 200809L
#include "../../src/core/provider_demux.h"
#include "test.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { HEADER = 32, REQUEST = 3, REPLY = 4, CANCEL = 5, EVENT = 11 };

static void p16(unsigned char *p, uint16_t v) {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

static void p32(unsigned char *p, uint32_t v) {
    p16(p, (uint16_t)v);
    p16(p + 2, (uint16_t)(v >> 16));
}

static void p64(unsigned char *p, uint64_t v) {
    p32(p, (uint32_t)v);
    p32(p + 4, (uint32_t)(v >> 32));
}

static uint16_t g16(const unsigned char *p) {
    return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}

static uint32_t g32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t g64(const unsigned char *p) {
    return (uint64_t)g32(p) | (uint64_t)g32(p + 4) << 32;
}

static int io(int fd, void *p, size_t n, int write_it) {
    size_t u = 0;
    while (u < n) {
        ssize_t r = write_it ? write(fd, (char *)p + u, n - u) : read(fd, (char *)p + u, n - u);
        if (r <= 0) return -1;
        u += (size_t)r;
    }
    return 0;
}

static int recv_frame(int fd, uint16_t *kind, uint64_t *id, unsigned char *payload) {
    unsigned char h[HEADER];
    if (io(fd, h, sizeof(h), 0)) return -1;
    *kind = g16(h + 6);
    *id = g64(h + 12);
    return io(fd, payload, g32(h + 8), 0);
}

static int send_frame(int fd, uint16_t kind, uint64_t id, const char *payload) {
    unsigned char h[HEADER];
    uint32_t n = (uint32_t)strlen(payload);
    memset(h, 0, sizeof(h));
    p32(h, UINT32_C(0x484c5052));
    p16(h + 4, 1);
    p16(h + 6, kind);
    p32(h + 8, n);
    p64(h + 12, id);
    return io(fd, h, sizeof(h), 1) || io(fd, (void *)(uintptr_t)payload, n, 1);
}

typedef struct server_state {
    int fd;
    uint64_t first;
    uint64_t second;
} server_state;

static void *server(void *opaque) {
    server_state *s = opaque;
    unsigned char p[32];
    uint16_t kind;
    uint64_t id;
    if (recv_frame(s->fd, &kind, &s->first, p) || kind != REQUEST) return (void *)1;
    if (recv_frame(s->fd, &kind, &s->second, p) || kind != REQUEST) return (void *)2;
    if (send_frame(s->fd, EVENT, 77, "e1") || send_frame(s->fd, REPLY, s->second, "two") ||
        send_frame(s->fd, EVENT, 77, "e2") || send_frame(s->fd, EVENT, 77, "dropped") ||
        send_frame(s->fd, REPLY, s->first, "one"))
        return (void *)3;
    if (recv_frame(s->fd, &kind, &id, p) || kind != REQUEST) return (void *)4;
    if (recv_frame(s->fd, &kind, &id, p) || kind != CANCEL) return (void *)5;
    if (send_frame(s->fd, REPLY, id, "late")) return (void *)6;
    if (recv_frame(s->fd, &kind, &id, p) || kind != REQUEST) return (void *)7;
    if (recv_frame(s->fd, &kind, &id, p) || kind != CANCEL) return (void *)8;
    close(s->fd);
    return NULL;
}

static void wake(void *opaque, uint64_t id) {
    _Atomic unsigned *count = opaque;
    if (id == 77) atomic_fetch_add(count, 1);
}

typedef struct waiting {
    hl_provider_demux *d;
    hl_provider_ticket t;
    hl_provider_reply reply;
    int status;
} waiting;

static void *waiter(void *opaque) {
    waiting *w = opaque;
    w->status = hl_provider_demux_wait(w->d, w->t, 1000, &w->reply);
    return NULL;
}

static void *fork_server(void *opaque) {
    int fd = *(int *)opaque;
    unsigned char first[16], second[16];
    uint16_t kind;
    uint64_t first_id, second_id;
    const char *first_reply, *second_reply;
    if (recv_frame(fd, &kind, &first_id, first) != 0 || kind != REQUEST) return (void *)1;
    if (recv_frame(fd, &kind, &second_id, second) != 0 || kind != REQUEST) return (void *)2;
    first_reply = memcmp(first, "child", 6) == 0 ? "child" : "parent";
    second_reply = memcmp(second, "child", 6) == 0 ? "child" : "parent";
    if (send_frame(fd, REPLY, second_id, second_reply) != 0 || send_frame(fd, REPLY, first_id, first_reply) != 0)
        return (void *)3;
    return NULL;
}

static int fork_broker(void) {
    int pair[2], status;
    pthread_t peer;
    pid_t child;
    void *result = (void *)99;
    hl_provider_demux *demux = NULL;
    hl_provider_ticket ticket;
    hl_provider_reply reply;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0 || pthread_create(&peer, NULL, fork_server, &pair[1]) != 0 ||
        hl_provider_demux_create(&demux, pair[0], 32, 4, 1, 1) != 0)
        return 1;
    child = fork();
    if (child < 0) return 2;
    if (child == 0) {
        close(pair[1]);
        if (hl_provider_demux_begin(demux, "child", 6, &ticket) != 0 ||
            hl_provider_demux_wait(demux, ticket, 1000, &reply) != 0 || reply.size != 5 ||
            memcmp(reply.bytes, "child", 5) != 0)
            _exit(3);
        hl_provider_reply_destroy(&reply);
        _exit(0);
    }
    if (hl_provider_demux_begin(demux, "parent", 7, &ticket) != 0 ||
        hl_provider_demux_wait(demux, ticket, 1000, &reply) != 0 || reply.size != 6 ||
        memcmp(reply.bytes, "parent", 6) != 0)
        return 4;
    hl_provider_reply_destroy(&reply);
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
        pthread_join(peer, &result) != 0 || result != NULL)
        return 5;
    hl_provider_demux_destroy(demux);
    close(pair[0]);
    close(pair[1]);
    return 0;
}

static void *death_server(void *opaque) {
    int fd = *(int *)opaque;
    unsigned char payload[16];
    uint16_t kind;
    uint64_t id;
    if (recv_frame(fd, &kind, &id, payload) != 0 || kind != REQUEST) return (void *)1;
    if (recv_frame(fd, &kind, &id, payload) != 0 || kind != REQUEST) return (void *)2;
    close(fd);
    return NULL;
}

static int fork_peer_death(void) {
    int pair[2], status;
    pthread_t peer;
    pid_t child;
    void *result = (void *)99;
    hl_provider_demux *demux = NULL;
    hl_provider_ticket ticket;
    hl_provider_reply reply;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0 || pthread_create(&peer, NULL, death_server, &pair[1]) != 0 ||
        hl_provider_demux_create(&demux, pair[0], 32, 2, 1, 1) != 0)
        return 1;
    child = fork();
    if (child < 0) return 2;
    if (child == 0) {
        close(pair[1]);
        if (hl_provider_demux_begin(demux, "child", 6, &ticket) != 0 ||
            hl_provider_demux_wait(demux, ticket, 1000, &reply) != -ECONNRESET)
            _exit(3);
        _exit(0);
    }
    if (hl_provider_demux_begin(demux, "parent", 7, &ticket) != 0 ||
        hl_provider_demux_wait(demux, ticket, 1000, &reply) != -ECONNRESET)
        return 4;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
        pthread_join(peer, &result) != 0 || result != NULL)
        return 5;
    hl_provider_demux_destroy(demux);
    close(pair[0]);
    return 0;
}

typedef struct pid_wake_state {
    pid_t expected;
    _Atomic unsigned calls;
    _Atomic unsigned wrong;
} pid_wake_state;

static void pid_wake(void *opaque, uint64_t id) {
    pid_wake_state *state = opaque;
    if (id != 91 || getpid() != state->expected) atomic_store(&state->wrong, 1);
    atomic_fetch_add(&state->calls, 1);
}

static int fork_subscriptions(void) {
    int pair[2], sync_pipe[2], status;
    pid_t child;
    hl_provider_demux *demux = NULL;
    hl_provider_event event;
    uint64_t lost;
    pid_wake_state parent_wake = {.expected = getpid()};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0 || pipe(sync_pipe) != 0 ||
        hl_provider_demux_create(&demux, pair[0], 32, 2, 2, 2) != 0 ||
        hl_provider_demux_subscribe(demux, 91, pid_wake, &parent_wake) != 0)
        return 1;
    child = fork();
    if (child < 0) return 2;
    if (child == 0) {
        char ready = 1;
        pid_wake_state child_wake = {.expected = getpid()};
        close(sync_pipe[0]);
        close(pair[1]);
        if (hl_provider_demux_subscribe(demux, 91, pid_wake, &child_wake) != 0 ||
            write(sync_pipe[1], &ready, 1) != 1)
            _exit(3);
        for (int i = 0; i < 1000 && atomic_load(&child_wake.calls) == 0; ++i) {
            struct timespec pause = {0, 1000000L};
            nanosleep(&pause, NULL);
        }
        if (atomic_load(&child_wake.calls) != 1 || atomic_load(&child_wake.wrong) != 0 ||
            hl_provider_demux_next(demux, 91, &event, &lost) != 0 || lost != 0 || event.size != 4 ||
            memcmp(event.bytes, "fork", 4) != 0)
            _exit(4);
        hl_provider_event_destroy(&event);
        if (hl_provider_demux_unsubscribe(demux, 91) != 0) _exit(5);
        _exit(0);
    }
    close(sync_pipe[1]);
    char ready;
    if (read(sync_pipe[0], &ready, 1) != 1 || send_frame(pair[1], EVENT, 91, "fork") != 0) return 6;
    for (int i = 0; i < 1000 && atomic_load(&parent_wake.calls) == 0; ++i) {
        struct timespec pause = {0, 1000000L};
        nanosleep(&pause, NULL);
    }
    if (atomic_load(&parent_wake.calls) != 1 || atomic_load(&parent_wake.wrong) != 0 ||
        hl_provider_demux_next(demux, 91, &event, &lost) != 0 || lost != 0 || event.size != 4 ||
        memcmp(event.bytes, "fork", 4) != 0)
        return 7;
    hl_provider_event_destroy(&event);
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
        hl_provider_demux_unsubscribe(demux, 91) != 0)
        return 8;
    hl_provider_demux_destroy(demux);
    close(pair[0]);
    close(pair[1]);
    close(sync_pipe[0]);
    return 0;
}

static int dead_owner_reclaim(void) {
    int pair[2], status;
    pid_t child;
    hl_provider_demux *demux = NULL;
    pid_wake_state wake_state = {.expected = getpid()};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0 ||
        hl_provider_demux_create(&demux, pair[0], 16, 1, 1, 1) != 0)
        return 1;
    child = fork();
    if (child < 0) return 2;
    if (child == 0) {
        pid_wake_state child_wake = {.expected = getpid()};
        _exit(hl_provider_demux_subscribe(demux, 92, pid_wake, &child_wake) == 0 ? 0 : 3);
    }
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
        hl_provider_demux_subscribe(demux, 92, pid_wake, &wake_state) != 0 ||
        hl_provider_demux_unsubscribe(demux, 92) != 0)
        return 4;
    hl_provider_demux_destroy(demux);
    close(pair[0]);
    close(pair[1]);
    return 0;
}

static int dead_waiter_teardown(void) {
    int pair[2], ready[2], status;
    pid_t child;
    hl_provider_demux *demux = NULL;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0 || pipe(ready) != 0 ||
        hl_provider_demux_create(&demux, pair[0], 16, 2, 1, 1) != 0)
        return 1;
    child = fork();
    if (child < 0) return 2;
    if (child == 0) {
        hl_provider_ticket ticket;
        hl_provider_reply reply;
        close(ready[0]);
        if (hl_provider_demux_begin(demux, "hang", 4, &ticket) != 0 || write(ready[1], "x", 1) != 1)
            _exit(3);
        (void)hl_provider_demux_wait(demux, ticket, 30000, &reply);
        _exit(4);
    }
    close(ready[1]);
    char marker;
    if (read(ready[0], &marker, 1) != 1 || kill(child, SIGKILL) != 0 ||
        waitpid(child, &status, 0) != child || !WIFSIGNALED(status))
        return 5;
    hl_provider_demux_destroy(demux);
    close(pair[0]);
    close(pair[1]);
    close(ready[0]);
    return 0;
}

int main(void) {
    int pair[2];
    server_state state;
    pthread_t peer, a, b;
    hl_provider_demux *d = NULL;
    hl_provider_ticket t1, t2, tc, tt, extra;
    waiting w1, w2;
    hl_provider_event event;
    hl_provider_reply reply;
    uint64_t lost;
    _Atomic unsigned wakes = 0;
    void *result = (void *)99;
    HL_CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    state.fd = pair[1];
    HL_CHECK(pthread_create(&peer, NULL, server, &state) == 0);
    HL_CHECK(hl_provider_demux_create(&d, pair[0], 32, 2, 1, 2) == 0);
    HL_CHECK(hl_provider_demux_subscribe(d, 77, wake, &wakes) == 0);
    HL_CHECK(hl_provider_demux_subscribe(d, 77, wake, &wakes) == -EEXIST);
    HL_CHECK(hl_provider_demux_subscribe(d, 78, wake, &wakes) == -ENOSPC);
    HL_CHECK(hl_provider_demux_begin(d, "a", 1, &t1) == 0 && hl_provider_demux_begin(d, "b", 1, &t2) == 0);
    HL_CHECK(hl_provider_demux_begin(d, "full", 4, &extra) == -ENOSPC);
    memset(&w1, 0, sizeof(w1));
    memset(&w2, 0, sizeof(w2));
    w1.d = d;
    w1.t = t1;
    w2.d = d;
    w2.t = t2;
    HL_CHECK(pthread_create(&a, NULL, waiter, &w1) == 0 && pthread_create(&b, NULL, waiter, &w2) == 0);
    HL_CHECK(pthread_join(a, NULL) == 0 && pthread_join(b, NULL) == 0);
    HL_CHECK(w1.status == 0 && w1.reply.size == 3 && memcmp(w1.reply.bytes, "one", 3) == 0);
    HL_CHECK(w2.status == 0 && w2.reply.size == 3 && memcmp(w2.reply.bytes, "two", 3) == 0);
    hl_provider_reply_destroy(&w1.reply);
    hl_provider_reply_destroy(&w2.reply);
    HL_CHECK(hl_provider_demux_next(d, 77, &event, &lost) == 0 && lost == 1 && event.size == 2 &&
             memcmp(event.bytes, "e1", 2) == 0);
    hl_provider_event_destroy(&event);
    HL_CHECK(hl_provider_demux_next(d, 77, &event, &lost) == 0 && lost == 0 && event.size == 2 &&
             memcmp(event.bytes, "e2", 2) == 0);
    hl_provider_event_destroy(&event);
    for (int i = 0; i < 100 && atomic_load(&wakes) < 3; ++i) {
        struct timespec pause = {0, 1000000L};
        nanosleep(&pause, NULL);
    }
    HL_CHECK(atomic_load(&wakes) >= 3);
    HL_CHECK(hl_provider_demux_begin(d, "cancel", 6, &tc) == 0);
    HL_CHECK(hl_provider_demux_cancel(d, tc) == 0);
    HL_CHECK(hl_provider_demux_wait(d, tc, 1000, &reply) == -ECANCELED);
    HL_CHECK(hl_provider_demux_begin(d, "timeout", 7, &tt) == 0);
    HL_CHECK(hl_provider_demux_wait(d, tt, 20, &reply) == -ETIMEDOUT);
    HL_CHECK(pthread_join(peer, &result) == 0 && result == NULL);
    /* Peer death wakes subscriptions and is visible after queued events drain. */
    for (int i = 0; i < 100 && atomic_load(&wakes) < 4; i++) {
        struct timespec pause = {0, 1000000L};
        nanosleep(&pause, NULL);
    }
    HL_CHECK(hl_provider_demux_next(d, 77, &event, &lost) == -ECONNRESET);
    HL_CHECK(hl_provider_demux_unsubscribe(d, 77) == 0);
    hl_provider_demux_destroy(d);
    close(pair[0]);
    HL_CHECK(fork_broker() == 0);
    HL_CHECK(fork_peer_death() == 0);
    HL_CHECK(fork_subscriptions() == 0);
    HL_CHECK(dead_owner_reclaim() == 0);
    return dead_waiter_teardown();
}
