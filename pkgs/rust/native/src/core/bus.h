#ifndef HL_CORE_BUS_H
#define HL_CORE_BUS_H

#include <stdint.h>

typedef uint64_t (*jit_guest_bus_query)(uint64_t address, uint64_t size);
void jit_guest_bus_bind(jit_guest_bus_query query, int active, uint64_t generation);
void jit_guest_bus_changed(void *opaque, uint64_t generation, int active);
void jit_guest_bus_transition_begin(void *opaque);
void jit_guest_bus_transition_end(void *opaque);
int jit_guest_bus_active(void);
uint64_t jit_guest_bus_fault(uint64_t address, uint64_t size);

#endif
