#include "guest_memory.h"

static const hl_guest_memory_ops *g_ops;

void hl_guest_memory_bind(const hl_guest_memory_ops *ops) {
    g_ops = ops;
}

int hl_guest_memory_resolve_exec(uint64_t guest, size_t length, const void **host, size_t *contiguous) {
    if (g_ops == NULL || g_ops->resolve_exec == NULL) return 0;
    return g_ops->resolve_exec(guest, length, host, contiguous);
}

int hl_guest_memory_read(uint64_t guest, void *destination, size_t length) {
    if (g_ops == NULL || g_ops->read == NULL) return 0;
    return g_ops->read(guest, destination, length);
}

int hl_guest_memory_write(uint64_t guest, const void *source, size_t length) {
    if (g_ops == NULL || g_ops->write == NULL) return 0;
    return g_ops->write(guest, source, length);
}

int hl_guest_memory_indirect(void) {
    return g_ops != NULL && g_ops->indirect != NULL && g_ops->indirect();
}

uint64_t hl_guest_memory_host_pointer(uint64_t guest) {
    return g_ops != NULL && g_ops->host_pointer != NULL ? g_ops->host_pointer(guest) : guest;
}
