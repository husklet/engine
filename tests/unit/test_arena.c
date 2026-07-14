#include "test.h"

#include "../../src/translator/arena.h"

typedef struct fake_arena {
    uint32_t flags;
    uint32_t preserve;
    hl_host_handle released;
    uint64_t size;
    uint64_t alignment;
} fake_arena;

static hl_host_result reserve_code(void *context, uint64_t size, uint64_t alignment, uint32_t flags,
                                   hl_host_code_mapping *mapping) {
    fake_arena *fake = context;
    fake->flags = flags;
    fake->size = size;
    fake->alignment = alignment;
    mapping->handle = 9;
    mapping->writable_address = 0x1000;
    mapping->executable_address = flags != 0 ? 0x3000 : 0x1000;
    mapping->mapped_size = size;
    return (hl_host_result){.status = HL_STATUS_OK};
}

static hl_host_result repair(void *context, hl_host_code_mapping *mapping, uint32_t preserve) {
    fake_arena *fake = context;
    fake->preserve = preserve;
    if (!preserve) {
        mapping->writable_address = 0x5000;
        mapping->executable_address = 0x5000;
    }
    return (hl_host_result){.status = HL_STATUS_OK};
}

static hl_host_result release(void *context, hl_host_handle handle) {
    ((fake_arena *)context)->released = handle;
    return (hl_host_result){.status = HL_STATUS_OK};
}

int main(void) {
    fake_arena fake = {0};
    hl_host_memory_services memory = {0};
    hl_host_services services = {0};
    hl_host_code_mapping mapping;
    hl_emit_state state = {0};

    memory.reserve_code = reserve_code;
    memory.repair_code_after_fork = repair;
    memory.release = release;
    services.context = &fake;
    services.memory = &memory;

    HL_CHECK(hl_arena_reserve(&services, 4096, 64, 1, &mapping) == 0);
    HL_CHECK(fake.flags == HL_HOST_CODE_DUAL_ALIAS);
    HL_CHECK(fake.size == 4096);
    HL_CHECK(fake.alignment == 64);
    hl_arena_bind(&state, &mapping);
    HL_CHECK(state.base == (uint8_t *)(uintptr_t)0x1000);
    HL_CHECK(state.cursor == state.base);
    HL_CHECK(state.rx_delta == 0x2000);
    HL_CHECK(state.dual_alias == 1);

    HL_CHECK(hl_arena_repair(&services, &state, 0) == 0);
    HL_CHECK(fake.preserve == 0);
    HL_CHECK(state.base == (uint8_t *)(uintptr_t)0x5000);
    HL_CHECK(state.rx_delta == 0);
    HL_CHECK(state.dual_alias == 1);

    hl_arena_release(&services, state.mapping.handle);
    HL_CHECK(fake.released == 9);
    return EXIT_SUCCESS;
}
