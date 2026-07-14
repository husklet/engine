#define _DEFAULT_SOURCE
#include "../range.h"

#include <sys/mman.h>
#include <unistd.h>

size_t hl_host_page_size(void) {
    long value = sysconf(_SC_PAGESIZE);
    if (value <= 0 || ((size_t)value & ((size_t)value - 1)) != 0) return 0;
    return (size_t)value;
}

int hl_host_address_mapped(uintptr_t address) {
    size_t page_size = hl_host_page_size();
    unsigned char resident;
    uintptr_t page;
    if (page_size == 0) return 0;
    page = address & ~((uintptr_t)page_size - 1);
    return mincore((void *)page, page_size, &resident) == 0;
}
