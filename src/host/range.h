#ifndef HL_HOST_RANGE_H
#define HL_HOST_RANGE_H

#include <stddef.h>
#include <stdint.h>

/* True when the host page containing address is part of the process address space. */
int hl_host_address_mapped(uintptr_t address);

/* True when every host page touched by [address, address + size) is mapped. */
int hl_host_range_mapped(uintptr_t address, size_t size);

#endif
