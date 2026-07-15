#ifndef HL_LINUX_ABI_BUS_H
#define HL_LINUX_ABI_BUS_H

#include <stdint.h>

uint64_t hl_linux_bus_generation(void);
int hl_linux_bus_active(void);
int hl_linux_bus_hit(uint64_t address, uint64_t size);
uint64_t hl_linux_bus_fault(uint64_t address, uint64_t size);
typedef void (*hl_linux_bus_change_fn)(void *opaque, uint64_t generation, int active);
void hl_linux_bus_set_change_callback(hl_linux_bus_change_fn callback, void *opaque);
typedef void (*hl_linux_bus_transition_fn)(void *opaque);
void hl_linux_bus_set_transition_callbacks(hl_linux_bus_transition_fn begin, hl_linux_bus_transition_fn end,
                                           void *opaque);

/* Serializes an externally observed host mapping change with translated guest
   memory.  begin() synchronously parks pre-guard translations; end() publishes
   the rebuilt precise ledger and releases peers. */
typedef struct hl_linux_bus_transition {
    uint64_t generation;
    uint32_t held;
} hl_linux_bus_transition;
int hl_linux_bus_transition_begin(hl_linux_bus_transition *transition);
int hl_linux_bus_transition_add(hl_linux_bus_transition *transition, uint64_t lo, uint64_t hi);
void hl_linux_bus_transition_clear(hl_linux_bus_transition *transition, uint64_t lo, uint64_t hi);
void hl_linux_bus_transition_end(hl_linux_bus_transition *transition);

#endif
