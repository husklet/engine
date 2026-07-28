#ifndef HL_LINUX_ABI_ELF_PROTECT_H
#define HL_LINUX_ABI_ELF_PROTECT_H

#include "hl/host_services.h"

#include <stdint.h>
#include "host_mman.h"
#include <unistd.h>

// THE LOADER'S PROTECTION CONTRACT, stated once because the two loaders each holding half of it is what
// let an x86-64 guest store into its own .rodata and carry on. A PT_LOAD's p_flags decide TWO things and
// they are set together, here:
//
//   the HOST PAGE PROTECTION is the only enforcement that exists. Both engines write guest memory with a
//   plain host store or memcpy and carry no permission check of their own, so if the page is writable the
//   store lands -- there is no second gate behind it.
//
//   the READ-ONLY REGISTRY (g_gro) is what lets the resulting host fault be classified as the guest's own
//   SIGSEGV (x86.c jit86_lazyguard, elf.c nonpie_guard) rather than a page to demand-map or unprotect, and
//   is what answers /proc/self/maps and the syscall uaccess checks.
//
// Register without protecting and the store is silently dropped while every registry-reading surface
// insists the page is read-only; protect without registering and the engine cannot tell its own fault
// from a lazy-growth one. Registry keys are GUEST coordinates (thread.c's one rule) while a host
// protection takes the storage address -- hence the nonpie_unfold on the way into g_gro.
//
// This is also the state the fork server's pristine-image restore has to put back, so it calls the same
// function rather than restating the rule (fork.c, FSRV_RESTORE_DONE).

#define HL_ELF_PROTECT_RETRIES 6

static uint32_t hl_elf_ph32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t hl_elf_ph64(const uint8_t *p) {
    return (uint64_t)hl_elf_ph32(p) | ((uint64_t)hl_elf_ph32(p + 4) << 32);
}

// `phdr` is the program-header table (file or mapped copy -- identical bytes), `bias` the amount the
// image was displaced from its link address. `mapping` is the image's host mapping; pass NULL to change
// the protection with mprotect(2) directly, which is all the fork-server restore has to hand.
static void hl_elf_protect_segments(const hl_host_memory_mapping *mapping, const uint8_t *phdr, int phnum, int phent,
                                    uint64_t bias) {
    size_t host_page = hl_host_page_size();
    for (int i = 0; i < phnum; i++) {
        const uint8_t *ph = phdr + (size_t)i * (size_t)phent;
        if (hl_elf_ph32(ph) != 1) continue; // PT_LOAD
        uint32_t fl = hl_elf_ph32(ph + 4);  // PF_X=1, PF_W=2, PF_R=4
        uint64_t v = hl_elf_ph64(ph + 16), msz = hl_elf_ph64(ph + 40);
        uint64_t s = (v + bias) & ~0xFFFull, e = (v + bias + msz + 0xFFFull) & ~0xFFFull;
        if (e <= s) continue;
        // A segment edge off a host page boundary shares that page with its neighbour (4 KiB guest
        // segments on a 16 KiB Apple-silicon host); narrowing it would make the neighbour's writable
        // .data/.bss subpage read-only. Leave those pages open physically and let the registry answer.
        if (host_page && !(s & (host_page - 1)) && !(e & (host_page - 1))) {
            if (mapping != NULL) {
                uint32_t protection = HL_HOST_MEMORY_READ | ((fl & 2) ? HL_HOST_MEMORY_WRITE : 0) |
                                      ((fl & 1) ? HL_HOST_MEMORY_EXECUTE : 0);
                const hl_host_services *host = effective_host_services();
                for (int t = 0;; t++) {
                    hl_host_result r =
                        host->memory->protect(host->context, mapping->handle, s - mapping->address, e - s, protection);
                    if (r.status == HL_STATUS_OK || r.status != HL_STATUS_OUT_OF_MEMORY || t >= HL_ELF_PROTECT_RETRIES)
                        break;
                    usleep(2000u << t); // transient pressure: back off and re-tighten
                }
            } else {
                int protection = PROT_READ | ((fl & 2) ? PROT_WRITE : 0) | ((fl & 1) ? PROT_EXEC : 0);
                (void)mprotect((void *)(uintptr_t)s, (size_t)(e - s), protection);
            }
        }
        uint64_t gs = nonpie_unfold(s), ge = nonpie_unfold(e - 1) + 1;
        if (fl & 2)
            gro_clear(gs, ge);
        else
            gro_add(gs, ge);
    }
}

#endif
