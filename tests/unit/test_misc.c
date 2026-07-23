#include "test.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "../../src/linux_abi/syscall/misc.h"

typedef struct fixture {
    int allow_mapping;
    uint8_t random_byte;
} fixture;

static int mapped(void *context, uintptr_t address, size_t size) {
    fixture *state = context;
    return state->allow_mapping && (address != 0 || size == 0);
}

static void random_bytes(void *context, void *output, size_t size) {
    fixture *state = context;
    memset(output, state->random_byte, size);
}

static uint64_t load_u64(const uint8_t *bytes) {
    uint64_t value;
    memcpy(&value, bytes, sizeof(value));
    return value;
}

int main(void) {
    fixture state = {1, 0xa5};
    char hostname[65] = {0};
    hl_linux_misc_context context = {
        .hostname = hostname,
        .hostname_capacity = sizeof(hostname),
        .memory_limit = 1024,
        .memory_used = 256,
        .process_count = 64,
        .machine = "aarch64",
        .mapped = mapped,
        .random = random_bytes,
        .callback_context = &state,
    };
    uint64_t arguments[6] = {0};
    int64_t result = 91;

    HL_CHECK(hl_linux_misc_dispatch(&context, 9999, arguments, &result) == 0 && result == 91);

    {
        uint8_t output[392];
        memset(output, 0xcc, sizeof(output));
        arguments[0] = (uint64_t)(uintptr_t)(output + 1);
        HL_CHECK(hl_linux_misc_dispatch(&context, 160, arguments, &result) == 1 && result == 0);
        HL_CHECK(output[0] == 0xcc && output[391] == 0xcc);
        HL_CHECK(strcmp((char *)output + 1, "Linux") == 0);
        HL_CHECK(strcmp((char *)output + 66, "jit") == 0);
        HL_CHECK(strcmp((char *)output + 131, "6.1.0") == 0);
        HL_CHECK(strcmp((char *)output + 196, "#1 jit") == 0);
        HL_CHECK(strcmp((char *)output + 261, "aarch64") == 0);
        strcpy(hostname, "box");
        HL_CHECK(hl_linux_misc_dispatch(&context, 160, arguments, &result) == 1 && result == 0);
        HL_CHECK(strcmp((char *)output + 66, "box") == 0);

        state.allow_mapping = 0;
        memset(output, 0x7b, sizeof(output));
        HL_CHECK(hl_linux_misc_dispatch(&context, 160, arguments, &result) == 1 && result == -EFAULT);
        for (size_t index = 0; index < sizeof(output); ++index)
            HL_CHECK(output[index] == 0x7b);
        state.allow_mapping = 1;
    }

    {
        const char name[] = "container-name";
        arguments[0] = (uint64_t)(uintptr_t)name;
        arguments[1] = sizeof(name) - 1;
        HL_CHECK(hl_linux_misc_dispatch(&context, 161, arguments, &result) == 1 && result == 0);
        HL_CHECK(strcmp(hostname, name) == 0);
        state.allow_mapping = 0;
        arguments[0] = (uint64_t)(uintptr_t)"changed";
        arguments[1] = 7;
        HL_CHECK(hl_linux_misc_dispatch(&context, 161, arguments, &result) == 1 && result == -EFAULT);
        HL_CHECK(strcmp(hostname, name) == 0);
        state.allow_mapping = 1;
    }

    {
        uint8_t info[114];
        memset(info, 0xdd, sizeof(info));
        arguments[0] = (uint64_t)(uintptr_t)(info + 1);
        arguments[1] = 0;
        HL_CHECK(hl_linux_misc_dispatch(&context, 179, arguments, &result) == 1 && result == 0);
        HL_CHECK(info[0] == 0xdd && info[113] == 0xdd);
        HL_CHECK(load_u64(info + 1) == 3600);
        HL_CHECK(load_u64(info + 33) == 1024);
        HL_CHECK(load_u64(info + 41) == 768);
        HL_CHECK(info[81] == 64 && info[105] == 1);
        context.memory_used = 2048;
        HL_CHECK(hl_linux_misc_dispatch(&context, 179, arguments, &result) == 1 && result == 0);
        // Usage above the cgroup limit reports freeram == 0 (a cap cannot show
        // negative/quarter-of-total free memory), matching /proc/meminfo MemFree.
        HL_CHECK(load_u64(info + 41) == 0);
    }

    {
        uint8_t output[8] = {0};
        arguments[0] = (uint64_t)(uintptr_t)output;
        arguments[1] = sizeof(output);
        HL_CHECK(hl_linux_misc_dispatch(&context, 278, arguments, &result) == 1 && result == (int64_t)sizeof(output));
        for (size_t index = 0; index < sizeof(output); ++index)
            HL_CHECK(output[index] == 0xa5);
        state.allow_mapping = 0;
        memset(output, 0x31, sizeof(output));
        HL_CHECK(hl_linux_misc_dispatch(&context, 278, arguments, &result) == 1 && result == -EFAULT);
        for (size_t index = 0; index < sizeof(output); ++index)
            HL_CHECK(output[index] == 0x31);
    }

    HL_CHECK(hl_linux_misc_dispatch(&context, 162, arguments, &result) == 1 && result == 0);
    HL_CHECK(hl_linux_misc_dispatch(&context, 293, arguments, &result) == 1 && result == -ENOSYS);
    return 0;
}
