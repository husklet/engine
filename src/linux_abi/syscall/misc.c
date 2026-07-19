#include "misc.h"

#include <errno.h>
#include <string.h>

int hl_linux_misc_dispatch(hl_linux_misc_context *context, uint64_t number, const uint64_t arguments[6],
                           int64_t *guest_result) {
    uint64_t address = arguments[0];
    uint64_t size = arguments[1];
    switch (number) {
    case 160: {
        char *output = (char *)(uintptr_t)address;
        if (!context->mapped(context->callback_context, (uintptr_t)address, 6 * 65)) {
            *guest_result = -EFAULT;
            break;
        }
        memset(output, 0, 6 * 65);
        strcpy(output, "Linux");
        strcpy(output + 65, context->hostname[0] ? context->hostname : "jit");
        strcpy(output + 130, "6.1.0");
        strcpy(output + 195, "#1 jit");
        strcpy(output + 260, context->machine);
        *guest_result = 0;
        break;
    }
    case 161: {
        int length = (int)size;
        if (length > 64) length = 64;
        if (length > 0) {
            if (!context->mapped(context->callback_context, (uintptr_t)address, (size_t)length)) {
                *guest_result = -EFAULT;
                break;
            }
            memcpy(context->hostname, (const void *)(uintptr_t)address, (size_t)length);
            context->hostname[length < (int)context->hostname_capacity ? length :
                                                                        (int)context->hostname_capacity - 1] = 0;
        }
        *guest_result = 0;
        break;
    }
    case 162:
        *guest_result = 0;
        break;
    case 179: {
        char *output = (char *)(uintptr_t)address;
        uint64_t total;
        uint64_t free_memory;
        if (!context->mapped(context->callback_context, (uintptr_t)address, 112)) {
            *guest_result = -EFAULT;
            break;
        }
        memset(output, 0, 112);
        total = context->memory_limit ? context->memory_limit : UINT64_C(8) << 30;
        free_memory = total > context->memory_used ? total - context->memory_used : total / 4;
        memcpy(output + 0, &(uint64_t){3600}, sizeof(uint64_t));
        memcpy(output + 32, &total, sizeof(total));
        memcpy(output + 40, &free_memory, sizeof(free_memory));
        memcpy(output + 80, &(uint16_t){64}, sizeof(uint16_t));
        memcpy(output + 104, &(uint32_t){1}, sizeof(uint32_t));
        *guest_result = 0;
        break;
    }
    case 278: {
        // Validate flags exactly as Linux (drivers/char/random.c): only GRND_NONBLOCK(1) | GRND_RANDOM(2) |
        // GRND_INSECURE(4) are defined, and GRND_RANDOM|GRND_INSECURE together is invalid. Any other bit ->
        // EINVAL (previously an unknown flag such as 0x10 wrongly succeeded).
        uint64_t flags = arguments[2];
        if ((flags & ~(uint64_t)0x7u) || (flags & 0x2u && flags & 0x4u)) {
            *guest_result = -EINVAL;
            break;
        }
        if (!context->mapped(context->callback_context, (uintptr_t)address, (size_t)size)) {
            *guest_result = -EFAULT;
            break;
        }
        context->random(context->callback_context, (void *)(uintptr_t)address, (size_t)size);
        *guest_result = (int64_t)size;
        break;
    }
    case 293:
        *guest_result = -ENOSYS;
        break;
    default:
        return 0;
    }
    return 1;
}
