#include "rep_runtime.h"

#include "../../guest_memory.h"
#include "cpu.h"

#include <string.h>

static hl_x86_rep_store_commit_fn g_rep_store_commit;
static hl_x86_rep_store_observation_active_fn g_rep_store_observation_active;
static hl_x86_rep_access_fn g_rep_readable;
static hl_x86_rep_access_fn g_rep_writable;
static hl_x86_rep_access_special_fn g_rep_access_special;

void hl_x86_rep_set_store_commit(hl_x86_rep_store_commit_fn callback, hl_x86_rep_store_observation_active_fn active) {
    g_rep_store_commit = callback;
    g_rep_store_observation_active = active;
}

void hl_x86_rep_set_access_validators(hl_x86_rep_access_fn readable, hl_x86_rep_access_fn writable,
                                      hl_x86_rep_access_special_fn special) {
    g_rep_readable = readable;
    g_rep_writable = writable;
    g_rep_access_special = special;
}

static uint64_t rep_fault(struct cpu *cpu, uint64_t address, uint64_t width, uint32_t required, uint64_t rip,
                          uint64_t completed) {
    if (cpu != NULL) {
        cpu->bus_ea = address;
        cpu->soft_width = width;
        cpu->soft_required = required;
        cpu->rip = rip;
        cpu->reason = R_SOFTMISS;
    }
    return completed;
}

// One element through the guest-memory seam. hl_guest_memory_read/write answer 0 for an address the engine
// does not indirect, and only then does the direct-access validator apply -- an indirected mapping was already
// permission-checked by the resolver. Returns 0 on success, -1 if the element must fault.
static int rep_element_read(uint64_t guest, void *destination, uint64_t width) {
    int indirected = hl_guest_memory_read(guest, destination, (size_t)width);
    if (indirected < 0) return -1;
    if (indirected > 0) return 0;
    if (g_rep_readable != NULL && !g_rep_readable(guest, (size_t)width)) return -1;
    memcpy(destination, (const void *)(uintptr_t)guest, (size_t)width);
    return 0;
}

static int rep_element_write(uint64_t guest, const void *source, uint64_t width) {
    int indirected = hl_guest_memory_write(guest, source, (size_t)width);
    if (indirected < 0) return -1;
    if (indirected == 0) {
        if (g_rep_writable != NULL && !g_rep_writable(guest, (size_t)width)) return -1;
        memcpy((void *)(uintptr_t)guest, source, (size_t)width);
    }
    if (g_rep_store_commit != NULL) g_rep_store_commit(guest, width);
    return 0;
}

// translator/guest/x86_64 -- rep movs/stos idiom upgrade.
// Generalizes the LSE idiom-upgrade lever to the x86 string ops: a `rep movs`/`rep stos`
// (the idiomatic memcpy/memset of every musl/glibc x86 guest) is lowered to ONE optimized
// host libc call instead of the per-element `ldr;str;sub;cbnz` host loop. Bit-exact with
// that scalar loop for all lengths (incl. 0), alignments, and the forward-overlap smear.

// Host helper for `rep movs`: copy `nbytes` forward, x86 element-by-element semantics.
// rep movs always copies LOW->HIGH; a plain memcpy/memmove is correct only when the
// regions are disjoint or dst precedes src. When src < dst < src+nbytes the forward copy
// SMEARS (each element is re-read after a previous element overwrote it) -- memmove would
// be WRONG here -- so we replay it element-by-element at the guest element width `w`,
// which reproduces the scalar loop's bytes exactly (byte-wise smear differs from a
// w>1 element smear at sub-element overlap offsets).
// W6A item 1 (non-PIE): a biased ET_EXEC's guest pointer may still carry its low link address (e.g. a
// rip-relative lea into the type/rodata section); rebase it to the real high mapping so these bulk C string
// helpers touch the mapped bytes (the single-access fault path nonpie_fixup cannot serve a libc memcpy).
// Inert for PIE/static-PIE (the translator's non-PIE range is empty).
uint64_t hl_x86_rep_movs(void *destination, const void *source, uint64_t nbytes, int w, int df, struct cpu *cpu,
                         uint64_t rip) {
    uint8_t *dst = destination;
    const uint8_t *src = source;
    hl_x86_count_rep_movs();
    dst = (uint8_t *)(uintptr_t)hl_guest_memory_host_pointer((uint64_t)(uintptr_t)dst);
    src = (const uint8_t *)(uintptr_t)hl_guest_memory_host_pointer((uint64_t)(uintptr_t)src);
    if (nbytes == 0) return 0;
    uint64_t span = nbytes - (uint64_t)w;
    uint64_t dlo = (uint64_t)(uintptr_t)dst - (df ? span : 0);
    uint64_t slo = (uint64_t)(uintptr_t)src - (df ? span : 0);
    int special = g_rep_access_special != NULL &&
                  (g_rep_access_special(slo, (size_t)nbytes, 0) || g_rep_access_special(dlo, (size_t)nbytes, 1));
    if (hl_guest_memory_indirect() || special) {
        uint64_t n = nbytes / (unsigned)w;
        for (uint64_t i = 0; i < n; ++i) {
            uint64_t step = i * (uint64_t)w;
            uint64_t dg = (uint64_t)(uintptr_t)(df ? dst - step : dst + step);
            uint64_t sg = (uint64_t)(uintptr_t)(df ? src - step : src + step);
            uint64_t element; // w <= 8; staged so a dst fault leaves guest memory untouched
            if (rep_element_read(sg, &element, (uint64_t)w) != 0)
                return rep_fault(cpu, sg, (uint64_t)w, X86_SOFT_READ, rip, i);
            if (rep_element_write(dg, &element, (uint64_t)w) != 0)
                return rep_fault(cpu, dg, (uint64_t)w, X86_SOFT_WRITE, rip, i);
        }
        return n;
    }
    if (g_rep_store_commit != NULL && g_rep_store_observation_active != NULL && g_rep_store_observation_active()) {
        uint64_t n = nbytes / (unsigned)w;
        for (uint64_t i = 0; i < n; ++i) {
            uint64_t step = i * (uint64_t)w;
            uint8_t *element_dst = df ? dst - step : dst + step;
            const uint8_t *element_src = df ? src - step : src + step;
            memcpy(element_dst, element_src, (size_t)w);
            g_rep_store_commit((uint64_t)(uintptr_t)element_dst, (uint64_t)w);
        }
        return n;
    }
    if (df) { // DF=1 backward: dst/src point at the HIGHEST element; copy high->low, element-granular (the
        // x86 `std; rep movs` used by memmove for dst>src overlap). Element-by-element replays the scalar
        // loop's exact bytes for every overlap/width; byte-identical to the -w element loop below.
        uint64_t n = nbytes / (unsigned)w;
        for (uint64_t i = 0; i < n; i++) {
            uint64_t o = i * (uint64_t)w;
            memcpy(dst - o, src - o, (size_t)w); // one whole w-wide element per step
        }
        return n;
    }
    if (dst <= src || dst >= src + nbytes) { // disjoint, or forward-safe (dst before src)
        memcpy(dst, src, nbytes);
        return nbytes / (unsigned)w;
    }
    switch (w) { // forward-overlap smear, element-granular (matches per-element rep movs)
    case 2: {
        uint16_t *d = (uint16_t *)dst;
        const uint16_t *s = (const uint16_t *)src;
        for (uint64_t i = 0, n = nbytes >> 1; i < n; i++)
            d[i] = s[i];
        return nbytes / (unsigned)w;
    }
    case 4: {
        uint32_t *d = (uint32_t *)dst;
        const uint32_t *s = (const uint32_t *)src;
        for (uint64_t i = 0, n = nbytes >> 2; i < n; i++)
            d[i] = s[i];
        return nbytes / (unsigned)w;
    }
    case 8: {
        uint64_t *d = (uint64_t *)dst;
        const uint64_t *s = (const uint64_t *)src;
        for (uint64_t i = 0, n = nbytes >> 3; i < n; i++)
            d[i] = s[i];
        return nbytes / (unsigned)w;
    }
    default:
        for (uint64_t i = 0; i < nbytes; i++)
            dst[i] = src[i];
        return nbytes;
    }
}

// Host helper for `rep stos`: fill `n` elements of width `w` with `val` (AL/AX/EAX/RAX).
uint64_t hl_x86_rep_stos(void *destination, uint64_t val, uint64_t n, int w, int df, struct cpu *cpu, uint64_t rip) {
    uint8_t *dst = destination;
    hl_x86_count_rep_stos();
    dst = (uint8_t *)(uintptr_t)hl_guest_memory_host_pointer((uint64_t)(uintptr_t)dst);
    uint64_t bytes;
    int overflow = __builtin_mul_overflow(n, (uint64_t)w, &bytes);
    uint64_t span = !overflow && bytes != 0 ? bytes - (uint64_t)w : 0;
    uint64_t dlo = (uint64_t)(uintptr_t)dst - (df ? span : 0);
    int special = overflow || (g_rep_access_special != NULL && g_rep_access_special(dlo, (size_t)bytes, 1));
    if (hl_guest_memory_indirect() || special) {
        for (uint64_t i = 0; i < n; ++i) {
            uint64_t step = i * (uint64_t)w;
            uint64_t dg = (uint64_t)(uintptr_t)(df ? dst - step : dst + step);
            if (rep_element_write(dg, &val, (uint64_t)w) != 0)
                return rep_fault(cpu, dg, (uint64_t)w, X86_SOFT_WRITE, rip, i);
        }
        return n;
    }
    if (g_rep_store_commit != NULL && g_rep_store_observation_active != NULL && g_rep_store_observation_active()) {
        for (uint64_t i = 0; i < n; ++i) {
            uint64_t step = i * (uint64_t)w;
            uint8_t *element_dst = df ? dst - step : dst + step;
            memcpy(element_dst, &val, (size_t)w);
            g_rep_store_commit((uint64_t)(uintptr_t)element_dst, (uint64_t)w);
        }
        return n;
    }
    if (df) { // DF=1 backward: dst points at the highest element; write val at dst, dst-w, dst-2w, ...
        uint8_t *p = dst;
        for (uint64_t i = 0; i < n; i++, p -= (unsigned)w)
            memcpy(p, &val, (size_t)w); // low w bytes of RAX, little-endian (== AL/AX/EAX/RAX)
        return n;
    }
    switch (w) {
    case 2: {
        uint16_t *p = (uint16_t *)dst, v = (uint16_t)val;
        for (uint64_t i = 0; i < n; i++)
            p[i] = v;
        return n;
    }
    case 4: {
        uint32_t *p = (uint32_t *)dst, v = (uint32_t)val;
        for (uint64_t i = 0; i < n; i++)
            p[i] = v;
        return n;
    }
    case 8: {
        uint64_t *p = (uint64_t *)dst, v = val;
        for (uint64_t i = 0; i < n; i++)
            p[i] = v;
        return n;
    }
    default: memset(dst, (int)(val & 0xff), n); return n;
    }
}
