#include "test.h"

#include "../../src/host/range.h"

#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

int main(void) {
    long page_value = sysconf(_SC_PAGESIZE);
    HL_CHECK(page_value > 0);
    size_t page = (size_t)page_value;
    HL_CHECK(hl_host_page_size() == page);
    unsigned char *mapping = mmap(NULL, page * 3, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    HL_CHECK(mapping != MAP_FAILED);

    HL_CHECK(hl_host_address_mapped((uintptr_t)mapping));
    HL_CHECK(hl_host_address_mapped((uintptr_t)mapping + page - 1));
    HL_CHECK(hl_host_range_mapped((uintptr_t)mapping + page - 1, page + 2));
    hl_host_region region = {0};
    HL_CHECK(hl_host_region_query((uintptr_t)mapping, &region));
    HL_CHECK((uintptr_t)mapping >= region.address && (uintptr_t)mapping - region.address < region.size);
    HL_CHECK((region.protection & (HL_HOST_REGION_READ | HL_HOST_REGION_WRITE)) ==
             (HL_HOST_REGION_READ | HL_HOST_REGION_WRITE));
    HL_CHECK(!hl_host_region_query((uintptr_t)mapping, NULL));
    HL_CHECK(!hl_host_region_query(UINTPTR_MAX, &region));
    HL_CHECK(hl_host_range_mapped(UINTPTR_MAX, 0));
    HL_CHECK(!hl_host_range_mapped(UINTPTR_MAX - 3, 8));

    HL_CHECK(munmap(mapping + page, page) == 0);
    HL_CHECK(!hl_host_address_mapped((uintptr_t)mapping + page));
    HL_CHECK(hl_host_region_query((uintptr_t)mapping + page, &region));
    HL_CHECK(region.address >= (uintptr_t)mapping + page);
    HL_CHECK(hl_host_page_neighbor_mapped((uintptr_t)mapping + page));
    HL_CHECK(!hl_host_page_neighbor_mapped((uintptr_t)mapping + page + 1));
    HL_CHECK(!hl_host_page_neighbor_mapped(UINTPTR_MAX));
    HL_CHECK(!hl_host_range_mapped((uintptr_t)mapping, page * 3));
    HL_CHECK(hl_host_range_mapped((uintptr_t)mapping, page));
    HL_CHECK(hl_host_range_mapped((uintptr_t)mapping + page * 2, page));

    HL_CHECK(mprotect(mapping, page, PROT_NONE) == 0);
    HL_CHECK(hl_host_address_mapped((uintptr_t)mapping));
    HL_CHECK(hl_host_region_query((uintptr_t)mapping, &region));
    HL_CHECK((region.protection & (HL_HOST_REGION_READ | HL_HOST_REGION_WRITE | HL_HOST_REGION_EXECUTE)) == 0);
    HL_CHECK(munmap(mapping, page) == 0);
    HL_CHECK(munmap(mapping + page * 2, page) == 0);

    /* The record a host registry keeps of what a partially unmapped handle gave back. */
    {
        hl_host_hole_set set = {NULL, 0, 0};
        uint64_t offset = 0;
        uint64_t size = 0;
        /* An untouched set holds its whole frame and yields it as one range. */
        HL_CHECK(hl_host_hole_set_holds(&set, 0, 300) && hl_host_hole_set_holds(NULL, 0, 1));
        HL_CHECK(hl_host_hole_set_held_range(&set, 300, 0, &offset, &size) && offset == 0 && size == 300);
        HL_CHECK(!hl_host_hole_set_held_range(&set, 300, 1, &offset, &size));
        /* A zero-length question is not a claim on anything. */
        HL_CHECK(!hl_host_hole_set_holds(&set, 0, 0));

        /* One hole in the middle splits the frame in two and stops answering for itself. */
        HL_CHECK(hl_host_hole_set_retire(&set, 100, 50) && set.count == 1);
        HL_CHECK(!hl_host_hole_set_holds(&set, 100, 50) && !hl_host_hole_set_holds(&set, 110, 10));
        HL_CHECK(hl_host_hole_set_holds(&set, 99, 2) && hl_host_hole_set_holds(&set, 149, 2));
        HL_CHECK(hl_host_hole_set_held_range(&set, 300, 0, &offset, &size) && offset == 0 && size == 100);
        HL_CHECK(hl_host_hole_set_held_range(&set, 300, 1, &offset, &size) && offset == 150 && size == 150);
        HL_CHECK(!hl_host_hole_set_held_range(&set, 300, 2, &offset, &size));

        /* Touching and overlapping subranges merge rather than accumulate. */
        HL_CHECK(hl_host_hole_set_retire(&set, 150, 25) && set.count == 1 && set.entries[0].size == 75);
        HL_CHECK(hl_host_hole_set_retire(&set, 90, 20) && set.count == 1 && set.entries[0].offset == 90 &&
                 set.entries[0].size == 85);
        /* A disjoint one is kept apart, and entries stay in ascending order. */
        HL_CHECK(hl_host_hole_set_retire(&set, 10, 5) && set.count == 2 && set.entries[0].offset == 10 &&
                 set.entries[1].offset == 90);
        HL_CHECK(hl_host_hole_set_retire(&set, 250, 5) && set.count == 3 && set.entries[2].offset == 250);
        /* One subrange spanning them all collapses back to a single entry. */
        HL_CHECK(hl_host_hole_set_retire(&set, 0, 260) && set.count == 1 && set.entries[0].offset == 0 &&
                 set.entries[0].size == 260);
        HL_CHECK(hl_host_hole_set_holds(&set, 0, 300) && !hl_host_hole_set_holds(&set, 0, 260));
        /* Retiring the rest leaves nothing held, which is what consumes a handle. */
        HL_CHECK(hl_host_hole_set_retire(&set, 260, 40) && !hl_host_hole_set_holds(&set, 0, 300));
        HL_CHECK(!hl_host_hole_set_held_range(&set, 300, 0, &offset, &size));
        /* A refused record leaves the set exactly as it was, so the caller keeps claiming. */
        HL_CHECK(!hl_host_hole_set_retire(&set, 5, 0) && set.count == 1);
        HL_CHECK(!hl_host_hole_set_retire(NULL, 0, 1));
        hl_host_hole_set_release(&set);
        HL_CHECK(set.entries == NULL && set.count == 0 && set.capacity == 0);
        hl_host_hole_set_release(&set);
        hl_host_hole_set_release(NULL);

        /* Growth past the initial capacity keeps every disjoint hole and their order. */
        for (uint32_t index = 0; index < 12u; ++index)
            HL_CHECK(hl_host_hole_set_retire(&set, (uint64_t)(11u - index) * 10u, 5) && set.count == index + 1u);
        for (uint32_t index = 0; index + 1u < set.count; ++index)
            HL_CHECK(set.entries[index].offset < set.entries[index + 1u].offset);
        HL_CHECK(hl_host_hole_set_held_range(&set, 120, 0, &offset, &size) && offset == 5 && size == 5);
        HL_CHECK(!hl_host_hole_set_holds(&set, 100, 5) && hl_host_hole_set_holds(&set, 115, 5));
        hl_host_hole_set_release(&set);
    }
    return 0;
}
