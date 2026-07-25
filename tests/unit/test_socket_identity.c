#include "test.h"
#include "../../src/linux_abi/container/socket_identity.h"

#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

enum { CLIENT_COUNT = 8 };

struct client {
    pthread_t thread;
    const char *listener_path;
    uint64_t object;
    uint64_t peer_object;
    unsigned char payload;
    int result;
};

static int unix_address(struct sockaddr_un *address, const char *path) {
    size_t length = strlen(path);
    if (length >= sizeof address->sun_path) return -1;
    memset(address, 0, sizeof *address);
    address->sun_family = AF_UNIX;
    memcpy(address->sun_path, path, length + 1);
    return 0;
}

static void *connect_client(void *argument) {
    struct client *client = argument;
    char path[HL_SOCKET_IDENTITY_PATH_SIZE];
    struct sockaddr_un local, listener;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 || hl_socket_identity_format(path, sizeof path, client->object, client->peer_object) != 0 ||
        unix_address(&local, path) != 0 || unix_address(&listener, client->listener_path) != 0) {
        client->result = 1;
        return NULL;
    }
    unlink(path);
    if (bind(fd, (struct sockaddr *)&local, sizeof local) != 0) {
        close(fd);
        client->result = 1;
        return NULL;
    }
    unlink(path);
    unsigned char reply = 0;
    if (connect(fd, (struct sockaddr *)&listener, sizeof listener) != 0 || send(fd, &client->payload, 1, 0) != 1 ||
        recv(fd, &reply, 1, 0) != 1 || reply != client->payload)
        client->result = 1;
    close(fd);
    return NULL;
}

int main(void) {
    char listener_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    snprintf(listener_path, sizeof listener_path, "/tmp/.hl-si-test-%ld", (long)getpid());
    struct sockaddr_un listener_address;
    HL_CHECK(unix_address(&listener_address, listener_path) == 0);
    int listener = socket(AF_UNIX, SOCK_STREAM, 0);
    HL_CHECK(listener >= 0);
    unlink(listener_path);
    HL_CHECK(bind(listener, (struct sockaddr *)&listener_address, sizeof listener_address) == 0);
    HL_CHECK(listen(listener, CLIENT_COUNT) == 0);

    struct client clients[CLIENT_COUNT] = {0};
    for (unsigned i = 0; i < CLIENT_COUNT; ++i) {
        clients[i].listener_path = listener_path;
        clients[i].object = UINT64_C(0x1000) + i;
        clients[i].peer_object = UINT64_C(0x2000) + i;
        clients[i].payload = (unsigned char)('a' + i);
        HL_CHECK(pthread_create(&clients[i].thread, NULL, connect_client, &clients[i]) == 0);
    }

    uint64_t seen[CLIENT_COUNT] = {0};
    for (unsigned i = 0; i < CLIENT_COUNT; ++i) {
        int fd = accept(listener, NULL, NULL);
        HL_CHECK(fd >= 0);
        struct sockaddr_un peer;
        socklen_t peer_length = sizeof peer;
        HL_CHECK(getpeername(fd, (struct sockaddr *)&peer, &peer_length) == 0);
        uint64_t object = 0, peer_object = 0;
        HL_CHECK(hl_socket_identity_parse(peer.sun_path, &object, &peer_object) == 0);
        HL_CHECK(object >= UINT64_C(0x1000) && object < UINT64_C(0x1000) + CLIENT_COUNT);
        unsigned index = (unsigned)(object - UINT64_C(0x1000));
        HL_CHECK(peer_object == UINT64_C(0x2000) + index);
        HL_CHECK(seen[index] == 0);
        seen[index] = peer_object;

        // The first byte on the accepted stream is application data; identity never consumes stream space.
        unsigned char bytes[64] = {0};
        HL_CHECK(recv(fd, bytes, sizeof bytes, 0) == 1);
        HL_CHECK(bytes[0] == clients[index].payload);
        HL_CHECK(send(fd, bytes, 1, 0) == 1);
        close(fd);
    }
    for (unsigned i = 0; i < CLIENT_COUNT; ++i) {
        HL_CHECK(pthread_join(clients[i].thread, NULL) == 0);
        HL_CHECK(clients[i].result == 0);
        HL_CHECK(seen[i] == clients[i].peer_object);
    }
    close(listener);
    unlink(listener_path);
    return EXIT_SUCCESS;
}
