#include "test.h"

#include "../../src/linux_abi/container/snapshot.h"

int main(void) {
    hl_linux_snapshot snapshot;
    uint64_t first;

    hl_linux_snapshot_init(&snapshot);
    HL_CHECK(!hl_linux_snapshot_enabled(&snapshot));
    HL_CHECK(hl_linux_snapshot_reserve(&snapshot, 0x1000) == 0);
    HL_CHECK(snapshot.next == HL_LINUX_SNAPSHOT_BASE);

    hl_linux_snapshot_enable(&snapshot);
    HL_CHECK(hl_linux_snapshot_enabled(&snapshot));
    first = hl_linux_snapshot_reserve(&snapshot, 1);
    HL_CHECK(first == HL_LINUX_SNAPSHOT_BASE);
    HL_CHECK(snapshot.next == first + UINT64_C(0x110000));
    HL_CHECK(hl_linux_snapshot_reserve(&snapshot, UINT64_C(0x10001)) == first + UINT64_C(0x110000));
    HL_CHECK(snapshot.next == first + UINT64_C(0x230000));

    hl_linux_snapshot_advance(&snapshot, first + UINT64_C(0x500001));
    HL_CHECK(snapshot.next == first + UINT64_C(0x600000));
    hl_linux_snapshot_advance(&snapshot, first + UINT64_C(0x400000));
    HL_CHECK(snapshot.next == first + UINT64_C(0x600000));

    snapshot.next = UINT64_MAX - UINT64_C(0x100000);
    HL_CHECK(hl_linux_snapshot_reserve(&snapshot, UINT64_C(0x10000)) == 0);
    HL_CHECK(snapshot.next == UINT64_MAX - UINT64_C(0x100000));
    HL_CHECK(hl_linux_snapshot_reserve(&snapshot, UINT64_MAX) == 0);
    HL_CHECK(snapshot.next == UINT64_MAX - UINT64_C(0x100000));

    hl_linux_snapshot_enable(NULL);
    hl_linux_snapshot_advance(NULL, 1);
    HL_CHECK(!hl_linux_snapshot_enabled(NULL));
    HL_CHECK(hl_linux_snapshot_reserve(NULL, 1) == 0);
    return EXIT_SUCCESS;
}
