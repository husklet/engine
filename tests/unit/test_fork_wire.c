#include "test.h"

#include "fork_codec.h"
#include "fork_wire.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

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
    return EXIT_SUCCESS;
}
