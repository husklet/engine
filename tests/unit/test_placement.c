#include "test.h"

#include "../../src/linux_abi/placement.h"

typedef struct map_fixture {
    void *results[2];
    void *addresses[2];
    size_t lengths[2];
    uint32_t placements[2];
    uint32_t calls;
} map_fixture;

static void *map_record(void *opaque, void *address, size_t length, uint32_t placement) {
    map_fixture *fixture = opaque;
    uint32_t call = fixture->calls++;
    if (call >= 2) return NULL;
    fixture->addresses[call] = address;
    fixture->lengths[call] = length;
    fixture->placements[call] = placement;
    return fixture->results[call];
}

int main(void) {
    void *fixed = (void *)(uintptr_t)UINT64_C(0x40000000);
    void *chosen = (void *)(uintptr_t)UINT64_C(0x71000000);
    int failed = 7;
    map_fixture fixture = {.results = {fixed, NULL}};

    HL_CHECK(hl_elf_place_image(map_record, &fixture, fixed, 0x20000, &failed) == fixed);
    HL_CHECK(failed == 0 && fixture.calls == 1 && fixture.addresses[0] == fixed &&
             fixture.lengths[0] == 0x20000 && fixture.placements[0] == HL_ELF_MAP_FIXED);

    fixture = (map_fixture){.results = {NULL, chosen}};
    HL_CHECK(hl_elf_place_image(map_record, &fixture, fixed, 0x30000, &failed) == chosen);
    HL_CHECK(failed == 1 && fixture.calls == 2 && fixture.addresses[0] == fixed &&
             fixture.placements[0] == HL_ELF_MAP_FIXED && fixture.addresses[1] == NULL &&
             fixture.lengths[1] == 0x30000 && fixture.placements[1] == HL_ELF_MAP_FLEXIBLE);

    fixture = (map_fixture){.results = {chosen, NULL}};
    HL_CHECK(hl_elf_place_image(map_record, &fixture, NULL, 0x40000, &failed) == chosen);
    HL_CHECK(failed == 0 && fixture.calls == 1 && fixture.addresses[0] == NULL &&
             fixture.placements[0] == HL_ELF_MAP_FLEXIBLE);

    fixture = (map_fixture){0};
    HL_CHECK(hl_elf_place_image(map_record, &fixture, fixed, 0x50000, &failed) == NULL);
    HL_CHECK(failed == 1 && fixture.calls == 2);
    HL_CHECK(hl_elf_place_image(NULL, NULL, fixed, 0x1000, &failed) == NULL && failed == 0);
    HL_CHECK(hl_elf_place_image(map_record, &fixture, fixed, 0, &failed) == NULL && failed == 0);
    return 0;
}
