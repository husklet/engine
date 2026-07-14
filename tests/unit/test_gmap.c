#include "test.h"

#include "../../src/linux_abi/container/vfs/gmap.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

int main(void) {
    hl_limit_table limits;
    hl_gmap_entry first;
    hl_gmap_entry second;

    hl_limit_table_init(&limits);
    hl_gmap_bind_limits(&limits);
    HL_CHECK(hl_gmap_count() == 0);

    hl_gmap_add(0, 0x1000);
    hl_gmap_add(UINT64_MAX, 0x1000);
    hl_gmap_add(0x10000, 0x5000);
    HL_CHECK(hl_gmap_count() == 1);
    HL_CHECK(hl_gmap_find_length(0x10000) == 0x5000);
    HL_CHECK(hl_gmap_contains(0x11000, 0x2000));
    HL_CHECK(!hl_gmap_contains(0x14000, 0x2000));

    hl_gmap_set_guest_length(0x10000, 0x4000);
    HL_CHECK(hl_gmap_get(0, &first));
    HL_CHECK(first.address == 0x10000 && first.length == 0x5000 && first.guest_length == 0x4000);

    hl_gmap_unmap_range(0x12000, 0x13000);
    HL_CHECK(hl_gmap_count() == 2);
    HL_CHECK(hl_gmap_get(0, &first));
    HL_CHECK(hl_gmap_get(1, &second));
    HL_CHECK(first.address == 0x10000 && first.length == 0x2000 && first.guest_length == 0x2000);
    HL_CHECK(second.address == 0x13000 && second.length == 0x2000 && second.guest_length == 0x2000);
    HL_CHECK(!hl_gmap_contains(0x12000, 1));

    hl_gmap_lock_add(0x10011, 100);
    HL_CHECK(hl_gmap_lock_total_bytes() == 0x1000);
    HL_CHECK(hl_gmap_lock_region_bytes(0x10000, 0x11000) == 0x1000);
    hl_gmap_lock_add(0x11000, 0x2000);
    HL_CHECK(hl_gmap_lock_total_bytes() == 0x3000);
    hl_gmap_lock_remove(0x11800, 0x800);
    HL_CHECK(hl_gmap_lock_total_bytes() == 0x2000);

    HL_CHECK(hl_limit_table_set(&limits, 8, 0x2000, 0x2000) == 0);
    HL_CHECK(hl_gmap_lock_limit_range(0x10000, 0x1000) == 0);
    HL_CHECK(hl_gmap_lock_limit_range(0x14000, 0x1000) == -ENOMEM);
    HL_CHECK(hl_gmap_lock_limit_all() == -ENOMEM);
    HL_CHECK(hl_limit_table_set(&limits, 8, 0, 0) == 0);
    HL_CHECK(hl_gmap_lock_limit_range(0x10000, 1) == -EPERM);

    hl_gmap_lock_all(1);
    HL_CHECK(hl_gmap_lock_future());
    HL_CHECK(hl_gmap_lock_total_bytes() == 0x4000);
    hl_gmap_lock_reset();
    HL_CHECK(!hl_gmap_lock_future() && hl_gmap_lock_total_bytes() == 0);

    hl_gmap_remove(0x10000);
    hl_gmap_remove(0x13000);
    HL_CHECK(hl_gmap_count() == 0);

    {
        int zero = open("/dev/zero", O_RDWR);
        HL_CHECK(zero >= 0);
        void *mapping = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_PRIVATE, zero, 0);
        close(zero);
        HL_CHECK(mapping != MAP_FAILED);
        hl_gmap_add((uint64_t)(uintptr_t)mapping, 0x1000);
        HL_CHECK(hl_gmap_count() == 1);
        (void)hl_gmap_lock_wire_current();
        hl_gmap_lock_unwire_all();
        hl_gmap_reset();
        HL_CHECK(hl_gmap_count() == 0);
    }
    return EXIT_SUCCESS;
}
