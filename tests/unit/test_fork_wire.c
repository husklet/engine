#include "test.h"

#include "fork_codec.h"
#include "fork_wire.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int listener_open(char *path, size_t capacity) {
    struct sockaddr_un address;
    int listener = socket(AF_UNIX, SOCK_STREAM, 0);
    HL_CHECK(listener >= 0);
    HL_CHECK(snprintf(path, capacity, "/tmp/hl-fork-client-%ld.sock", (long)getpid()) > 0);
    (void)unlink(path);
    memset(&address, 0, sizeof address);
    address.sun_family = AF_UNIX;
    HL_CHECK(strlen(path) < sizeof address.sun_path);
    memcpy(address.sun_path, path, strlen(path) + 1);
    HL_CHECK(bind(listener, (struct sockaddr *)&address, sizeof address) == 0);
    HL_CHECK(listen(listener, 1) == 0);
    return listener;
}

int main(void) {
    char storage[128];
    char *input[] = {"guest", "two words", ""};
    char *decoded[4];
    size_t written = 0;
    size_t consumed = 0;
    int sockets[2];
    int pipe_descriptors[2];
    int received[8];
    int received_count = 0;
    char marker = 'M';
    char message[4] = {0};

    HL_CHECK(hl_fork_wire_pack_strings(storage, sizeof storage, &written, 3, input) == 0);
    HL_CHECK(hl_fork_wire_unpack_strings(storage, written, &consumed, decoded, 4) == 3);
    HL_CHECK(consumed == written);
    HL_CHECK(strcmp(decoded[0], input[0]) == 0);
    HL_CHECK(strcmp(decoded[1], input[1]) == 0);
    HL_CHECK(strcmp(decoded[2], input[2]) == 0);
    HL_CHECK(decoded[3] == NULL);

    consumed = 0;
    HL_CHECK(hl_fork_wire_unpack_strings(storage, written - 1, &consumed, decoded, 4) == -1);
    consumed = 0;
    storage[sizeof(int32_t) * 2 - 1] = 0x7f;
    HL_CHECK(hl_fork_wire_unpack_strings(storage, written, &consumed, decoded, 4) == -1);
    {
        int32_t one = 1;
        int32_t length = 4;
        memcpy(storage, &one, sizeof one);
        memcpy(storage + sizeof one, &length, sizeof length);
        memcpy(storage + sizeof one + sizeof length, "a\0b\0", 4);
        consumed = 0;
        HL_CHECK(hl_fork_wire_unpack_strings(storage, 12, &consumed, decoded, 4) == -1);
    }
    written = sizeof storage - 2;
    HL_CHECK(hl_fork_wire_pack_strings(storage, sizeof storage, &written, 3, input) == -1);

    HL_CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    HL_CHECK(pipe(pipe_descriptors) == 0);
    HL_CHECK(hl_fork_wire_send_descriptors(sockets[0], "wire", 4, pipe_descriptors + 1, 1) == 0);
    HL_CHECK(hl_fork_wire_receive_descriptors(sockets[1], message, sizeof message, received, &received_count) == 4);
    HL_CHECK(memcmp(message, "wire", 4) == 0);
    HL_CHECK(received_count == 1);
    HL_CHECK(write(received[0], &marker, 1) == 1);
    marker = 0;
    HL_CHECK(read(pipe_descriptors[0], &marker, 1) == 1);
    HL_CHECK(marker == 'M');

    {
        int extra[9][2];
        int rights[9];
        char control[CMSG_SPACE(sizeof rights)] = {0};
        struct iovec vector = {.iov_base = &marker, .iov_len = 1};
        struct msghdr ancillary = {0};
        struct cmsghdr *header;
        for (int index = 0; index < 9; index++) {
            HL_CHECK(pipe(extra[index]) == 0);
            rights[index] = extra[index][1];
        }
        ancillary.msg_iov = &vector;
        ancillary.msg_iovlen = 1;
        ancillary.msg_control = control;
        ancillary.msg_controllen = sizeof control;
        header = CMSG_FIRSTHDR(&ancillary);
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = CMSG_LEN(sizeof rights);
        memcpy(CMSG_DATA(header), rights, sizeof rights);
        HL_CHECK(sendmsg(sockets[0], &ancillary, 0) == 1);
        received_count = 0;
        HL_CHECK(hl_fork_wire_receive_descriptors(sockets[1], message, sizeof message, received,
                                                  &received_count) == -1);
        HL_CHECK(received_count == 0);
        for (int index = 0; index < 9; index++) {
            HL_CHECK(close(extra[index][0]) == 0);
            HL_CHECK(close(extra[index][1]) == 0);
        }
    }

    HL_CHECK(hl_fork_wire_send_descriptors(sockets[0], NULL, 1, NULL, 0) == -1);
    HL_CHECK(hl_fork_wire_send_descriptors(sockets[0], "x", 1, NULL, 9) == -1);
    HL_CHECK(hl_fork_wire_receive(sockets[0], NULL, 1) == -1);

    HL_CHECK(close(received[0]) == 0);
    HL_CHECK(close(pipe_descriptors[0]) == 0);
    HL_CHECK(close(pipe_descriptors[1]) == 0);
    HL_CHECK(close(sockets[0]) == 0);
    HL_CHECK(close(sockets[1]) == 0);

    {
        char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
        char missing[sizeof path];
        char launch[6] = {0};
        int launch_descriptors[8];
        int launch_descriptor_count = 0;
        int listener = listener_open(path, sizeof path);
        int accepted;
        hl_host_fork_client client = HL_HOST_FORK_CLIENT_INIT;

        HL_CHECK(snprintf(missing, sizeof missing, "%s.missing", path) > 0);
        HL_CHECK(hl_host_fork_client_open(&client, missing) == -1);
        HL_CHECK(client.private_value == 0);

        HL_CHECK(hl_host_fork_client_open(&client, path) == 0);
        accepted = accept(listener, NULL, NULL);
        HL_CHECK(accepted >= 0);
        HL_CHECK(hl_host_fork_client_open(&client, path) == -1);

        HL_CHECK(hl_host_fork_client_send_launch(&client, "launch", 6) == 0);
        HL_CHECK(hl_fork_wire_receive_descriptors(accepted, launch, sizeof launch,
                                                  launch_descriptors,
                                                  &launch_descriptor_count) == 6);
        HL_CHECK(memcmp(launch, "launch", 6) == 0);
        HL_CHECK(launch_descriptor_count == 3);
        for (int index = 0; index < launch_descriptor_count; index++)
            HL_CHECK(close(launch_descriptors[index]) == 0);

        HL_CHECK(hl_fork_wire_send(accepted, "done", 4) == 0);
        memset(launch, 0, sizeof launch);
        HL_CHECK(hl_host_fork_client_receive(&client, launch, 4) == 0);
        HL_CHECK(memcmp(launch, "done", 4) == 0);

        hl_host_fork_client_close(&client);
        HL_CHECK(client.private_value == 0);
        HL_CHECK(read(accepted, launch, 1) == 0);
        hl_host_fork_client_close(&client);
        HL_CHECK(hl_host_fork_client_receive(&client, launch, 1) == -1);

        HL_CHECK(close(accepted) == 0);
        HL_CHECK(close(listener) == 0);
        HL_CHECK(unlink(path) == 0);
    }
    return EXIT_SUCCESS;
}
