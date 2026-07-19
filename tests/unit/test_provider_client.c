#define _POSIX_C_SOURCE 200809L
#include "../../src/core/provider_client.h"
#include "test.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum { HEADER = 32, REQUEST = 3, REPLY = 4, CANCEL = 5, SUBSCRIBE = 9, UNSUBSCRIBE = 10, EVENT = 11 };

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

static int exact(int fd, void *p, size_t n, int writing) {
    size_t used = 0;
    while (used < n) {
        ssize_t count = writing ? write(fd, (char *)p + used, n - used) : read(fd, (char *)p + used, n - used);
        if (count <= 0) return -1;
        used += (size_t)count;
    }
    return 0;
}

static int receive(int fd, uint16_t *kind, uint64_t *id, unsigned char *payload, uint32_t *size) {
    unsigned char h[HEADER];
    if (exact(fd, h, sizeof(h), 0) != 0) return -1;
    *kind = get16(h + 6);
    *id = get64(h + 12);
    *size = get32(h + 8);
    return exact(fd, payload, *size, 0);
}

static int send_frame(int fd, uint16_t kind, uint64_t id, const void *payload, uint32_t size) {
    unsigned char h[HEADER];
    memset(h, 0, sizeof(h));
    put32(h, UINT32_C(0x484c5052));
    put16(h + 4, 1);
    put16(h + 6, kind);
    put32(h + 8, size);
    put64(h + 12, id);
    return exact(fd, h, sizeof(h), 1) || exact(fd, (void *)(uintptr_t)payload, size, 1);
}

static void *server(void *opaque) {
    int fd = *(int *)opaque;
    unsigned char first[16], second[16];
    uint16_t kind;
    uint64_t one, two;
    uint32_t n1, n2;
    if (receive(fd, &kind, &one, first, &n1) != 0 || kind != REQUEST) return (void *)1;
    if (receive(fd, &kind, &two, second, &n2) != 0 || kind != REQUEST) return (void *)2;
    if (send_frame(fd, REPLY, two, second, n2) != 0 || send_frame(fd, REPLY, one, first, n1) != 0) return (void *)3;
    if (receive(fd, &kind, &one, first, &n1) != 0 || kind != SUBSCRIBE || one != 77 || n1 != 4 ||
        memcmp(first, "poll", 4) != 0)
        return (void *)4;
    if (send_frame(fd, EVENT, 77, "ready", 5) != 0) return (void *)5;
    if (receive(fd, &kind, &one, first, &n1) != 0 || kind != UNSUBSCRIBE || one != 77 || n1 != 0) return (void *)6;
    if (receive(fd, &kind, &one, first, &n1) != 0 || kind != REQUEST) return (void *)7;
    if (receive(fd, &kind, &two, second, &n2) != 0 || kind != CANCEL || two != one) return (void *)8;
    return NULL;
}

typedef struct call {
    hl_provider_client *client;
    const char *text;
    int status;
    hl_provider_reply reply;
} call;

static void *request(void *opaque) {
    call *value = opaque;
    value->status =
        hl_provider_client_request(value->client, value->text, (uint32_t)strlen(value->text), 1000, &value->reply);
    return NULL;
}

static void wake(void *opaque, uint64_t subscription) {
    if (subscription == 77) atomic_fetch_add((_Atomic unsigned *)opaque, 1);
}

int main(void) {
    int pair[2];
    pthread_t peer, left, right;
    hl_provider_client client;
    call a, b;
    void *result = (void *)99;
    hl_provider_reply event;
    uint64_t lost;
    _Atomic unsigned wakes = 0;
    struct timespec pause = {0, 1000000L};
    HL_CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    HL_CHECK(pthread_create(&peer, NULL, server, &pair[1]) == 0);
    HL_CHECK(hl_provider_client_init(&client, pair[0], 16) == 0);
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    a.client = &client;
    a.text = "one";
    b.client = &client;
    b.text = "two";
    HL_CHECK(pthread_create(&left, NULL, request, &a) == 0 && pthread_create(&right, NULL, request, &b) == 0);
    HL_CHECK(pthread_join(left, NULL) == 0 && pthread_join(right, NULL) == 0);
    HL_CHECK(a.status == 0 && a.reply.size == 3 && memcmp(a.reply.bytes, "one", 3) == 0);
    HL_CHECK(b.status == 0 && b.reply.size == 3 && memcmp(b.reply.bytes, "two", 3) == 0);
    hl_provider_reply_destroy(&a.reply);
    hl_provider_reply_destroy(&b.reply);
    HL_CHECK(hl_provider_client_subscribe(&client, 77, "poll", 4, wake, &wakes) == 0);
    for (int i = 0; i < 100 && atomic_load(&wakes) == 0; ++i)
        nanosleep(&pause, NULL);
    HL_CHECK(atomic_load(&wakes) == 1);
    HL_CHECK(hl_provider_client_readiness(&client, 77, &event, &lost) == 0 && lost == 0 && event.size == 5 &&
             memcmp(event.bytes, "ready", 5) == 0);
    hl_provider_reply_destroy(&event);
    HL_CHECK(hl_provider_client_unsubscribe(&client, 77) == 0);
    HL_CHECK(hl_provider_client_request(&client, "timeout", 7, 20, &event) == -ETIMEDOUT);
    HL_CHECK(pthread_join(peer, &result) == 0 && result == NULL);
    hl_provider_client_destroy(&client);
    close(pair[0]);
    close(pair[1]);
    return 0;
}
