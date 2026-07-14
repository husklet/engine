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
    HL_CHECK(hl_host_range_mapped(UINTPTR_MAX, 0));
    HL_CHECK(!hl_host_range_mapped(UINTPTR_MAX - 3, 8));

    HL_CHECK(munmap(mapping + page, page) == 0);
    HL_CHECK(!hl_host_address_mapped((uintptr_t)mapping + page));
    HL_CHECK(hl_host_page_neighbor_mapped((uintptr_t)mapping + page));
    HL_CHECK(!hl_host_page_neighbor_mapped((uintptr_t)mapping + page + 1));
    HL_CHECK(!hl_host_page_neighbor_mapped(UINTPTR_MAX));
    HL_CHECK(!hl_host_range_mapped((uintptr_t)mapping, page * 3));
    HL_CHECK(hl_host_range_mapped((uintptr_t)mapping, page));
    HL_CHECK(hl_host_range_mapped((uintptr_t)mapping + page * 2, page));

    HL_CHECK(mprotect(mapping, page, PROT_NONE) == 0);
    HL_CHECK(hl_host_address_mapped((uintptr_t)mapping));
    HL_CHECK(munmap(mapping, page) == 0);
    HL_CHECK(munmap(mapping + page * 2, page) == 0);
    return 0;
}
