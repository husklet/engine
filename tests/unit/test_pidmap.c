#include "test.h"

#include "../../src/linux_abi/container/pidmap.h"

int main(void) {
    hl_linux_pidmap map;
    uint32_t index;

    hl_linux_pidmap_init(&map);
    HL_CHECK(hl_linux_pidmap_count(&map) == 0);
    HL_CHECK(hl_linux_pidmap_host(&map, 41) == 41);
    HL_CHECK(hl_linux_pidmap_guest(&map, 101) == 101);

    HL_CHECK(hl_linux_pidmap_add(&map, 41, 101) == 0);
    HL_CHECK(hl_linux_pidmap_add(&map, 42, 102) == 0);
    HL_CHECK(hl_linux_pidmap_host(&map, 41) == 101);
    HL_CHECK(hl_linux_pidmap_guest(&map, 102) == 42);
    HL_CHECK(hl_linux_pidmap_add(&map, 41, 201) == 0);
    HL_CHECK(hl_linux_pidmap_count(&map) == 2);
    HL_CHECK(hl_linux_pidmap_host(&map, 41) == 201);
    HL_CHECK(hl_linux_pidmap_guest(&map, 101) == 101);

    HL_CHECK(hl_linux_pidmap_remove_host(&map, 201) == 0);
    HL_CHECK(hl_linux_pidmap_count(&map) == 1);
    HL_CHECK(hl_linux_pidmap_host(&map, 41) == 41);
    HL_CHECK(hl_linux_pidmap_remove_host(&map, 201) == -1);

    hl_linux_pidmap_init(&map);
    for (index = 0; index < HL_LINUX_PIDMAP_CAPACITY; ++index)
        HL_CHECK(hl_linux_pidmap_add(&map, (int32_t)(index + 1), (int32_t)(index + 10001)) == 0);
    HL_CHECK(hl_linux_pidmap_add(&map, 9000, 19000) == -1);
    HL_CHECK(hl_linux_pidmap_count(&map) == HL_LINUX_PIDMAP_CAPACITY);
    HL_CHECK(hl_linux_pidmap_add(&map, 1, 20001) == 0);
    HL_CHECK(hl_linux_pidmap_count(&map) == HL_LINUX_PIDMAP_CAPACITY);
    HL_CHECK(hl_linux_pidmap_host(&map, 1) == 20001);

    HL_CHECK(hl_linux_pidmap_add(NULL, 1, 1) == -1);
    HL_CHECK(hl_linux_pidmap_add(&map, 0, 1) == -1);
    HL_CHECK(hl_linux_pidmap_add(&map, 1, 0) == -1);
    HL_CHECK(hl_linux_pidmap_host(NULL, -1) == -1);
    HL_CHECK(hl_linux_pidmap_guest(NULL, -1) == -1);
    HL_CHECK(hl_linux_pidmap_count(NULL) == 0);
    return EXIT_SUCCESS;
}
