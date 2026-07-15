#ifndef HL_LINUX_ABI_FORK_WIRE_H
#define HL_LINUX_ABI_FORK_WIRE_H

#include <stddef.h>

int hl_fork_wire_send_descriptors(int socket, const void *buffer, size_t size, const int *descriptors,
                                  int descriptor_count);
int hl_fork_wire_receive_descriptors(int socket, void *buffer, size_t size, int *descriptors,
                                     int *descriptor_count);
int hl_fork_wire_send(int socket, const void *buffer, size_t size);
int hl_fork_wire_receive(int socket, void *buffer, size_t size);
int hl_fork_wire_pack_strings(char *output, size_t capacity, size_t *offset, int count,
                              char *const strings[]);
int hl_fork_wire_unpack_strings(const char *input, size_t size, size_t *offset, char **strings,
                                int capacity);

#endif
