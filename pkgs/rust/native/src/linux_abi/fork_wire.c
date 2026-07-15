#include "fork_wire.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define HL_FORK_WIRE_MAX_DESCRIPTORS 8

int hl_fork_wire_send_descriptors(int socket, const void *buffer, size_t size, const int *descriptors,
                                  int descriptor_count) {
    struct iovec vector = {.iov_base = (void *)buffer, .iov_len = size};
    char control[CMSG_SPACE(sizeof(int) * HL_FORK_WIRE_MAX_DESCRIPTORS)] = {0};
    struct msghdr message = {0};
    ssize_t result;
    if (buffer == NULL || size == 0 || descriptor_count < 0 ||
        descriptor_count > HL_FORK_WIRE_MAX_DESCRIPTORS ||
        (descriptor_count > 0 && descriptors == NULL)) {
        errno = EINVAL;
        return -1;
    }
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    if (descriptor_count > 0) {
        struct cmsghdr *header;
        message.msg_control = control;
        message.msg_controllen = (socklen_t)CMSG_SPACE(sizeof(int) * (size_t)descriptor_count);
        header = CMSG_FIRSTHDR(&message);
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = (socklen_t)CMSG_LEN(sizeof(int) * (size_t)descriptor_count);
        memcpy(CMSG_DATA(header), descriptors, sizeof(int) * (size_t)descriptor_count);
    }
    do {
        result = sendmsg(socket, &message, 0);
    } while (result < 0 && errno == EINTR);
    if (result <= 0) return -1;
    if ((size_t)result < size)
        return hl_fork_wire_send(socket, (const char *)buffer + result, size - (size_t)result);
    return 0;
}

int hl_fork_wire_receive_descriptors(int socket, void *buffer, size_t size, int *descriptors,
                                     int *descriptor_count) {
    struct iovec vector = {.iov_base = buffer, .iov_len = size};
    char control[CMSG_SPACE(sizeof(int) * HL_FORK_WIRE_MAX_DESCRIPTORS)];
    struct msghdr message = {0};
    ssize_t result;
    if (buffer == NULL || size == 0 || descriptor_count == NULL || descriptors == NULL) {
        errno = EINVAL;
        return -1;
    }
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof control;
    do {
        result = recvmsg(socket, &message, 0);
    } while (result < 0 && errno == EINTR);
    if (result < 0) return -1;
    *descriptor_count = 0;
    if ((message.msg_flags & MSG_CTRUNC) != 0) {
        /* The kernel may already have installed the rights that fit. Close every complete one visible
           in the truncated buffer before rejecting the message. */
        for (struct cmsghdr *header = CMSG_FIRSTHDR(&message); header != NULL;
             header = CMSG_NXTHDR(&message, header)) {
            if (header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS &&
                header->cmsg_len >= CMSG_LEN(0)) {
                size_t bytes = header->cmsg_len - CMSG_LEN(0);
                int *rights = (int *)CMSG_DATA(header);
                for (size_t index = 0; index < bytes / sizeof(int); index++) (void)close(rights[index]);
            }
        }
        errno = EMSGSIZE;
        return -1;
    }
    for (struct cmsghdr *header = CMSG_FIRSTHDR(&message); header != NULL;
         header = CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS) {
            if (header->cmsg_len < CMSG_LEN(0)) goto invalid_rights;
            size_t bytes = header->cmsg_len - CMSG_LEN(0);
            int count = (int)(bytes / sizeof(int));
            int *rights = (int *)CMSG_DATA(header);
            if (bytes % sizeof(int) != 0 || count > HL_FORK_WIRE_MAX_DESCRIPTORS - *descriptor_count) {
                for (int index = 0; index < count; index++) (void)close(rights[index]);
                goto invalid_rights;
            }
            memcpy(descriptors + *descriptor_count, rights, sizeof(int) * (size_t)count);
            *descriptor_count += count;
        }
    }
    return (int)result;

invalid_rights:
    while (*descriptor_count > 0) (void)close(descriptors[--*descriptor_count]);
    errno = EPROTO;
    return -1;
}

int hl_fork_wire_send(int socket, const void *buffer, size_t size) {
    const char *cursor = (const char *)buffer;
    if (buffer == NULL && size != 0) {
        errno = EINVAL;
        return -1;
    }
    while (size > 0) {
        ssize_t result = send(socket, cursor, size, 0);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) return -1;
        cursor += result;
        size -= (size_t)result;
    }
    return 0;
}

int hl_fork_wire_receive(int socket, void *buffer, size_t size) {
    char *cursor = (char *)buffer;
    if (buffer == NULL && size != 0) {
        errno = EINVAL;
        return -1;
    }
    while (size > 0) {
        ssize_t result = recv(socket, cursor, size, 0);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) return -1;
        cursor += result;
        size -= (size_t)result;
    }
    return 0;
}

int hl_fork_wire_pack_strings(char *output, size_t capacity, size_t *offset, int count,
                              char *const strings[]) {
    int32_t encoded_count;
    if (output == NULL || offset == NULL || count < 0 || (count > 0 && strings == NULL) ||
        *offset > capacity || capacity - *offset < sizeof encoded_count)
        return -1;
    encoded_count = count;
    memcpy(output + *offset, &encoded_count, sizeof encoded_count);
    *offset += sizeof encoded_count;
    for (int index = 0; index < count; index++) {
        size_t length;
        int32_t encoded_length;
        if (strings[index] == NULL) return -1;
        length = strlen(strings[index]) + 1;
        if (length > INT32_MAX || capacity - *offset < sizeof encoded_length ||
            capacity - *offset - sizeof encoded_length < length)
            return -1;
        encoded_length = (int32_t)length;
        memcpy(output + *offset, &encoded_length, sizeof encoded_length);
        *offset += sizeof encoded_length;
        memcpy(output + *offset, strings[index], length);
        *offset += length;
    }
    return 0;
}

int hl_fork_wire_unpack_strings(const char *input, size_t size, size_t *offset, char **strings,
                                int capacity) {
    int32_t count;
    if (input == NULL || offset == NULL || strings == NULL || capacity < 1 || *offset > size ||
        size - *offset < sizeof count)
        return -1;
    memcpy(&count, input + *offset, sizeof count);
    *offset += sizeof count;
    if (count < 0 || count >= capacity) return -1;
    for (int index = 0; index < count; index++) {
        int32_t length;
        if (size - *offset < sizeof length) return -1;
        memcpy(&length, input + *offset, sizeof length);
        *offset += sizeof length;
        if (length < 1 || (size_t)length > size - *offset || input[*offset + (size_t)length - 1] != 0 ||
            memchr(input + *offset, 0, (size_t)length - 1) != NULL)
            return -1;
        strings[index] = (char *)input + *offset;
        *offset += (size_t)length;
    }
    strings[count] = NULL;
    return count;
}
