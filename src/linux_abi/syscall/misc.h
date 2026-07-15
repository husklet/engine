#ifndef HL_LINUX_ABI_SYSCALL_MISC_H
#define HL_LINUX_ABI_SYSCALL_MISC_H

#include <stddef.h>
#include <stdint.h>

typedef int (*hl_linux_misc_mapped_fn)(void *context, uintptr_t address, size_t size);
typedef void (*hl_linux_misc_random_fn)(void *context, void *output, size_t size);

typedef struct hl_linux_misc_context {
    char *hostname;
    size_t hostname_capacity;
    uint64_t memory_limit;
    uint64_t memory_used;
    const char *machine;
    hl_linux_misc_mapped_fn mapped;
    hl_linux_misc_random_fn random;
    void *callback_context;
} hl_linux_misc_context;

int hl_linux_misc_dispatch(hl_linux_misc_context *context, uint64_t number, const uint64_t arguments[6],
                           int64_t *guest_result);

#endif
