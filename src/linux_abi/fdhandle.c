// hl/linux_abi -- the guest-descriptor -> hl_host_handle side table. See fdhandle.h.
//
// Compiled as part of fdcache.c rather than as a translation unit of its own,
// because both of its two consumer populations must see the SAME instance: the
// guest-target unity TU, which reaches it from the syscall layer and from the
// host-seam headers, and the separately compiled library sources (the logical
// VMA ledger), which reach it from outside that TU. fdcache.c is the file this
// layer already keeps its other descriptor-keyed side table in, and it is
// already linked into both, so this costs no new build-graph edge.
#include "fdhandle.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

// 64 leaves of 1024 covers every descriptor number the syscall layer's own
// fd-indexed tables cover. Leaves are claimed on demand, so a process that
// never publishes pays 64 pointers and nothing else.
#define HL_FDHANDLE_LEAF 1024
#define HL_FDHANDLE_LEAVES 64
#define HL_FDHANDLE_CAPACITY (HL_FDHANDLE_LEAF * HL_FDHANDLE_LEAVES)

struct hl_fdhandle_slot {
    _Atomic uint64_t handle;
    _Atomic uint32_t state;
};

struct hl_fdhandle_leaf {
    struct hl_fdhandle_slot slots[HL_FDHANDLE_LEAF];
};

static _Atomic(struct hl_fdhandle_leaf *) g_fdhandle_leaves[HL_FDHANDLE_LEAVES];
static _Atomic(const hl_host_services *) g_fdhandle_host;
static _Atomic uint32_t g_fdhandle_population;

void hl_fdhandle_bind(const hl_host_services *host) {
    atomic_store_explicit(&g_fdhandle_host, host, memory_order_release);
}

const hl_host_services *hl_fdhandle_host(void) {
    return atomic_load_explicit(&g_fdhandle_host, memory_order_acquire);
}

uint32_t hl_fdhandle_population(void) {
    return atomic_load_explicit(&g_fdhandle_population, memory_order_relaxed);
}

static const hl_host_file_services *fdhandle_file(void) {
    const hl_host_services *host = hl_fdhandle_host();
    return host == NULL ? NULL : host->file;
}

// The leaf a descriptor lives in, without claiming one. NULL is the common case
// and the reason lookup on an unbound descriptor is two loads and a branch.
static struct hl_fdhandle_leaf *fdhandle_leaf(int descriptor) {
    if (descriptor < 0 || descriptor >= HL_FDHANDLE_CAPACITY) return NULL;
    return atomic_load_explicit(&g_fdhandle_leaves[descriptor / HL_FDHANDLE_LEAF], memory_order_acquire);
}

// Claim the leaf if it does not exist. A losing racer frees its own allocation
// and adopts the winner's; no leaf is ever freed, so a pointer any reader loaded
// stays valid for the life of the process.
static struct hl_fdhandle_leaf *fdhandle_leaf_claim(int descriptor) {
    struct hl_fdhandle_leaf *fresh;
    struct hl_fdhandle_leaf *observed = NULL;
    if (descriptor < 0 || descriptor >= HL_FDHANDLE_CAPACITY) return NULL;
    fresh = fdhandle_leaf(descriptor);
    if (fresh != NULL) return fresh;
    fresh = calloc(1, sizeof *fresh);
    if (fresh == NULL) return NULL;
    if (atomic_compare_exchange_strong_explicit(&g_fdhandle_leaves[descriptor / HL_FDHANDLE_LEAF], &observed, fresh,
                                                memory_order_acq_rel, memory_order_acquire))
        return fresh;
    free(fresh);
    return observed;
}

static struct hl_fdhandle_slot *fdhandle_slot(int descriptor) {
    struct hl_fdhandle_leaf *leaf = fdhandle_leaf(descriptor);
    return leaf == NULL ? NULL : &leaf->slots[descriptor % HL_FDHANDLE_LEAF];
}

// Retire a binding and hand the caller the handle it held, or INVALID. The
// handle is cleared before the state so no reader can see a live handle paired
// with a state word that has already been reused.
static hl_host_handle fdhandle_take(int descriptor) {
    struct hl_fdhandle_slot *slot = fdhandle_slot(descriptor);
    uint64_t previous;
    if (slot == NULL) return HL_HOST_HANDLE_INVALID;
    previous = atomic_exchange_explicit(&slot->handle, (uint64_t)HL_HOST_HANDLE_INVALID, memory_order_acq_rel);
    if (previous == HL_HOST_HANDLE_INVALID) return HL_HOST_HANDLE_INVALID;
    atomic_store_explicit(&slot->state, 0, memory_order_relaxed);
    (void)atomic_fetch_sub_explicit(&g_fdhandle_population, 1, memory_order_relaxed);
    return (hl_host_handle)previous;
}

static void fdhandle_close(hl_host_handle handle) {
    const hl_host_file_services *file = fdhandle_file();
    const hl_host_services *host = hl_fdhandle_host();
    if (handle == HL_HOST_HANDLE_INVALID || file == NULL || file->close == NULL) return;
    (void)file->close(host->context, handle);
}

int hl_fdhandle_publish(int descriptor, hl_host_handle handle, uint32_t state) {
    struct hl_fdhandle_leaf *leaf;
    struct hl_fdhandle_slot *slot;
    if (handle == HL_HOST_HANDLE_INVALID) return -1;
    leaf = fdhandle_leaf_claim(descriptor);
    if (leaf == NULL) return -1;
    slot = &leaf->slots[descriptor % HL_FDHANDLE_LEAF];
    // A publish over a live binding is a descriptor being reused without the
    // reset path having run. Close the displaced handle rather than leak it.
    fdhandle_close(fdhandle_take(descriptor));
    atomic_store_explicit(&slot->state, state, memory_order_relaxed);
    atomic_store_explicit(&slot->handle, (uint64_t)handle, memory_order_release);
    (void)atomic_fetch_add_explicit(&g_fdhandle_population, 1, memory_order_relaxed);
    return 0;
}

int hl_fdhandle_lookup(int descriptor, hl_host_handle *handle) {
    struct hl_fdhandle_slot *slot = fdhandle_slot(descriptor);
    uint64_t found;
    if (slot == NULL) return 0;
    found = atomic_load_explicit(&slot->handle, memory_order_acquire);
    if (found == HL_HOST_HANDLE_INVALID) return 0;
    if (handle != NULL) *handle = (hl_host_handle)found;
    return 1;
}

int hl_fdhandle_lookup_state(int descriptor, hl_host_handle *handle, uint32_t *state) {
    struct hl_fdhandle_slot *slot = fdhandle_slot(descriptor);
    uint64_t found;
    if (slot == NULL) return 0;
    found = atomic_load_explicit(&slot->handle, memory_order_acquire);
    if (found == HL_HOST_HANDLE_INVALID) return 0;
    if (handle != NULL) *handle = (hl_host_handle)found;
    if (state != NULL) *state = atomic_load_explicit(&slot->state, memory_order_relaxed);
    return 1;
}

int hl_fdhandle_set_state(int descriptor, uint32_t state) {
    struct hl_fdhandle_slot *slot = fdhandle_slot(descriptor);
    if (slot == NULL || atomic_load_explicit(&slot->handle, memory_order_acquire) == HL_HOST_HANDLE_INVALID) return -1;
    atomic_store_explicit(&slot->state, state, memory_order_relaxed);
    return 0;
}

int hl_fdhandle_forget(int descriptor) {
    return fdhandle_take(descriptor) != HL_HOST_HANDLE_INVALID;
}

int hl_fdhandle_release(int descriptor) {
    hl_host_handle handle = fdhandle_take(descriptor);
    if (handle == HL_HOST_HANDLE_INVALID) return 0;
    fdhandle_close(handle);
    return 1;
}

int hl_fdhandle_clone(int from, int to) {
    const hl_host_file_services *file = fdhandle_file();
    const hl_host_services *host = hl_fdhandle_host();
    hl_host_handle source;
    uint32_t state = 0;
    hl_host_result cloned;
    if (from == to) return 0;
    if (!hl_fdhandle_lookup_state(from, &source, &state)) return -1;
    if (file == NULL || file->clone_for_fork == NULL) return -1;
    cloned = file->clone_for_fork(host->context, source);
    if (cloned.status != HL_STATUS_OK || cloned.value == HL_HOST_HANDLE_INVALID) return -1;
    // dup(2) never carries close-on-exec to the new descriptor; the caller sets
    // it afterwards for dup3(O_CLOEXEC)/F_DUPFD_CLOEXEC.
    if (hl_fdhandle_publish(to, (hl_host_handle)cloned.value, state & ~(uint32_t)HL_FDHANDLE_CLOEXEC) != 0) {
        fdhandle_close((hl_host_handle)cloned.value);
        return -1;
    }
    return 0;
}

int hl_fdhandle_release_cloexec(void) {
    int released = 0;
    int leaf_index;
    for (leaf_index = 0; leaf_index < HL_FDHANDLE_LEAVES; leaf_index++) {
        struct hl_fdhandle_leaf *leaf = atomic_load_explicit(&g_fdhandle_leaves[leaf_index], memory_order_acquire);
        int slot_index;
        if (leaf == NULL) continue;
        for (slot_index = 0; slot_index < HL_FDHANDLE_LEAF; slot_index++) {
            struct hl_fdhandle_slot *slot = &leaf->slots[slot_index];
            if (atomic_load_explicit(&slot->handle, memory_order_acquire) == HL_HOST_HANDLE_INVALID) continue;
            if (!(atomic_load_explicit(&slot->state, memory_order_relaxed) & HL_FDHANDLE_CLOEXEC)) continue;
            released += hl_fdhandle_release(leaf_index * HL_FDHANDLE_LEAF + slot_index);
        }
    }
    return released;
}

int hl_fdhandle_adopt_standard_streams(void) {
    static const uint32_t direction[3] = {HL_FDHANDLE_READ, HL_FDHANDLE_WRITE, HL_FDHANDLE_WRITE};
    const hl_host_file_services *file = fdhandle_file();
    const hl_host_services *host = hl_fdhandle_host();
    int adopted = 0;
    int stream;
    if (file == NULL || file->standard_stream == NULL) return 0;
    for (stream = 0; stream < 3; stream++) {
        hl_host_result opened;
        uint32_t state;
        if (hl_fdhandle_lookup(stream, NULL)) continue;
        opened = file->standard_stream(host->context, (uint32_t)stream);
        if (opened.status != HL_STATUS_OK || opened.value == HL_HOST_HANDLE_INVALID) continue;
        // detail carries the HL_HOST_FILE_* access the host was able to
        // establish; direction[] is the floor, because a host that cannot
        // report an access mode still knows which stream was asked for.
        state = direction[stream];
        if (opened.detail & HL_HOST_FILE_READ) state |= HL_FDHANDLE_READ;
        if (opened.detail & HL_HOST_FILE_WRITE) state |= HL_FDHANDLE_WRITE;
        if (opened.detail & HL_HOST_FILE_APPEND) state |= HL_FDHANDLE_APPEND;
        // A standard stream is a console, pipe or redirected file; the engine
        // cannot tell which, and claiming an offset it may not have would make
        // an F_GETFL answer wrong. Not seekable is the honest floor.
        if (hl_fdhandle_publish(stream, (hl_host_handle)opened.value, state) != 0) {
            fdhandle_close((hl_host_handle)opened.value);
            continue;
        }
        adopted++;
    }
    return adopted;
}
