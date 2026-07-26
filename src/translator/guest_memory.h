#ifndef HL_TRANSLATOR_GUEST_MEMORY_H
#define HL_TRANSLATOR_GUEST_MEMORY_H

#include <stddef.h>
#include <stdint.h>

/*
 * Guest-address indirection seam.
 *
 * A guest address is not always a host address: the Linux ABI's logical-VMA
 * ledger (linux_abi/logical_vma.c) can move it, and a biased non-PIE image
 * moves it again (core/target/x86_64.c's window).  The translator has to honor
 * both, and DOCS.md section 3 forbids it from calling either -- engine ->
 * translator and engine -> Linux ABI are arrows, translator -> Linux ABI is
 * not.  So the engine binds the accessors here, exactly as it already binds
 * hl_x86_avx_state and the guest-fetch direct validator.
 *
 * Unbound is the honest standalone default and every entry is optional: a
 * translator with no engine under it sees one flat address space where a guest
 * address IS a host address.  That is what the decoder's own unit tests assume.
 */
typedef struct hl_guest_memory_ops {
    /* Executable bytes.  >0: *host/*contiguous describe an indirected mapping.
       0: ordinary, the guest address is the host address.  <0: fault. */
    int (*resolve_exec)(uint64_t guest, size_t length, const void **host, size_t *contiguous);
    /* Data, same tri-state.  0 copies nothing -- the caller owns the ordinary
       case, because only the caller knows which access validator applies. */
    int (*read)(uint64_t guest, void *destination, size_t length);
    int (*write)(uint64_t guest, const void *source, size_t length);
    /* Nonzero once any indirected mapping exists, i.e. the slow element-wise
       path is mandatory. */
    int (*indirect)(void);
    /* Non-PIE bias fold: low link address -> the real high mapping. */
    uint64_t (*host_pointer)(uint64_t guest);
} hl_guest_memory_ops;

void hl_guest_memory_bind(const hl_guest_memory_ops *ops);

int hl_guest_memory_resolve_exec(uint64_t guest, size_t length, const void **host, size_t *contiguous);
int hl_guest_memory_read(uint64_t guest, void *destination, size_t length);
int hl_guest_memory_write(uint64_t guest, const void *source, size_t length);
int hl_guest_memory_indirect(void);
uint64_t hl_guest_memory_host_pointer(uint64_t guest);

#endif
