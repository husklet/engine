#define _DEFAULT_SOURCE
#include "range.h"

#include <unistd.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#else
#include <sys/mman.h>
#endif

static size_t hl_host_page_size(void) {
    long value = sysconf(_SC_PAGESIZE);
    if (value <= 0 || ((size_t)value & ((size_t)value - 1)) != 0) return 0;
    return (size_t)value;
}

int hl_host_address_mapped(uintptr_t address) {
#if defined(__APPLE__)
    mach_vm_address_t region = address;
    mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    if (mach_vm_region(mach_task_self(), &region, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count,
                       &object) != KERN_SUCCESS)
        return 0;
    return address >= (uintptr_t)region && address < (uintptr_t)region + (uintptr_t)size;
#else
    size_t page_size = hl_host_page_size();
    unsigned char resident;
    uintptr_t page;
    if (page_size == 0) return 0;
    page = address & ~((uintptr_t)page_size - 1);
    return mincore((void *)page, page_size, &resident) == 0;
#endif
}

int hl_host_range_mapped(uintptr_t address, size_t size) {
    size_t page_size;
    uintptr_t end;
    uintptr_t page;
    if (size == 0) return 1;
    end = address + size;
    if (end < address) return 0;
    page_size = hl_host_page_size();
    if (page_size == 0) return 0;
    page = address & ~((uintptr_t)page_size - 1);
    for (;;) {
        if (!hl_host_address_mapped(page)) return 0;
        if (end - 1 - page < page_size) return 1;
        page += page_size;
    }
}
