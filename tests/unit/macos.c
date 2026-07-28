#define _POSIX_C_SOURCE 200809L
#include "test.h"

#include "hl/macos.h"
#include "counter.h"
#include "transfer.h"
#include "../../src/host/clock.h"
#include "../../src/host/file.h"
#include "../../src/host/system.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef MAP_ANON
#define MAP_ANON 0x1000
#endif

static int32_t child_exit_37(void *context) {
    HL_CHECK(context == (void *)(uintptr_t)37);
    return 37;
}

/* Shared state for the parking cases: a parked thread has to be reachable from the thread that
 * releases or interrupts it. */
typedef struct park_probe {
    const hl_host_services *services;
    uint64_t waiter;
    uint32_t *word;
    uint64_t deadline_ns;
    int32_t status;
    uint64_t value;
} park_probe;

static uint64_t monotonic_now_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

static void sleep_ms(long milliseconds) {
    struct timespec delay = {milliseconds / 1000, (milliseconds % 1000) * 1000000L};
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {}
}

static void *park_once(void *context) {
    park_probe *probe = context;
    hl_host_result result =
        probe->services->sync->park(probe->services->context, probe->waiter, HL_HOST_PARK_PRIVATE,
                                    (uint64_t)(uintptr_t)probe->word, probe->word, 0, 4, probe->deadline_ns);
    probe->status = result.status;
    probe->value = result.value;
    return NULL;
}

typedef struct stream_fork_close_context {
    const hl_host_services *services;
    hl_host_handle input;
    hl_host_handle output;
} stream_fork_close_context;

static int32_t child_close_inherited_stream(void *context) {
    stream_fork_close_context *stream = context;
    if (stream->services->stream->close(stream->services->context, stream->input).status != HL_STATUS_OK) return 91;
    if (stream->services->stream->close(stream->services->context, stream->output).status != HL_STATUS_OK) return 92;
    return 0;
}

typedef struct signal_repair_probe {
    const hl_host_services *services;
    uint64_t address;
    int result;
} signal_repair_probe;

static void *repair_signal_page_once(void *context) {
    signal_repair_probe *probe = context;
    probe->result = probe->services->memory->repair_signal_page(
        probe->services->context, probe->address, UINT64_C(4096), HL_HOST_MEMORY_READ | HL_HOST_MEMORY_WRITE);
    return NULL;
}

static int32_t child_sleep(void *context) {
    struct timespec duration = {0, (long)(intptr_t)context};
    while (nanosleep(&duration, &duration) != 0 && errno == EINTR) {}
    return 41;
}

static int32_t child_pause(void *context) {
    (void)context;
    for (;;)
        pause();
    return 0;
}

typedef struct stream_read_context {
    const hl_host_services *services;
    hl_host_handle handle;
    unsigned char bytes[256];
    hl_host_result result;
} stream_read_context;

static void *stream_read_thread(void *opaque) {
    stream_read_context *reader = opaque;
    reader->result =
        reader->services->file->read(reader->services->context, reader->handle, reader->bytes, sizeof(reader->bytes));
    return NULL;
}

typedef struct stream_writer_context {
    const hl_host_services *services;
    hl_host_handle handle;
    unsigned char byte;
    uint32_t records;
    uint32_t failed;
} stream_writer_context;

static void *stream_writer_thread(void *opaque) {
    stream_writer_context *writer = opaque;
    unsigned char record[64];
    memset(record, writer->byte, sizeof record);
    for (uint32_t index = 0; index < writer->records; ++index) {
        hl_host_result result = writer->services->stream->write(writer->services->context, writer->handle,
                                                                (hl_host_const_bytes){record, sizeof record});
        if (result.status != HL_STATUS_OK || result.value != sizeof record) {
            writer->failed = 1;
            break;
        }
    }
    return NULL;
}

typedef struct process_wait_context {
    const hl_host_services *services;
    hl_host_handle process;
    hl_host_result result;
} process_wait_context;

typedef struct clock_interrupt_context {
    pthread_t target;
} clock_interrupt_context;

static void clock_interrupt_handler(int signal_number) {
    (void)signal_number;
}

static void *interrupt_clock_sleep(void *opaque) {
    const clock_interrupt_context *interrupt = opaque;
    const struct timespec delay = {0, 20 * 1000 * 1000};
    (void)nanosleep(&delay, NULL);
    (void)pthread_kill(interrupt->target, SIGUSR1);
    return NULL;
}

static void *wait_for_process(void *opaque) {
    process_wait_context *waiter = opaque;
    waiter->result =
        waiter->services->process->wait(waiter->services->context, waiter->process, HL_HOST_DEADLINE_INFINITE);
    return NULL;
}

static size_t private_descriptor_count(void) {
    hl_host_process_fd entries[1024];
    size_t count = 0;
    size_t private_count = 0;
    HL_CHECK(hl_host_process_fds(getpid(), entries, 1024, &count));
    HL_CHECK(count <= 1024);
    for (size_t index = 0; index < count; ++index)
        if ((entries[index].flags & HL_HOST_PROCESS_FD_ENGINE_PRIVATE) != 0) private_count++;
    return private_count;
}

int main(void) {
    hl_host_macos *host;
    hl_host_services services;
    hl_host_code_mapping code;
    hl_host_result process;
    hl_host_result process_exit;
    hl_host_result file;
    char path[128];
    char moved_path[160];
    char contents[3] = {0};
    HL_CHECK(hl_host_macos_create(&host, &services) == HL_STATUS_OK);
    {
        /* What a mapping handle covers after a partial unmap_range.
         *
         * A partial unmap keeps the handle -- only a full-range unmap consumes it -- but the
         * subrange it gave back has to leave the handle's coverage, because the registry answers
         * the ownership question from it. While the hole stayed inside the handle's frame, the
         * registry claimed memory the owner no longer had: a genuinely vacant address was refused
         * with BUSY, and a whole-handle teardown would have unmapped whatever the address space
         * had placed in that hole in the meantime.
         *
         * The addressing frame itself cannot move. protect, sync and unmap_range are all keyed on
         * an offset from the base the mapping was placed at, so shrinking the frame would shift
         * every one of them; what changes is which bytes of the frame are still held. */
        long frame_page_value = sysconf(_SC_PAGESIZE);
        uint64_t frame_page = frame_page_value > 0 ? (uint64_t)frame_page_value : UINT64_C(16384);
        hl_host_memory_mapping frame = {HL_HOST_MEMORY_MAPPING_ABI, sizeof(frame), 0, 0, 0, 0};
        hl_host_memory_mapping tenant = {HL_HOST_MEMORY_MAPPING_ABI, sizeof(tenant), 0, 0, 0, 0};
        uint64_t base;
        HL_CHECK(services.memory
                     ->map_anonymous(services.context, 0, frame_page * 3, HL_HOST_MEMORY_READ | HL_HOST_MEMORY_WRITE,
                                     HL_HOST_MEMORY_PRIVATE, &frame)
                     .status == HL_STATUS_OK);
        base = frame.address;
        HL_CHECK(services.memory->unmap_range(services.context, frame.handle, frame_page, frame_page).status ==
                 HL_STATUS_OK);
        HL_CHECK(msync((void *)(uintptr_t)base, (size_t)frame_page, MS_ASYNC) == 0);
        HL_CHECK(msync((void *)(uintptr_t)(base + frame_page), (size_t)frame_page, MS_ASYNC) != 0);
        HL_CHECK(msync((void *)(uintptr_t)(base + frame_page * 2), (size_t)frame_page, MS_ASYNC) == 0);
        /* The handle survived, and offsets still mean what they meant when it was created. */
        HL_CHECK(services.memory
                     ->protect(services.context, frame.handle, frame_page * 2, frame_page, HL_HOST_MEMORY_READ)
                     .status == HL_STATUS_OK);
        /* The hole is vacant, so an address-keyed release of it must succeed. */
        HL_CHECK(services.memory->unmap_address(services.context, base + frame_page, frame_page).status ==
                 HL_STATUS_OK);
        /* What the handle kept is still refused, on either side of the hole and across all three. */
        HL_CHECK(services.memory->unmap_address(services.context, base, frame_page).status == HL_STATUS_BUSY);
        HL_CHECK(services.memory->unmap_address(services.context, base + frame_page * 2, frame_page).status ==
                 HL_STATUS_BUSY);
        HL_CHECK(services.memory->unmap_address(services.context, base, frame_page * 3).status == HL_STATUS_BUSY);
        /* A placement is free to take the hole; the exact-address form proves it really is vacant. */
        HL_CHECK(services.memory
                     ->map_anonymous(services.context, base + frame_page, frame_page,
                                     HL_HOST_MEMORY_READ | HL_HOST_MEMORY_WRITE,
                                     HL_HOST_MEMORY_PRIVATE | HL_HOST_MEMORY_FIXED_NOREPLACE, &tenant)
                     .status == HL_STATUS_OK &&
                 tenant.address == base + frame_page);
        ((unsigned char *)(uintptr_t)tenant.address)[0] = 0xc3;
        /* Busy again, for the right reason: a second handle holds it now. */
        HL_CHECK(services.memory->unmap_address(services.context, tenant.address, frame_page).status ==
                 HL_STATUS_BUSY);
        /* Tearing the first handle down gives back only what it still held. */
        HL_CHECK(services.memory->release(services.context, frame.handle).status == HL_STATUS_OK);
        HL_CHECK(msync((void *)(uintptr_t)base, (size_t)frame_page, MS_ASYNC) != 0);
        HL_CHECK(msync((void *)(uintptr_t)(base + frame_page * 2), (size_t)frame_page, MS_ASYNC) != 0);
        HL_CHECK(msync((void *)(uintptr_t)tenant.address, (size_t)frame_page, MS_ASYNC) == 0);
        HL_CHECK(((unsigned char *)(uintptr_t)tenant.address)[0] == 0xc3);
        HL_CHECK(services.memory->release(services.context, tenant.handle).status == HL_STATUS_OK);

        /* Piecewise complete: when the last held byte goes the handle is consumed exactly as a
         * single full-range unmap consumes it, so a live mapping handle always holds a byte. */
        frame = (hl_host_memory_mapping){HL_HOST_MEMORY_MAPPING_ABI, sizeof(frame), 0, 0, 0, 0};
        HL_CHECK(services.memory
                     ->map_anonymous(services.context, 0, frame_page * 2, HL_HOST_MEMORY_READ | HL_HOST_MEMORY_WRITE,
                                     HL_HOST_MEMORY_PRIVATE, &frame)
                     .status == HL_STATUS_OK);
        HL_CHECK(services.memory->unmap_range(services.context, frame.handle, frame_page, frame_page).status ==
                 HL_STATUS_OK);
        HL_CHECK(services.memory->unmap_range(services.context, frame.handle, 0, frame_page).status == HL_STATUS_OK);
        HL_CHECK(services.memory->release(services.context, frame.handle).status == HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.memory->unmap_address(services.context, frame.address, frame_page * 2).status ==
                 HL_STATUS_OK);
    }
    {
        /* Address-keyed release and range wiring, appended in HL_HOST_MEMORY_ABI 7. Both populations that
         * need an address key hold no mapping handle: a range a provider placed at a fixed address, and a
         * range whose ownership handle a later fixed placement retired. discard() reproduces the second.
         *
         * BUSY is answered from the registry rather than from the address space, so the case is set up
         * from mappings this block owns end to end rather than from whatever earlier blocks left behind. */
        long page_value = sysconf(_SC_PAGESIZE);
        uint64_t page = page_value > 0 ? (uint64_t)page_value : UINT64_C(16384);
        hl_host_memory_mapping owned = {HL_HOST_MEMORY_MAPPING_ABI, sizeof(owned), 0, 0, 0, 0};
        hl_host_memory_mapping wired_map = {HL_HOST_MEMORY_MAPPING_ABI, sizeof(wired_map), 0, 0, 0, 0};
        hl_host_result wired;
        uint64_t base;
        HL_CHECK(services.memory->abi == HL_HOST_MEMORY_ABI && services.memory->size >= sizeof(*services.memory));
        HL_CHECK(services.memory->unmap_address(services.context, 0, page).status == HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.memory->unmap_address(services.context, page, 0).status == HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.memory->unmap_address(services.context, page + 1, page).status == HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.memory->unmap_address(services.context, page, page - 1).status == HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.memory->unmap_address(services.context, UINT64_MAX - page + 1, page * 2).status ==
                 HL_STATUS_INVALID_ARGUMENT);

        HL_CHECK(services.memory
                     ->map_anonymous(services.context, 0, page * 2, HL_HOST_MEMORY_READ | HL_HOST_MEMORY_WRITE,
                                     HL_HOST_MEMORY_PRIVATE, &owned)
                     .status == HL_STATUS_OK);
        base = owned.address;
        /* A live handle refuses the whole range, whether the overlap is total or partial, and nothing moves. */
        HL_CHECK(services.memory->unmap_address(services.context, base, page * 2).status == HL_STATUS_BUSY);
        HL_CHECK(services.memory->unmap_address(services.context, base + page, page).status == HL_STATUS_BUSY);
        ((unsigned char *)(uintptr_t)base)[0] = 0xc3;
        HL_CHECK(msync((void *)(uintptr_t)base, (size_t)(page * 2), MS_ASYNC) == 0);

        /* Retiring the handle leaves the address space untouched: that is the range gmap is left holding. */
        HL_CHECK(services.memory->discard(services.context, owned.handle).status == HL_STATUS_OK);
        HL_CHECK(((unsigned char *)(uintptr_t)base)[0] == 0xc3);
        HL_CHECK(services.memory->unmap_address(services.context, base, page).status == HL_STATUS_OK);
        HL_CHECK(msync((void *)(uintptr_t)base, (size_t)page, MS_ASYNC) != 0);
        HL_CHECK(msync((void *)(uintptr_t)(base + page), (size_t)page, MS_ASYNC) == 0);
        /* A vacant range succeeds, exactly as munmap does, so a repeated teardown is not an error. */
        HL_CHECK(services.memory->unmap_address(services.context, base, page).status == HL_STATUS_OK);
        HL_CHECK(services.memory->unmap_address(services.context, base + page, page).status == HL_STATUS_OK);
        /* The retired handle stays retired and never becomes an alias for the reused address. */
        HL_CHECK(services.memory->unmap_range(services.context, owned.handle, 0, page).status ==
                 HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.memory->release(services.context, owned.handle).status == HL_STATUS_INVALID_ARGUMENT);

        HL_CHECK(services.memory
                     ->map_anonymous(services.context, 0, page, HL_HOST_MEMORY_READ | HL_HOST_MEMORY_WRITE,
                                     HL_HOST_MEMORY_PRIVATE, &wired_map)
                     .status == HL_STATUS_OK);
        HL_CHECK(services.memory->wire_range(services.context, wired_map.address, page, 1).status ==
                 HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.memory->wire_range(services.context, 0, page, 0).status == HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.memory->wire_range(services.context, wired_map.address + 1, page, 0).status ==
                 HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.memory->wire_range(services.context, wired_map.address, 0, 0).status ==
                 HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.memory->unwire_range(services.context, 0, page).status == HL_STATUS_INVALID_ARGUMENT);
        wired = services.memory->wire_range(services.context, wired_map.address, page, 0);
        /* Darwin has mlock(2), so this host never answers NOT_SUPPORTED; an exhausted RLIMIT_MEMLOCK stays
         * a refusal and is never reported as a success that pinned nothing. */
        HL_CHECK(wired.status != HL_STATUS_NOT_SUPPORTED);
        if (wired.status == HL_STATUS_OK) {
            HL_CHECK(wired.detail == (uint64_t)HL_HOST_WIRE_RESIDENT);
            HL_CHECK(services.memory->unwire_range(services.context, wired_map.address, page).status == HL_STATUS_OK);
        }
        HL_CHECK(services.memory->release(services.context, wired_map.handle).status == HL_STATUS_OK);
        if (msync((void *)(uintptr_t)wired_map.address, (size_t)page, MS_ASYNC) != 0)
            HL_CHECK(services.memory->wire_range(services.context, wired_map.address, page, 0).status != HL_STATUS_OK);
    }
    {
        /* Address-keyed protection and flush, appended in HL_HOST_MEMORY_ABI 8.
         *
         * The ruling this block pins down: protect_address does NOT inherit unmap_address's
         * live-handle refusal. Unmapping a range a handle still holds leaves that handle claiming
         * address space that no longer exists, and a later teardown then unmaps whatever replaced
         * it. Re-protecting leaves the frame, the hole set, the contents and the handle exactly as
         * they were, and the owner can put the protection back through the handle-keyed call -- so
         * refusing would refuse the ordinary case, which is what mprotect is for. The one range
         * that keeps the refusal is a code mapping, where the protection is an engine invariant
         * made of two views and an address-keyed caller holds neither the handle nor the other. */
        long page_value = sysconf(_SC_PAGESIZE);
        uint64_t page = page_value > 0 ? (uint64_t)page_value : UINT64_C(16384);
        hl_host_memory_mapping owned = {HL_HOST_MEMORY_MAPPING_ABI, sizeof(owned), 0, 0, 0, 0};
        hl_host_memory_mapping loose = {HL_HOST_MEMORY_MAPPING_ABI, sizeof(loose), 0, 0, 0, 0};
        hl_host_code_mapping executable;

        HL_CHECK(services.memory->protect_address != NULL && services.memory->sync_address != NULL);
        HL_CHECK(services.memory->protect_address(services.context, 0, page, HL_HOST_MEMORY_READ).status ==
                 HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.memory->protect_address(services.context, page, 0, HL_HOST_MEMORY_READ).status ==
                 HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.memory->protect_address(services.context, page + 1, page, HL_HOST_MEMORY_READ).status ==
                 HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.memory->protect_address(services.context, page, page, UINT32_MAX).status ==
                 HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.memory->protect_address(services.context, UINT64_MAX - page + 1, page * 2,
                                                  HL_HOST_MEMORY_READ)
                     .status == HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.memory->sync_address(services.context, 0, page, 0).status == HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.memory->sync_address(services.context, page, 0, 0).status == HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.memory->sync_address(services.context, page + 1, page, 0).status ==
                 HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.memory->sync_address(services.context, page, page, UINT32_MAX).status ==
                 HL_STATUS_INVALID_ARGUMENT);

        HL_CHECK(services.memory
                     ->map_anonymous(services.context, 0, page * 2, HL_HOST_MEMORY_READ | HL_HOST_MEMORY_WRITE,
                                     HL_HOST_MEMORY_PRIVATE, &owned)
                     .status == HL_STATUS_OK);
        ((unsigned char *)(uintptr_t)owned.address)[0] = 0x5a;
        /* A live ordinary handle covers this range, and the call still goes through. */
        HL_CHECK(services.memory->protect_address(services.context, owned.address, page, HL_HOST_MEMORY_READ).status ==
                 HL_STATUS_OK);
        /* Nothing was taken away: the range is still mapped, still readable, and the handle can put
         * the protection back through the offset-keyed call it has always used. */
        HL_CHECK(msync((void *)(uintptr_t)owned.address, (size_t)page, MS_ASYNC) == 0);
        HL_CHECK(((unsigned char *)(uintptr_t)owned.address)[0] == 0x5a);
        HL_CHECK(services.memory
                     ->protect(services.context, owned.handle, 0, page * 2,
                               HL_HOST_MEMORY_READ | HL_HOST_MEMORY_WRITE)
                     .status == HL_STATUS_OK);
        ((unsigned char *)(uintptr_t)owned.address)[0] = 0x5b;
        /* An unaligned length is rounded up to whole pages, as the host operation itself does. */
        HL_CHECK(services.memory->protect_address(services.context, owned.address, page + 1,
                                                  HL_HOST_MEMORY_READ | HL_HOST_MEMORY_WRITE)
                     .status == HL_STATUS_OK);
        HL_CHECK(services.memory->sync_address(services.context, owned.address, page * 2, 0).status == HL_STATUS_OK);
        HL_CHECK(services.memory
                     ->sync_address(services.context, owned.address, page * 2, HL_HOST_MEMORY_SYNC_ASYNC)
                     .status == HL_STATUS_OK);
        HL_CHECK(services.memory->release(services.context, owned.handle).status == HL_STATUS_OK);

        /* The population the blocked caller actually holds: pages whose ownership handle is gone. */
        HL_CHECK(services.memory
                     ->map_anonymous(services.context, 0, page, HL_HOST_MEMORY_READ | HL_HOST_MEMORY_WRITE,
                                     HL_HOST_MEMORY_PRIVATE, &loose)
                     .status == HL_STATUS_OK);
        HL_CHECK(services.memory->discard(services.context, loose.handle).status == HL_STATUS_OK);
        HL_CHECK(services.memory->protect_address(services.context, loose.address, page, HL_HOST_MEMORY_READ).status ==
                 HL_STATUS_OK);
        HL_CHECK(services.memory->unmap_address(services.context, loose.address, page).status == HL_STATUS_OK);
        /* A range with nothing mapped is refused rather than silently succeeding: a guest whose
         * protection change reports success without changing anything turns a fault into a store. */
        HL_CHECK(services.memory->protect_address(services.context, loose.address, page, HL_HOST_MEMORY_READ).status !=
                 HL_STATUS_OK);

        /* Code mappings keep the refusal, whole and with nothing changed. */
        memset(&executable, 0, sizeof(executable));
        HL_CHECK(services.memory->reserve_code(services.context, page, page, 0, &executable).status == HL_STATUS_OK);
        HL_CHECK(services.memory
                     ->protect_address(services.context, executable.executable_address, page, HL_HOST_MEMORY_READ)
                     .status == HL_STATUS_BUSY);
        HL_CHECK(services.memory
                     ->protect_address(services.context, executable.writable_address, page, HL_HOST_MEMORY_READ)
                     .status == HL_STATUS_BUSY);
        /* Flushing it is not a protection change and is not refused. */
        HL_CHECK(services.memory->sync_address(services.context, executable.writable_address, page, 0).status ==
                 HL_STATUS_OK);
        HL_CHECK(services.memory->release(services.context, executable.handle).status == HL_STATUS_OK);
    }
    {
        /* The parking trio, appended in HL_HOST_SYNC_ABI 3. This host has no address-keyed wait
         * exposed to userspace, so it runs the portable arm: process-local queues, which serve the
         * private tier honestly and cannot serve the shared one at all. That refusal is typed. */
        uint32_t word = 0;
        uint64_t key = (uint64_t)(uintptr_t)&word;
        pthread_t worker;
        park_probe probe;
        hl_host_result parked;

        HL_CHECK(services.sync->abi == HL_HOST_SYNC_ABI && services.sync->park != NULL &&
                 services.sync->unpark != NULL && services.sync->interrupt_park != NULL);

        HL_CHECK(services.sync->park(services.context, 0, HL_HOST_PARK_PRIVATE, key, &word, 0, 4, 0).status ==
                 HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.sync->park(services.context, 1, HL_HOST_PARK_PRIVATE, key, NULL, 0, 4, 0).status ==
                 HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.sync->park(services.context, 1, 7u, key, &word, 0, 4, 0).status ==
                 HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.sync->park(services.context, 1, HL_HOST_PARK_PRIVATE, key, &word, 0, 3, 0).status ==
                 HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.sync->park(services.context, 1, HL_HOST_PARK_PRIVATE, key, (const char *)&word + 1, 0, 4, 0)
                     .status == HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.sync->unpark(services.context, HL_HOST_PARK_PRIVATE, key, NULL, 1).status ==
                 HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.sync->unpark(services.context, 9u, key, &word, 1).status == HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.sync->interrupt_park(services.context, 0).status == HL_STATUS_INVALID_ARGUMENT);
        /* Typed absence rather than a wait that is private while claiming to be shared. */
        HL_CHECK(services.sync->park(services.context, 1, HL_HOST_PARK_SHARED, key, &word, 0, 4, 0).status ==
                 HL_STATUS_NOT_SUPPORTED);
        HL_CHECK(services.sync->unpark(services.context, HL_HOST_PARK_SHARED, key, &word, 1).status ==
                 HL_STATUS_NOT_SUPPORTED);

        /* The compare is the provider's, and it happens before anything is enqueued. */
        HL_CHECK(services.sync->park(services.context, 1, HL_HOST_PARK_PRIVATE, key, &word, 99, 4,
                                     HL_HOST_DEADLINE_INFINITE)
                     .status == HL_STATUS_WOULD_BLOCK);
        HL_CHECK(services.sync->park(services.context, 1, HL_HOST_PARK_PRIVATE, key, &word, 0, 4, 1).status ==
                 HL_STATUS_TIMED_OUT);
        parked = services.sync->unpark(services.context, HL_HOST_PARK_PRIVATE, key, &word, 1);
        HL_CHECK(parked.status == HL_STATUS_OK);

        /* An interruption recorded before the block is consumed by it and never blocks. An infinite
         * deadline is used deliberately: if the record were kept against an outstanding wait rather
         * than against the waiter, this call would never return. */
        HL_CHECK(services.sync->interrupt_park(services.context, 4242).status == HL_STATUS_OK);
        HL_CHECK(services.sync->park(services.context, 4242, HL_HOST_PARK_PRIVATE, key, &word, 0, 4,
                                     HL_HOST_DEADLINE_INFINITE)
                     .status == HL_STATUS_INTERRUPTED);
        HL_CHECK(services.sync->park(services.context, 4242, HL_HOST_PARK_PRIVATE, key, &word, 0, 4, 1).status ==
                 HL_STATUS_TIMED_OUT);

        /* Released by another thread, with the word changed under it. */
        probe = (park_probe){&services, 11, &word, monotonic_now_ns() + UINT64_C(5000000000), -1, 0};
        HL_CHECK(pthread_create(&worker, NULL, park_once, &probe) == 0);
        sleep_ms(80);
        __atomic_store_n(&word, 1u, __ATOMIC_RELEASE);
        HL_CHECK(services.sync->unpark(services.context, HL_HOST_PARK_PRIVATE, key, &word, 1).status == HL_STATUS_OK);
        HL_CHECK(pthread_join(worker, NULL) == 0);
        HL_CHECK(probe.status == HL_STATUS_OK);

        /* Interrupted while blocked, with the word deliberately unchanged. This is the case a wait
         * that can only be woken by a value change cannot express, and it is what carries a guest
         * signal to a thread sitting in a wait nobody was going to end. */
        __atomic_store_n(&word, 0u, __ATOMIC_RELEASE);
        probe = (park_probe){&services, 12, &word, monotonic_now_ns() + UINT64_C(5000000000), -1, 0};
        HL_CHECK(pthread_create(&worker, NULL, park_once, &probe) == 0);
        sleep_ms(80);
        HL_CHECK(services.sync->interrupt_park(services.context, 12).status == HL_STATUS_OK);
        HL_CHECK(pthread_join(worker, NULL) == 0);
        HL_CHECK(probe.status == HL_STATUS_INTERRUPTED && __atomic_load_n(&word, __ATOMIC_ACQUIRE) == 0);
    }
    {
        /* The terminal group. What it answers is device facts; what it must never be asked is
         * anything Linux-shaped, and nothing here has a shape to pass one. */
        hl_host_terminal_size window;
        uint32_t mode = 0;
        hl_host_result ordinary;
        char terminal_path[128];

        HL_CHECK((services.capabilities & HL_HOST_CAP_TERMINAL) != 0 && services.terminal != NULL &&
                 services.terminal->abi == HL_HOST_TERMINAL_ABI);
        HL_CHECK(hl_host_services_validate(&services, HL_HOST_CAP_TERMINAL) == HL_STATUS_OK);
        HL_CHECK(services.terminal->probe(services.context, HL_HOST_HANDLE_INVALID).status ==
                 HL_STATUS_INVALID_ARGUMENT);

        snprintf(terminal_path, sizeof(terminal_path), "/tmp/hl_terminal_probe_%ld", (long)getpid());
        ordinary = services.file->open_relative(services.context, HL_HOST_HANDLE_CWD, terminal_path,
                                                strlen(terminal_path), HL_HOST_FILE_READ | HL_HOST_FILE_WRITE,
                                                HL_HOST_FILE_CREATE | HL_HOST_FILE_TRUNCATE, 0600);
        HL_CHECK(ordinary.status == HL_STATUS_OK);
        /* The point of the group: a live, valid, entirely ordinary object that is not a terminal.
         * A file-type field cannot separate these two cases on every host; this can. */
        HL_CHECK(services.terminal->probe(services.context, ordinary.value).status == HL_STATUS_OK &&
                 services.terminal->probe(services.context, ordinary.value).value == 0);
        HL_CHECK(services.terminal->get_mode(services.context, ordinary.value, &mode).status != HL_STATUS_OK);
        HL_CHECK(services.terminal->get_size(services.context, ordinary.value, &window).status != HL_STATUS_OK);
        HL_CHECK(services.terminal->get_mode(services.context, ordinary.value, NULL).status ==
                 HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.terminal->get_size(services.context, ordinary.value, NULL).status ==
                 HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.terminal->set_size(services.context, ordinary.value, NULL).status ==
                 HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.terminal->set_mode(services.context, ordinary.value, UINT32_MAX).status ==
                 HL_STATUS_INVALID_ARGUMENT);
        /* Typed absence, not a missing callback: this host delivers a resize by a means the
         * operation does not describe. */
        HL_CHECK(services.terminal->size_change_event(services.context, ordinary.value).status ==
                 HL_STATUS_NOT_SUPPORTED);
        HL_CHECK(services.file->close(services.context, ordinary.value).status == HL_STATUS_OK);
        /* Stale: the handle is gone and every operation says so rather than reaching a reused slot. */
        HL_CHECK(services.terminal->probe(services.context, ordinary.value).status == HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.terminal->size_change_event(services.context, ordinary.value).status ==
                 HL_STATUS_INVALID_ARGUMENT);
        (void)services.file->unlink_relative(services.context, HL_HOST_HANDLE_CWD, terminal_path,
                                             strlen(terminal_path));
    }
    {
        hl_host_result root = services.file->open_relative(services.context, HL_HOST_HANDLE_CWD, "/", 1,
                                                           HL_HOST_FILE_PATH_ONLY | HL_HOST_FILE_DIRECTORY, 0, 0);
        struct stat status;
        hl_host_result borrowed;
        HL_CHECK(root.status == HL_STATUS_OK);
        borrowed = services.posix_attachment->borrow_file_at_least(services.context, root.value, 64);
        HL_CHECK(borrowed.status == HL_STATUS_OK && borrowed.value >= 64 && borrowed.value <= INT_MAX);
        HL_CHECK(fcntl((int)borrowed.value, F_GETFD) & FD_CLOEXEC);
        HL_CHECK(fstat((int)borrowed.value, &status) == 0 && S_ISDIR(status.st_mode));
        HL_CHECK(services.posix_attachment->release(services.context, borrowed.value).status == HL_STATUS_OK);
        HL_CHECK(services.posix_attachment->borrow_file_at_least(services.context, HL_HOST_HANDLE_INVALID, 64).status ==
                 HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.file->close(services.context, root.value).status == HL_STATUS_OK);
    }
    {
        char range_path[128];
        hl_host_filesystem_metadata filesystem;
        hl_host_file_metadata range_metadata;
        hl_host_result range_file;
        snprintf(range_path, sizeof(range_path), "/tmp/hl_file_abi14_macos_%ld", (long)getpid());
        range_file = services.file->open_relative(services.context, HL_HOST_HANDLE_CWD, range_path, strlen(range_path),
                                                  HL_HOST_FILE_READ | HL_HOST_FILE_WRITE,
                                                  HL_HOST_FILE_CREATE | HL_HOST_FILE_EXCLUSIVE, 0600);
        HL_CHECK(range_file.status == HL_STATUS_OK && services.file->abi == HL_HOST_FILE_ABI);
        HL_CHECK(services.file->allocate_range(services.context, range_file.value, 0, 0, 8192).status == HL_STATUS_OK);
        HL_CHECK(services.file->metadata(services.context, range_file.value, &range_metadata).status == HL_STATUS_OK &&
                 range_metadata.size == 8192);
        HL_CHECK(services.file->allocate_range(services.context, range_file.value, 0, 8192, 8192).status ==
                 HL_STATUS_OK);
        HL_CHECK(services.file->metadata(services.context, range_file.value, &range_metadata).status == HL_STATUS_OK &&
                 range_metadata.size == 16384 && range_metadata.allocated_size <= 16384);
        HL_CHECK(services.file->filesystem_metadata(services.context, range_file.value, &filesystem).status ==
                     HL_STATUS_OK &&
                 filesystem.block_size > 0 && filesystem.blocks > 0 && filesystem.blocks_free <= filesystem.blocks);
        HL_CHECK(services.file->close(services.context, range_file.value).status == HL_STATUS_OK &&
                 unlink(range_path) == 0);
    }
    {
        hl_host_result stream = services.file->standard_stream(services.context, HL_HOST_STANDARD_OUTPUT);
        HL_CHECK(stream.status == HL_STATUS_OK && (stream.detail & HL_HOST_FILE_WRITE) != 0);
        HL_CHECK(services.file->close(services.context, stream.value).status == HL_STATUS_OK);
        HL_CHECK(services.file->standard_stream(services.context, 3).status == HL_STATUS_INVALID_ARGUMENT);
    }
    {
        char probe_path[128];
        char first = 0;
        char second = 0;
        int saved = dup(STDIN_FILENO);
        int descriptor;
        hl_host_result stream;
        snprintf(probe_path, sizeof(probe_path), "/tmp/hl_stdio_macos_%ld", (long)getpid());
        descriptor = open(probe_path, O_CREAT | O_EXCL | O_RDWR | O_APPEND | O_NONBLOCK, 0600);
        HL_CHECK(saved >= 0 && descriptor >= 0 && write(descriptor, "ab", 2) == 2 &&
                 lseek(descriptor, 0, SEEK_SET) == 0 && dup2(descriptor, STDIN_FILENO) == STDIN_FILENO);
        stream = services.file->standard_stream(services.context, HL_HOST_STANDARD_INPUT);
        HL_CHECK(stream.status == HL_STATUS_OK && (stream.detail & HL_HOST_FILE_READ) != 0 &&
                 (stream.detail & (HL_HOST_FILE_APPEND | HL_HOST_FILE_NONBLOCK)) ==
                     (HL_HOST_FILE_APPEND | HL_HOST_FILE_NONBLOCK));
        HL_CHECK(services.file->read(services.context, stream.value, &first, 1).value == 1 && first == 'a');
        HL_CHECK(read(STDIN_FILENO, &second, 1) == 1 && second == 'b');
        HL_CHECK(services.file->close(services.context, stream.value).status == HL_STATUS_OK);
        HL_CHECK(dup2(saved, STDIN_FILENO) == STDIN_FILENO && close(saved) == 0 && close(descriptor) == 0 &&
                 unlink(probe_path) == 0);
    }
    HL_CHECK(check_counter(&services) == 0);
    HL_CHECK(check_transfer_fork(&services) == 0);
    HL_CHECK(hl_host_services_validate(&services, HL_HOST_CAP_MEMORY | HL_HOST_CAP_CLOCK | HL_HOST_CAP_PROCESS |
                                                      HL_HOST_CAP_EVENT_TIMER | HL_HOST_CAP_SHARED_MEMORY |
                                                      HL_HOST_CAP_CODE_MAPPING | HL_HOST_CAP_SYNC) == HL_STATUS_OK);
    {
        size_t private_before = private_descriptor_count();
        hl_host_result pollset = services.event->create(services.context);
        HL_CHECK(pollset.status == HL_STATUS_OK);
        HL_CHECK(private_descriptor_count() == private_before + 1);
        hl_host_event_record event;
        uint64_t deadline;
        HL_CHECK(pollset.status == HL_STATUS_OK);
        deadline = services.clock->monotonic_ns(services.context).value + UINT64_C(5000000);
        HL_CHECK(services.event->arm_timer(services.context, pollset.value, 91, deadline, 0).status == HL_STATUS_OK);
        {
            hl_host_result ready =
                services.event->wait(services.context, pollset.value, &event, 1, deadline + UINT64_C(1000000000));
            HL_CHECK(ready.status == HL_STATUS_OK && ready.value == 1 && event.token == 91 &&
                     (event.readiness & HL_HOST_READY_TIMER) != 0);
        }
        deadline = services.clock->monotonic_ns(services.context).value + UINT64_C(5000000);
        HL_CHECK(services.event->arm_timer(services.context, pollset.value, 92, deadline, UINT64_C(5000000)).status ==
                 HL_STATUS_OK);
        HL_CHECK(
            services.event->wait(services.context, pollset.value, &event, 1, deadline + UINT64_C(1000000000)).value ==
            1);
        HL_CHECK(services.event->disarm_timer(services.context, pollset.value, 92).status == HL_STATUS_OK);
        HL_CHECK(services.event->disarm_timer(services.context, pollset.value, 92).status == HL_STATUS_NOT_FOUND);
        HL_CHECK(services.event->wake(services.context, pollset.value).status == HL_STATUS_OK);
        HL_CHECK(services.event
                     ->wait(services.context, pollset.value, &event, 1,
                            services.clock->monotonic_ns(services.context).value + UINT64_C(100000000))
                     .value == 0);
        HL_CHECK(services.event->close(services.context, pollset.value).status == HL_STATUS_OK);
        HL_CHECK(private_descriptor_count() == private_before);
    }
    {
        static const char payload[] = "shared-memory";
        char readback[sizeof(payload)] = {0};
        hl_host_file_metadata metadata;
        hl_host_result shared = services.shared_memory->create(services.context, 4096, 0);
        hl_host_result copy;
        HL_CHECK(shared.status == HL_STATUS_OK && shared.value != HL_HOST_HANDLE_INVALID &&
                 shared.detail == shared.value);
        HL_CHECK(services.shared_memory->create(services.context, 4096, 1).status == HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.shared_memory->open(services.context, shared.detail, 1).status == HL_STATUS_INVALID_ARGUMENT);
        copy = services.shared_memory->open(services.context, shared.detail, 0);
        HL_CHECK(copy.status == HL_STATUS_OK && copy.value != shared.value && copy.detail == shared.detail);
        HL_CHECK(
            services.file->write_at(services.context, shared.value, 17, (hl_host_const_bytes){payload, sizeof(payload)})
                .value == sizeof(payload));
        HL_CHECK(services.file->read_at(services.context, copy.value, 17, (hl_host_bytes){readback, sizeof(readback)})
                     .value == sizeof(readback));
        HL_CHECK(memcmp(readback, payload, sizeof(payload)) == 0);
        HL_CHECK(services.shared_memory->resize(services.context, copy.value, 8192).status == HL_STATUS_OK);
        HL_CHECK(services.file->metadata(services.context, shared.value, &metadata).status == HL_STATUS_OK &&
                 metadata.type == HL_HOST_FILE_TYPE_REGULAR && metadata.size == 8192);
        HL_CHECK(services.shared_memory->close(services.context, shared.value).status == HL_STATUS_OK);
        HL_CHECK(services.shared_memory->open(services.context, shared.detail, 0).status == HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.shared_memory->resize(services.context, copy.value, 12288).status == HL_STATUS_OK);
        HL_CHECK(services.file->metadata(services.context, copy.value, &metadata).status == HL_STATUS_OK &&
                 metadata.size == 12288);
        HL_CHECK(services.shared_memory->close(services.context, copy.value).status == HL_STATUS_OK);
        HL_CHECK(services.shared_memory->resize(services.context, copy.value, 4096).status ==
                 HL_STATUS_INVALID_ARGUMENT);
    }
    {
        hl_host_result raw_before = services.clock->raw_monotonic_ns(services.context);
        hl_host_result process_before = services.clock->process_cpu_ns(services.context);
        hl_host_result thread_before = services.clock->thread_cpu_ns(services.context);
        volatile uint64_t work = 0;
        uint64_t index;
        hl_host_result raw_after;
        hl_host_result process_after;
        hl_host_result thread_after;
        hl_host_result deadline;
        struct sigaction action = {0};
        struct sigaction previous;
        clock_interrupt_context interrupt = {pthread_self()};
        pthread_t interrupter;

        HL_CHECK(raw_before.status == HL_STATUS_OK && process_before.status == HL_STATUS_OK &&
                 thread_before.status == HL_STATUS_OK);
        for (index = 0; index < UINT64_C(1000000); ++index)
            work += index;
        HL_CHECK(work != 0);
        raw_after = services.clock->raw_monotonic_ns(services.context);
        process_after = services.clock->process_cpu_ns(services.context);
        thread_after = services.clock->thread_cpu_ns(services.context);
        HL_CHECK(raw_after.status == HL_STATUS_OK && raw_after.value >= raw_before.value);
        HL_CHECK(process_after.status == HL_STATUS_OK && process_after.value > process_before.value);
        HL_CHECK(thread_after.status == HL_STATUS_OK && thread_after.value > thread_before.value);

        deadline = services.clock->monotonic_ns(services.context);
        HL_CHECK(deadline.status == HL_STATUS_OK);
        deadline.value += UINT64_C(5000000);
        HL_CHECK(services.clock->sleep_until(services.context, HL_HOST_CLOCK_MONOTONIC, deadline.value).status ==
                 HL_STATUS_OK);
        HL_CHECK(services.clock->monotonic_ns(services.context).value >= deadline.value);
        HL_CHECK(services.clock->sleep_until(services.context, HL_HOST_CLOCK_PROCESS_CPU, 0).status == HL_STATUS_OK);
        deadline = services.clock->realtime_ns(services.context);
        HL_CHECK(deadline.status == HL_STATUS_OK);
        HL_CHECK(services.clock->sleep_until(services.context, HL_HOST_CLOCK_REALTIME, deadline.value).status ==
                 HL_STATUS_OK);

        action.sa_handler = clock_interrupt_handler;
        HL_CHECK(sigemptyset(&action.sa_mask) == 0);
        HL_CHECK(sigaction(SIGUSR1, &action, &previous) == 0);
        deadline = services.clock->monotonic_ns(services.context);
        HL_CHECK(deadline.status == HL_STATUS_OK);
        HL_CHECK(pthread_create(&interrupter, NULL, interrupt_clock_sleep, &interrupt) == 0);
        HL_CHECK(services.clock
                     ->sleep_until(services.context, HL_HOST_CLOCK_MONOTONIC, deadline.value + UINT64_C(2000000000))
                     .status == HL_STATUS_INTERRUPTED);
        HL_CHECK(pthread_join(interrupter, NULL) == 0);
        deadline = services.clock->monotonic_ns(services.context);
        HL_CHECK(pthread_create(&interrupter, NULL, interrupt_clock_sleep, &interrupt) == 0);
        HL_CHECK(services.clock->backoff_ns(services.context, UINT64_C(100000000)).status == HL_STATUS_OK);
        HL_CHECK(pthread_join(interrupter, NULL) == 0);
        HL_CHECK(services.clock->monotonic_ns(services.context).value >= deadline.value + UINT64_C(100000000));
        HL_CHECK(sigaction(SIGUSR1, &previous, NULL) == 0);
    }
    {
        static const char message[] = {'h', 'o', 's', 't', '\0', 'l', 'o', 'g'};
        char received[sizeof(message)] = {0};
        int descriptors[2];
        int saved_stderr;
        ssize_t count;
        HL_CHECK(pipe(descriptors) == 0);
        saved_stderr = dup(STDERR_FILENO);
        HL_CHECK(saved_stderr >= 0);
        HL_CHECK(dup2(descriptors[1], STDERR_FILENO) == STDERR_FILENO);
        HL_CHECK(close(descriptors[1]) == 0);
        services.log->emit(services.context, 0x8badf00du, message, sizeof(message));
        HL_CHECK(dup2(saved_stderr, STDERR_FILENO) == STDERR_FILENO);
        HL_CHECK(close(saved_stderr) == 0);
        count = read(descriptors[0], received, sizeof(received));
        HL_CHECK(count == (ssize_t)sizeof(received));
        HL_CHECK(memcmp(received, message, sizeof(message)) == 0);
        HL_CHECK(close(descriptors[0]) == 0);
    }
    {
        const uint32_t count = 65536;
        hl_host_handle *mutexes = calloc(count, sizeof(*mutexes));
        hl_host_handle stale;
        uint32_t index;
        HL_CHECK(mutexes != NULL);
        for (index = 0; index < count; ++index) {
            hl_host_result created = services.sync->mutex_create(services.context);
            HL_CHECK(created.status == HL_STATUS_OK);
            mutexes[index] = created.value;
        }
        stale = mutexes[0];
        HL_CHECK(services.sync->mutex_create(services.context).status == HL_STATUS_RESOURCE_LIMIT);
        for (index = 0; index < count; ++index) {
            HL_CHECK(services.sync->mutex_lock(services.context, mutexes[index]).status == HL_STATUS_OK);
            HL_CHECK(services.sync->mutex_unlock(services.context, mutexes[index]).status == HL_STATUS_OK);
            HL_CHECK(services.sync->mutex_close(services.context, mutexes[index]).status == HL_STATUS_OK);
        }
        {
            hl_host_result replacement = services.sync->mutex_create(services.context);
            HL_CHECK(replacement.status == HL_STATUS_OK && replacement.value != stale);
            HL_CHECK(services.sync->mutex_lock(services.context, stale).status == HL_STATUS_INVALID_ARGUMENT);
            HL_CHECK(services.sync->mutex_lock(services.context, replacement.value).status == HL_STATUS_OK);
            HL_CHECK(services.sync->mutex_unlock(services.context, replacement.value).status == HL_STATUS_OK);
            HL_CHECK(services.sync->mutex_close(services.context, replacement.value).status == HL_STATUS_OK);
        }
        free(mutexes);
    }
    {
        hl_host_result first = services.sync->mutex_create(services.context);
        hl_host_result second = services.sync->mutex_create(services.context);
        HL_CHECK(first.status == HL_STATUS_OK && second.status == HL_STATUS_OK && first.value != second.value);
        HL_CHECK(services.sync->mutex_lock(services.context, first.value).status == HL_STATUS_OK);
        HL_CHECK(services.sync->mutex_lock(services.context, second.value).status == HL_STATUS_OK);
        HL_CHECK(services.sync->mutex_close(services.context, first.value).status == HL_STATUS_BUSY);
        HL_CHECK(services.sync->mutex_unlock(services.context, first.value).status == HL_STATUS_OK);
        HL_CHECK(services.sync->mutex_unlock(services.context, second.value).status == HL_STATUS_OK);
        HL_CHECK(services.sync->mutex_close(services.context, first.value).status == HL_STATUS_OK);
        HL_CHECK(services.sync->mutex_close(services.context, second.value).status == HL_STATUS_OK);
        HL_CHECK(services.sync->mutex_lock(services.context, first.value).status == HL_STATUS_INVALID_ARGUMENT);
    }
    {
        struct timespec realtime;
        struct timespec monotonic;
        HL_CHECK(hl_production_clock_gettime(&services, HL_PRODUCTION_CLOCK_REALTIME, &realtime) == 0);
        HL_CHECK(hl_production_clock_gettime(&services, HL_PRODUCTION_CLOCK_MONOTONIC, &monotonic) == 0);
        HL_CHECK(realtime.tv_sec > 0 && monotonic.tv_sec > 0);
    }

    process = services.process->spawn_cloned(services.context, child_exit_37, (void *)(uintptr_t)37);
    HL_CHECK(process.status == HL_STATUS_OK && process.value != HL_HOST_HANDLE_INVALID);
    process_exit = services.process->wait(services.context, process.value, HL_HOST_DEADLINE_INFINITE);
    HL_CHECK(process_exit.status == HL_STATUS_OK && process_exit.detail == HL_HOST_PROCESS_EXIT_CODE &&
             process_exit.value == 37);
    HL_CHECK(services.process->close(services.context, process.value).status == HL_STATUS_OK);
    HL_CHECK(services.process->close(services.context, process.value).status == HL_STATUS_INVALID_ARGUMENT);

    HL_CHECK(services.sync->fork_prepare(services.context).status == HL_STATUS_OK);
    process = services.process->spawn_prepared(services.context, child_exit_37, (void *)(uintptr_t)37);
    HL_CHECK(process.status == HL_STATUS_OK && process.value != HL_HOST_HANDLE_INVALID);
    process_exit = services.process->wait(services.context, process.value, HL_HOST_DEADLINE_INFINITE);
    HL_CHECK(process_exit.status == HL_STATUS_OK && process_exit.detail == HL_HOST_PROCESS_EXIT_CODE &&
             process_exit.value == 37);
    HL_CHECK(services.process->close(services.context, process.value).status == HL_STATUS_OK);

    process = services.process->spawn_cloned(services.context, child_sleep, (void *)(intptr_t)150000000);
    HL_CHECK(process.status == HL_STATUS_OK);
    {
        uint64_t deadline = services.clock->monotonic_ns(services.context).value + UINT64_C(20000000);
        HL_CHECK(services.process->wait(services.context, process.value, deadline).status == HL_STATUS_WOULD_BLOCK);
        HL_CHECK(services.clock->monotonic_ns(services.context).value >= deadline);
    }
    process_exit = services.process->wait(services.context, process.value, HL_HOST_DEADLINE_INFINITE);
    HL_CHECK(process_exit.status == HL_STATUS_OK && process_exit.detail == HL_HOST_PROCESS_EXIT_CODE &&
             process_exit.value == 41);
    HL_CHECK(services.process->wait(services.context, process.value, 0).value == 41);
    HL_CHECK(services.process->close(services.context, process.value).status == HL_STATUS_OK);

    {
        process_wait_context first = {&services, 0, {0}};
        process_wait_context second = {&services, 0, {0}};
        pthread_t first_thread;
        pthread_t second_thread;
        process = services.process->spawn_cloned(services.context, child_pause, NULL);
        HL_CHECK(process.status == HL_STATUS_OK);
        first.process = process.value;
        second.process = process.value;
        HL_CHECK(pthread_create(&first_thread, NULL, wait_for_process, &first) == 0);
        HL_CHECK(pthread_create(&second_thread, NULL, wait_for_process, &second) == 0);
        HL_CHECK(services.process->close(services.context, process.value).status == HL_STATUS_BUSY);
        HL_CHECK(services.process->terminate(services.context, process.value, HL_HOST_PROCESS_TERMINATE_FORCE).status ==
                 HL_STATUS_OK);
        HL_CHECK(pthread_join(first_thread, NULL) == 0 && pthread_join(second_thread, NULL) == 0);
        HL_CHECK(first.result.status == HL_STATUS_OK && second.result.status == HL_STATUS_OK &&
                 first.result.detail == HL_HOST_PROCESS_EXIT_SIGNAL && second.result.detail == first.result.detail &&
                 first.result.value == SIGKILL && second.result.value == first.result.value);
        HL_CHECK(services.process->close(services.context, process.value).status == HL_STATUS_OK);
    }
    HL_CHECK(services.clock->monotonic_ns(services.context).status == HL_STATUS_OK);
    {
        hl_host_result mapping =
            services.memory->reserve(services.context, 16384, 16384, HL_HOST_MEMORY_READ | HL_HOST_MEMORY_WRITE);
        HL_CHECK(mapping.status == HL_STATUS_OK && mapping.value != HL_HOST_HANDLE_INVALID);
        HL_CHECK(services.memory->protect(services.context, mapping.value, 0, 16384, HL_HOST_MEMORY_READ).status ==
                 HL_STATUS_OK);
        HL_CHECK(services.memory->protect(services.context, mapping.value, 16384, 1, HL_HOST_MEMORY_READ).status ==
                 HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(services.memory->release(services.context, mapping.value).status == HL_STATUS_OK);
        HL_CHECK(services.memory->release(services.context, mapping.value).status == HL_STATUS_INVALID_ARGUMENT);
    }
    {
        long page_value = sysconf(_SC_PAGESIZE);
        uint64_t page = page_value > 0 ? (uint64_t)page_value : UINT64_C(16384);
        hl_host_memory_mapping anonymous = {HL_HOST_MEMORY_MAPPING_ABI, sizeof(anonymous), 0, 0, 0, 0};
        HL_CHECK(services.memory
                     ->map_anonymous(services.context, 0, page, HL_HOST_MEMORY_READ | HL_HOST_MEMORY_WRITE,
                                     HL_HOST_MEMORY_PRIVATE, &anonymous)
                     .status == HL_STATUS_OK);
        unsigned char *bytes = (unsigned char *)(uintptr_t)anonymous.address;
        bytes[0] = 0xa9;
        HL_CHECK(services.memory->discard(services.context, anonymous.handle).status == HL_STATUS_OK);
        HL_CHECK(bytes[0] == 0xa9 && munmap(bytes, (size_t)page) == 0);
        anonymous = (hl_host_memory_mapping){HL_HOST_MEMORY_MAPPING_ABI, sizeof(anonymous), 0, 0, 0, 0};
        HL_CHECK(services.memory
                     ->map_anonymous(services.context, 0, page, HL_HOST_MEMORY_READ | HL_HOST_MEMORY_WRITE,
                                     HL_HOST_MEMORY_SHARED, &anonymous)
                     .status == HL_STATUS_OK);
        bytes = (unsigned char *)(uintptr_t)anonymous.address;
        bytes[0] = 0;
        pid_t shared_child = fork();
        HL_CHECK(shared_child >= 0);
        if (shared_child == 0) {
            bytes[0] = 0x5a;
            _exit(0);
        }
        int shared_status = 0;
        HL_CHECK(waitpid(shared_child, &shared_status, 0) == shared_child && WIFEXITED(shared_status) &&
                 WEXITSTATUS(shared_status) == 0 && bytes[0] == 0x5a);
        HL_CHECK(services.memory->release(services.context, anonymous.handle).status == HL_STATUS_OK);
        HL_CHECK(services.memory
                     ->map_anonymous(services.context, UINT64_MAX - page + 2, page, HL_HOST_MEMORY_READ,
                                     HL_HOST_MEMORY_PRIVATE | HL_HOST_MEMORY_FIXED, &anonymous)
                     .status == HL_STATUS_INVALID_ARGUMENT);
    }
    {
        long host_page_value = sysconf(_SC_PAGESIZE);
        size_t host_page = host_page_value > 0 ? (size_t)host_page_value : 16384u;
        unsigned char *page = mmap(NULL, host_page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        HL_CHECK(page != MAP_FAILED);
        page[0] = 0x6d;
        page[host_page > 4096 ? 4096 : host_page - 1] = 0x37;
        HL_CHECK(mprotect(page, host_page, PROT_NONE) == 0);
        HL_CHECK(services.memory->repair_signal_page(services.context, (uint64_t)(uintptr_t)page, 4096,
                                                     HL_HOST_MEMORY_READ | HL_HOST_MEMORY_WRITE));
        HL_CHECK(page[0] == 0x6d && page[host_page > 4096 ? 4096 : host_page - 1] == 0x37);
        HL_CHECK(munmap(page, host_page) == 0);
        HL_CHECK(services.memory->repair_signal_page(services.context, (uint64_t)(uintptr_t)page + 1, 4096,
                                                     HL_HOST_MEMORY_READ | HL_HOST_MEMORY_WRITE) == 0);
        HL_CHECK(services.memory->repair_signal_page(services.context, (uint64_t)(uintptr_t)page, 8192,
                                                     HL_HOST_MEMORY_READ | HL_HOST_MEMORY_WRITE) == 0);
        signal_repair_probe probes[8];
        pthread_t threads[8];
        for (size_t index = 0; index < 8; ++index) {
            probes[index] = (signal_repair_probe){&services, (uint64_t)(uintptr_t)page, 0};
            HL_CHECK(pthread_create(&threads[index], NULL, repair_signal_page_once, &probes[index]) == 0);
        }
        for (size_t index = 0; index < 8; ++index)
            HL_CHECK(pthread_join(threads[index], NULL) == 0 && probes[index].result == 1);
        page[0] = 0x91;
        HL_CHECK(page[0] == 0x91 && munmap(page, host_page) == 0);
    }
    snprintf(path, sizeof(path), "/tmp/hl_host_macos_%ld", (long)getpid());
    file = services.file->open_relative(services.context, HL_HOST_HANDLE_CWD, path, strlen(path),
                                        HL_HOST_FILE_READ | HL_HOST_FILE_WRITE | HL_HOST_FILE_APPEND,
                                        HL_HOST_FILE_CREATE | HL_HOST_FILE_EXCLUSIVE, 0600);
    HL_CHECK(file.status == HL_STATUS_OK);
    {
        hl_host_file_mapping mapped = {HL_HOST_FILE_MAPPING_ABI, sizeof(mapped), 0, 0, 0, 0};
        unsigned char payload[8192];
        unsigned char *reservation;
        memset(payload, 0x5a, sizeof payload);
        HL_CHECK(
            services.file->write_at(services.context, file.value, 0, (hl_host_const_bytes){payload, sizeof payload})
                .value == sizeof payload);
        reservation = mmap(NULL, 32768, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        HL_CHECK(reservation != MAP_FAILED);
        memset(reservation, 0xa5, 4096);
        HL_CHECK(services.memory
                     ->map_file(services.context, file.value, (uint64_t)(uintptr_t)(reservation + 4096), 4096, 4096,
                                HL_HOST_MEMORY_READ, HL_HOST_MEMORY_PRIVATE | HL_HOST_MEMORY_FIXED, &mapped)
                     .status == HL_STATUS_OK);
        for (size_t index = 0; index < 4096; ++index)
            HL_CHECK(reservation[index] == 0xa5 && reservation[4096 + index] == 0x5a);
        HL_CHECK(mapped.address == (uint64_t)(uintptr_t)(reservation + 4096) && mapped.reserved == 4096);
        HL_CHECK(services.memory->release(services.context, mapped.handle).status == HL_STATUS_OK);
        HL_CHECK(munmap(reservation + 16384, 16384) == 0);
        reservation = mmap(NULL, 32768, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        HL_CHECK(reservation != MAP_FAILED);
        mapped = (hl_host_file_mapping){HL_HOST_FILE_MAPPING_ABI, sizeof(mapped), 0, 0, 0, 0};
        HL_CHECK(services.memory
                     ->map_file(services.context, file.value, (uint64_t)(uintptr_t)(reservation + 4096), 4096, 4096,
                                HL_HOST_MEMORY_READ, HL_HOST_MEMORY_SHARED | HL_HOST_MEMORY_FIXED, &mapped)
                     .status == HL_STATUS_INVALID_ARGUMENT);
        HL_CHECK(munmap(reservation, 32768) == 0);
        {
            long page_value = sysconf(_SC_PAGESIZE);
            size_t host_page = page_value > 0 ? (size_t)page_value : 16384u;
            unsigned char *occupied = mmap(NULL, host_page, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
            unsigned char *vacant = mmap(NULL, host_page, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
            hl_host_file_mapping exact = {HL_HOST_FILE_MAPPING_ABI, sizeof(exact), 0, 0, 0, 0};
            hl_host_file_mapping collision = {HL_HOST_FILE_MAPPING_ABI, sizeof(collision), 0, 0, 0, 0};
            HL_CHECK(occupied != MAP_FAILED && vacant != MAP_FAILED);
            memset(occupied, 0xa7, host_page);
            HL_CHECK(munmap(vacant, host_page) == 0);
            HL_CHECK(services.memory
                         ->map_file(services.context, file.value, (uint64_t)(uintptr_t)occupied, 0, host_page,
                                    HL_HOST_MEMORY_READ, HL_HOST_MEMORY_PRIVATE | HL_HOST_MEMORY_FIXED_NOREPLACE,
                                    &collision)
                         .status == HL_STATUS_ALREADY_EXISTS);
            HL_CHECK(occupied[0] == 0xa7 && occupied[host_page - 1] == 0xa7);
            HL_CHECK(services.memory
                         ->map_file(services.context, file.value, (uint64_t)(uintptr_t)vacant, 0, host_page,
                                    HL_HOST_MEMORY_READ, HL_HOST_MEMORY_PRIVATE | HL_HOST_MEMORY_FIXED_NOREPLACE,
                                    &exact)
                         .status == HL_STATUS_OK);
            HL_CHECK(exact.address == (uint64_t)(uintptr_t)vacant && vacant[0] == 0x5a);
            HL_CHECK(services.memory->release(services.context, exact.handle).status == HL_STATUS_OK);
            HL_CHECK(munmap(occupied, host_page) == 0);
        }
        HL_CHECK(services.file->truncate(services.context, file.value, 0).status == HL_STATUS_OK);
    }
    HL_CHECK(services.file
                 ->sync_range(services.context, file.value, 0, 0,
                              HL_HOST_FILE_SYNC_WAIT_BEFORE | HL_HOST_FILE_SYNC_WRITE | HL_HOST_FILE_SYNC_WAIT_AFTER)
                 .status == HL_STATUS_OK);
    HL_CHECK(services.file->sync_range(services.context, file.value, 0, 0, 8).status == HL_STATUS_INVALID_ARGUMENT);
    HL_CHECK(services.file->sync_filesystem(services.context, file.value).status == HL_STATUS_OK);
    {
        char resolved[1024];
        char *expected = realpath(path, NULL);
        hl_host_result result =
            services.file->path(services.context, file.value, (hl_host_bytes){resolved, sizeof resolved});
        HL_CHECK(expected != NULL);
        HL_CHECK(result.status == HL_STATUS_OK && result.value == strlen(expected) &&
                 memcmp(resolved, expected, (size_t)result.value) == 0);
        result = services.file->path(services.context, file.value, (hl_host_bytes){resolved, strlen(expected) - 1});
        HL_CHECK(result.status == HL_STATUS_RESOURCE_LIMIT && result.value == strlen(expected));
        free(expected);
    }
    HL_CHECK(services.file->write_at(services.context, file.value, 0, (hl_host_const_bytes){"a", 1}).value == 1);
    {
        const hl_host_iovec positioned[] = {{(uint64_t)(uintptr_t)"x", 1}};
        const hl_host_iovec appended[] = {{(uint64_t)(uintptr_t)"b", 1}, {(uint64_t)(uintptr_t)"c", 1}};
        HL_CHECK(services.file->writev_at(services.context, file.value, positioned, 1, 0).value == 1);
        HL_CHECK(services.file->appendv(services.context, file.value, appended, 2).value == 2);
    }
    HL_CHECK(
        services.file->read_at(services.context, file.value, 0, (hl_host_bytes){contents, sizeof(contents)}).value ==
        sizeof(contents));
    HL_CHECK(memcmp(contents, "xbc", sizeof(contents)) == 0);
    {
        hl_host_file_metadata metadata;
        struct stat native_metadata;
        hl_host_result clone;
        char first = 0;
        char second = 0;
        char vector_contents[3] = {0};
        hl_host_iovec vectors[] = {{(uint64_t)(uintptr_t)&vector_contents[0], 1},
                                   {(uint64_t)(uintptr_t)&vector_contents[1], 2}};
        HL_CHECK(services.file->metadata(services.context, file.value, &metadata).status == HL_STATUS_OK);
        HL_CHECK(stat(path, &native_metadata) == 0);
        HL_CHECK(metadata.type == HL_HOST_FILE_TYPE_REGULAR && metadata.size == 3 &&
                 (metadata.permissions & 0600u) == 0600u);
        HL_CHECK(metadata.link_count >= 1);
        /* Host metadata describes the native object.  In particular, macOS may inherit a new file's
           group from its parent directory instead of the process GID. */
        HL_CHECK(metadata.user == (uint32_t)native_metadata.st_uid);
        HL_CHECK(metadata.group == (uint32_t)native_metadata.st_gid);
        HL_CHECK(metadata.modified_ns != 0 && metadata.accessed_ns != 0 && metadata.changed_ns != 0 &&
                 metadata.created_ns != 0);
        HL_CHECK(services.file->seek(services.context, file.value, 0, SEEK_SET).value == 0);
        clone = services.file->clone_for_fork(services.context, file.value);
        HL_CHECK(clone.status == HL_STATUS_OK && clone.value != file.value);
        HL_CHECK(services.file->read(services.context, clone.value, &first, 1).value == 1 && first == 'x');
        HL_CHECK(services.file->read(services.context, file.value, &second, 1).value == 1 && second == 'b');
        HL_CHECK(services.file->seek(services.context, clone.value, 0, SEEK_SET).value == 0);
        HL_CHECK(services.file->readv(services.context, clone.value, vectors, 2).value == 3);
        HL_CHECK(memcmp(vector_contents, "xbc", 3) == 0);
        HL_CHECK(services.file->append(services.context, file.value, (hl_host_const_bytes){"d", 1}).value == 1);
        {
            hl_host_result sequential = services.file->open_relative(
                services.context, HL_HOST_HANDLE_CWD, path, strlen(path), HL_HOST_FILE_READ | HL_HOST_FILE_WRITE, 0, 0);
            char positioned_contents[4] = {0};
            const hl_host_iovec written[] = {{(uint64_t)(uintptr_t)"z", 1}, {(uint64_t)(uintptr_t)"w", 1}};
            hl_host_iovec positioned[] = {{(uint64_t)(uintptr_t)&positioned_contents[0], 2},
                                          {(uint64_t)(uintptr_t)&positioned_contents[2], 2}};
            HL_CHECK(sequential.status == HL_STATUS_OK);
            HL_CHECK(services.file->seek(services.context, sequential.value, 0, SEEK_SET).value == 0);
            HL_CHECK(services.file->write(services.context, sequential.value, "y", 1).value == 1);
            HL_CHECK(services.file->writev(services.context, sequential.value, written, 2).value == 2);
            HL_CHECK(services.file->readv_at(services.context, sequential.value, positioned, 2, 0).value == 4);
            HL_CHECK(memcmp(positioned_contents, "yzwd", 4) == 0);
            HL_CHECK(
                services.file->write_at(services.context, sequential.value, 0, (hl_host_const_bytes){"xbc", 3}).value ==
                3);
            HL_CHECK(services.file->truncate(services.context, sequential.value, 3).status == HL_STATUS_OK);
            HL_CHECK(services.file->close(services.context, sequential.value).status == HL_STATUS_OK);
        }
        HL_CHECK(services.file->sync(services.context, clone.value).status == HL_STATUS_OK);
        HL_CHECK(services.file->data_sync(services.context, clone.value).status == HL_STATUS_OK);
        HL_CHECK(services.file->truncate(services.context, clone.value, 2).status == HL_STATUS_OK);
        HL_CHECK(services.file->truncate(services.context, clone.value, 3).status == HL_STATUS_OK);
        HL_CHECK(services.file->write_at(services.context, clone.value, 2, (hl_host_const_bytes){"c", 1}).value == 1);
        {
            hl_host_result watch = services.watch->open(services.context, clone.value);
            hl_host_result watched_pollset = services.event->create(services.context);
            hl_host_watch_record current = {0}, changed = {0};
            hl_host_event_record notification = {0};
            HL_CHECK(watch.status == HL_STATUS_OK && watched_pollset.status == HL_STATUS_OK);
            HL_CHECK(services.watch->query(services.context, watch.value, &current).status == HL_STATUS_OK &&
                     current.size == 3);
            HL_CHECK(services.event
                         ->control(services.context, watched_pollset.value, HL_HOST_EVENT_ADD, watch.value, 313,
                                   HL_HOST_READY_READ)
                         .status == HL_STATUS_OK);
            HL_CHECK(services.file->truncate(services.context, clone.value, 9).status == HL_STATUS_OK);
            uint64_t watch_deadline = services.clock->monotonic_ns(services.context).value + UINT64_C(1000000000);
            HL_CHECK(
                services.event->wait(services.context, watched_pollset.value, &notification, 1, watch_deadline).value ==
                    1 &&
                notification.token == 313 && (notification.readiness & HL_HOST_READY_READ) != 0);
            HL_CHECK(services.watch->drain(services.context, watch.value, &changed, 1).value == 1 &&
                     changed.generation > current.generation && changed.size == 9 &&
                     (changed.changes & HL_HOST_WATCH_SIZE) != 0);
            HL_CHECK(services.watch->drain(services.context, watch.value, &changed, 1).status == HL_STATUS_WOULD_BLOCK);
            HL_CHECK(services.file->write_at(services.context, clone.value, 0, (hl_host_const_bytes){"q", 1}).value ==
                     1);
            watch_deadline = services.clock->monotonic_ns(services.context).value + UINT64_C(1000000000);
            HL_CHECK(
                services.event->wait(services.context, watched_pollset.value, &notification, 1, watch_deadline).value ==
                1);
            HL_CHECK(services.watch->drain(services.context, watch.value, &changed, 1).value == 1 &&
                     changed.size == 9 && (changed.changes & HL_HOST_WATCH_DATA) != 0);
            pid_t watch_child = fork();
            HL_CHECK(watch_child >= 0);
            if (watch_child == 0) {
                hl_host_watch_record inherited = {0};
                _exit(services.watch->query(services.context, watch.value, &inherited).status == HL_STATUS_OK &&
                              inherited.size == 9
                          ? 0
                          : 30);
            }
            int watch_status = 0;
            HL_CHECK(waitpid(watch_child, &watch_status, 0) == watch_child && WIFEXITED(watch_status) &&
                     WEXITSTATUS(watch_status) == 0);
            snprintf(moved_path, sizeof(moved_path), "%s.watch", path);
            (void)unlink(moved_path);
            HL_CHECK(rename(path, moved_path) == 0);
            watch_deadline = services.clock->monotonic_ns(services.context).value + UINT64_C(1000000000);
            HL_CHECK(
                services.event->wait(services.context, watched_pollset.value, &notification, 1, watch_deadline).value ==
                1);
            HL_CHECK(services.watch->drain(services.context, watch.value, &changed, 1).value == 1 &&
                     (changed.changes & HL_HOST_WATCH_IDENTITY) != 0);
            HL_CHECK(rename(moved_path, path) == 0);
            watch_deadline = services.clock->monotonic_ns(services.context).value + UINT64_C(1000000000);
            HL_CHECK(
                services.event->wait(services.context, watched_pollset.value, &notification, 1, watch_deadline).value ==
                1);
            HL_CHECK(services.watch->drain(services.context, watch.value, &changed, 1).value == 1);
            HL_CHECK(unlink(path) == 0);
            watch_deadline = services.clock->monotonic_ns(services.context).value + UINT64_C(1000000000);
            HL_CHECK(
                services.event->wait(services.context, watched_pollset.value, &notification, 1, watch_deadline).value ==
                1);
            HL_CHECK(services.watch->drain(services.context, watch.value, &changed, 1).value == 1 &&
                     (changed.changes & HL_HOST_WATCH_DELETED) != 0);
            int replacement = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
            HL_CHECK(replacement >= 0 && ftruncate(replacement, 3) == 0 && close(replacement) == 0);
            HL_CHECK(services.event->close(services.context, watched_pollset.value).status == HL_STATUS_OK);
            HL_CHECK(services.watch->close(services.context, watch.value).status == HL_STATUS_OK);
            HL_CHECK(services.file->truncate(services.context, clone.value, 3).status == HL_STATUS_OK);
        }
        HL_CHECK(services.file->close(services.context, clone.value).status == HL_STATUS_OK);
    }
    HL_CHECK(services.file->close(services.context, file.value).status == HL_STATUS_OK);
    {
        struct stat status;
        errno = 0;
        HL_CHECK(hl_host_file_exclusive(&services, path, 0600) == -1 && errno == EIO);
        HL_CHECK(stat(path, &status) == 0 && status.st_size == 3);
        HL_CHECK(hl_host_file_reset(&services, path, 0600) == 0);
        HL_CHECK(stat(path, &status) == 0 && status.st_size == 0);
    }
    snprintf(moved_path, sizeof(moved_path), "%s.moved", path);
    {
        hl_host_result renamed =
            services.file->rename_relative(services.context, HL_HOST_HANDLE_CWD, path, strlen(path), HL_HOST_HANDLE_CWD,
                                           moved_path, strlen(moved_path));
        HL_CHECK(renamed.status == HL_STATUS_OK);
    }
    HL_CHECK(hl_host_file_store(&services, path, 0600, "replacement", 11) == 0);
    HL_CHECK(services.file
                 ->rename_relative(services.context, HL_HOST_HANDLE_CWD, moved_path, strlen(moved_path),
                                   HL_HOST_HANDLE_CWD, path, strlen(path))
                 .status == HL_STATUS_OK);
    {
        struct stat status;
        HL_CHECK(stat(path, &status) == 0 && status.st_size == 0);
    }
    HL_CHECK(services.file->unlink_relative(services.context, HL_HOST_HANDLE_CWD, path, strlen(path)).status ==
             HL_STATUS_OK);
    {
        hl_host_services standalone = services;
        standalone.capabilities &= ~(uint64_t)HL_HOST_CAP_FILE;
        standalone.file = NULL;
        HL_CHECK(hl_host_services_validate(&standalone, HL_HOST_CAP_STREAM) == HL_STATUS_OK);
        hl_host_result pipe = services.stream->pipe_pair(services.context, 0);
        char bytes[16] = {0};
        HL_CHECK(pipe.status == HL_STATUS_OK && pipe.value != 0 && pipe.detail != 0);
        HL_CHECK((standalone.stream->readiness(standalone.context, pipe.value, HL_HOST_READY_READ).value &
                  HL_HOST_READY_READ) == 0);
        HL_CHECK((standalone.stream->readiness(standalone.context, pipe.detail, HL_HOST_READY_WRITE).value &
                  HL_HOST_READY_WRITE) != 0);
        HL_CHECK(standalone.stream->write(standalone.context, pipe.detail, (hl_host_const_bytes){"stream", 6}).value ==
                 6);
        {
            hl_host_result pollset = services.event->create(services.context);
            hl_host_event_record event_record = {0};
            HL_CHECK(pollset.status == HL_STATUS_OK);
            HL_CHECK(
                services.event
                    ->control(services.context, pollset.value, HL_HOST_EVENT_ADD, pipe.value, 73, HL_HOST_READY_READ)
                    .status == HL_STATUS_OK);
            HL_CHECK(services.event->wait(services.context, pollset.value, &event_record, 1, HL_HOST_DEADLINE_INFINITE)
                         .value == 1);
            HL_CHECK(event_record.token == 73 && (event_record.readiness & HL_HOST_READY_READ) != 0);
            HL_CHECK(
                services.event
                    ->control(services.context, pollset.value, HL_HOST_EVENT_DELETE, pipe.value, 73, HL_HOST_READY_READ)
                    .status == HL_STATUS_OK);
            HL_CHECK(services.event->close(services.context, pollset.value).status == HL_STATUS_OK);
        }
        HL_CHECK((standalone.stream->readiness(standalone.context, pipe.value, HL_HOST_READY_READ).value &
                  HL_HOST_READY_READ) != 0);
        HL_CHECK(standalone.stream->read(standalone.context, pipe.value, (hl_host_bytes){bytes, sizeof bytes}).value ==
                 6);
        HL_CHECK(memcmp(bytes, "stream", 6) == 0);
        HL_CHECK(services.stream->set_status_flags(services.context, pipe.value, HL_HOST_STREAM_NONBLOCK).status ==
                 HL_STATUS_OK);
        HL_CHECK(services.file->read(services.context, pipe.value, bytes, sizeof bytes).status ==
                 HL_STATUS_WOULD_BLOCK);
        HL_CHECK(services.stream->set_status_flags(services.context, pipe.value, 0).status == HL_STATUS_OK);
        {
            pid_t blocked = fork();
            int blocked_status = 0;
            struct timespec settle = {0, 10000000};
            HL_CHECK(blocked >= 0);
            if (blocked == 0) {
                char byte;
                (void)services.file->read(services.context, pipe.value, &byte, 1);
                _exit(20);
            }
            nanosleep(&settle, NULL);
            HL_CHECK(kill(blocked, SIGKILL) == 0);
            HL_CHECK(waitpid(blocked, &blocked_status, 0) == blocked && WIFSIGNALED(blocked_status));
            HL_CHECK(services.file->write(services.context, pipe.detail, "r", 1).value == 1);
            HL_CHECK(services.file->read(services.context, pipe.value, bytes, 1).value == 1 && bytes[0] == 'r');
        }
        HL_CHECK(services.file->close(services.context, pipe.value).status == HL_STATUS_OK);
        {
            hl_host_result broken = services.file->write(services.context, pipe.detail, "x", 1);
            HL_CHECK(broken.status == HL_STATUS_DISCONNECTED && broken.detail == EPIPE);
        }
        HL_CHECK(services.file->close(services.context, pipe.detail).status == HL_STATUS_OK);
    }
    {
        hl_host_result pipe = services.stream->pipe_pair(services.context, 0);
        hl_host_result clone = services.file->clone_for_fork(services.context, pipe.value);
        char byte = 0;
        HL_CHECK(pipe.status == HL_STATUS_OK && clone.status == HL_STATUS_OK);
        HL_CHECK(services.file->close(services.context, pipe.value).status == HL_STATUS_OK);
        HL_CHECK(services.file->write(services.context, pipe.detail, "d", 1).value == 1);
        HL_CHECK(services.file->read(services.context, clone.value, &byte, 1).value == 1 && byte == 'd');
        HL_CHECK(services.file->close(services.context, clone.value).status == HL_STATUS_OK);
        HL_CHECK(services.file->close(services.context, pipe.detail).status == HL_STATUS_OK);
    }
    {
        hl_host_result pipe = services.stream->pipe_pair(services.context, 0);
        stream_fork_close_context child = {&services, pipe.value, pipe.detail};
        hl_host_result spawned;
        hl_host_result waited;
        HL_CHECK(pipe.status == HL_STATUS_OK);
        HL_CHECK(services.sync->fork_prepare(services.context).status == HL_STATUS_OK);
        spawned = services.process->spawn_prepared(services.context, child_close_inherited_stream, &child);
        HL_CHECK(spawned.status == HL_STATUS_OK);
        HL_CHECK(services.stream->close(services.context, pipe.detail).status == HL_STATUS_OK);
        waited = services.process->wait(services.context, spawned.value, HL_HOST_DEADLINE_INFINITE);
        HL_CHECK(waited.status == HL_STATUS_OK && waited.detail == HL_HOST_PROCESS_EXIT_CODE && waited.value == 0);
        HL_CHECK(services.process->close(services.context, spawned.value).status == HL_STATUS_OK);
        HL_CHECK(services.stream->close(services.context, pipe.value).status == HL_STATUS_OK);
    }
    {
        hl_host_result pipe = services.stream->pipe_pair(services.context, 0);
        stream_read_context reader = {&services, pipe.value, {0}, {0}};
        pthread_t thread;
        struct timespec settle = {0, 10000000};
        HL_CHECK(pipe.status == HL_STATUS_OK && pthread_create(&thread, NULL, stream_read_thread, &reader) == 0);
        nanosleep(&settle, NULL);
        HL_CHECK(services.file->close(services.context, pipe.value).status == HL_STATUS_OK);
        HL_CHECK(services.file->write(services.context, pipe.detail, "p", 1).value == 1);
        HL_CHECK(pthread_join(thread, NULL) == 0 && reader.result.value == 1 && reader.bytes[0] == 'p');
        HL_CHECK(services.file->close(services.context, pipe.detail).status == HL_STATUS_OK);
    }
    {
        hl_host_result source = services.stream->pipe_pair(services.context, HL_HOST_STREAM_NONBLOCK);
        hl_host_result destination = services.stream->pipe_pair(services.context, HL_HOST_STREAM_NONBLOCK);
        stream_read_context reader = {&services, source.value, {0}, {0}};
        pthread_t thread;
        hl_host_result moved;
        char delivered[256];
        hl_host_result received;
        unsigned char payload[100];
        memset(payload, 0x5a, sizeof payload);
        HL_CHECK(source.status == HL_STATUS_OK && destination.status == HL_STATUS_OK);
        HL_CHECK(services.file->write(services.context, source.detail, payload, sizeof payload).value ==
                 sizeof payload);
        HL_CHECK(pthread_create(&thread, NULL, stream_read_thread, &reader) == 0);
        moved = services.stream->move(services.context, source.value, 0, destination.detail, 0, sizeof payload, 0);
        HL_CHECK(pthread_join(thread, NULL) == 0);
        received = services.file->read(services.context, destination.value, delivered, sizeof delivered);
        HL_CHECK((reader.result.status == HL_STATUS_OK ? reader.result.value : 0) +
                     (received.status == HL_STATUS_OK ? received.value : 0) ==
                 sizeof payload);
        HL_CHECK(moved.status == HL_STATUS_OK || moved.status == HL_STATUS_WOULD_BLOCK);
        HL_CHECK(services.file->close(services.context, source.value).status == HL_STATUS_OK);
        HL_CHECK(services.file->close(services.context, source.detail).status == HL_STATUS_OK);
        HL_CHECK(services.file->close(services.context, destination.value).status == HL_STATUS_OK);
        HL_CHECK(services.file->close(services.context, destination.detail).status == HL_STATUS_OK);
    }
    {
        enum { RECORDS = 100, RECORD_SIZE = 64, TOTAL = 2 * RECORDS * RECORD_SIZE };

        hl_host_result pipe = services.stream->pipe_pair(services.context, 0);
        stream_writer_context first = {&services, pipe.detail, 'A', RECORDS, 0};
        stream_writer_context second = {&services, pipe.detail, 'B', RECORDS, 0};
        pthread_t first_thread, second_thread;
        unsigned char received[TOTAL];
        size_t total = 0;
        HL_CHECK(pipe.status == HL_STATUS_OK);
        HL_CHECK(pthread_create(&first_thread, NULL, stream_writer_thread, &first) == 0);
        HL_CHECK(pthread_create(&second_thread, NULL, stream_writer_thread, &second) == 0);
        while (total < sizeof received) {
            hl_host_result result = services.stream->read(services.context, pipe.value,
                                                          (hl_host_bytes){received + total, sizeof received - total});
            HL_CHECK(result.status == HL_STATUS_OK && result.value != 0 && result.value <= sizeof received - total);
            total += (size_t)result.value;
        }
        HL_CHECK(pthread_join(first_thread, NULL) == 0 && pthread_join(second_thread, NULL) == 0);
        HL_CHECK(!first.failed && !second.failed);
        for (size_t offset = 0; offset < sizeof received; offset += RECORD_SIZE) {
            HL_CHECK(received[offset] == 'A' || received[offset] == 'B');
            for (size_t index = 1; index < RECORD_SIZE; ++index)
                HL_CHECK(received[offset + index] == received[offset]);
        }
        HL_CHECK(services.stream->close(services.context, pipe.value).status == HL_STATUS_OK);
        HL_CHECK(services.stream->close(services.context, pipe.detail).status == HL_STATUS_OK);
    }
    {
        hl_host_result source = services.stream->pipe_pair(services.context, HL_HOST_STREAM_NONBLOCK);
        hl_host_result destination = services.stream->pipe_pair(services.context, HL_HOST_STREAM_NONBLOCK);
        unsigned char fill[4096];
        char retained[8] = {0};
        hl_host_result written;
        memset(fill, 0xa5, sizeof fill);
        do
            written = services.file->write(services.context, destination.detail, fill, sizeof fill);
        while (written.status == HL_STATUS_OK && written.value != 0);
        HL_CHECK(written.status == HL_STATUS_WOULD_BLOCK);
        HL_CHECK(services.file->write(services.context, source.detail, "retain", 6).value == 6);
        HL_CHECK(services.stream->move(services.context, source.value, 0, destination.detail, 0, 6, 0).status ==
                 HL_STATUS_WOULD_BLOCK);
        HL_CHECK(services.file->read(services.context, source.value, retained, sizeof retained).value == 6);
        HL_CHECK(memcmp(retained, "retain", 6) == 0);
        HL_CHECK(services.file->close(services.context, source.value).status == HL_STATUS_OK);
        HL_CHECK(services.file->close(services.context, source.detail).status == HL_STATUS_OK);
        HL_CHECK(services.file->close(services.context, destination.value).status == HL_STATUS_OK);
        HL_CHECK(services.file->close(services.context, destination.detail).status == HL_STATUS_OK);
    }
    {
        char target[32] = {0};
        hl_host_file_metadata metadata;
        hl_host_result link;
        snprintf(moved_path, sizeof(moved_path), "%s.link", path);
        HL_CHECK(symlink("portable-target", moved_path) == 0);
        HL_CHECK(services.file
                     ->open_relative(services.context, HL_HOST_HANDLE_CWD, moved_path, strlen(moved_path),
                                     HL_HOST_FILE_READ | HL_HOST_FILE_NOFOLLOW, 0, 0)
                     .status != HL_STATUS_OK);
        link = services.file->open_relative(services.context, HL_HOST_HANDLE_CWD, moved_path, strlen(moved_path),
                                            HL_HOST_FILE_PATH_ONLY | HL_HOST_FILE_NOFOLLOW, 0, 0);
        HL_CHECK(link.status == HL_STATUS_OK);
        HL_CHECK(services.file->metadata(services.context, link.value, &metadata).status == HL_STATUS_OK &&
                 metadata.type == HL_HOST_FILE_TYPE_SYMLINK);
        HL_CHECK(services.file->readlink(services.context, link.value, (hl_host_bytes){target, sizeof target}).value ==
                 strlen("portable-target"));
        HL_CHECK(memcmp(target, "portable-target", strlen("portable-target")) == 0);
        HL_CHECK(
            services.file->set_owner(services.context, link.value, (uint32_t)getuid(), (uint32_t)getgid()).status ==
            HL_STATUS_OK);
        HL_CHECK(services.file->close(services.context, link.value).status == HL_STATUS_OK);
        HL_CHECK(unlink(moved_path) == 0);
    }
    memset(&code, 0, sizeof code);
    HL_CHECK(services.memory->reserve_code(services.context, 16384, 16384, HL_HOST_CODE_DUAL_ALIAS, &code).status ==
             HL_STATUS_OK);
    HL_CHECK(services.memory->begin_code_write(services.context).status == HL_STATUS_OK);
    memcpy((void *)(uintptr_t)code.writable_address, "code", 5);
    code.content_size = 5;
    HL_CHECK(services.memory->end_code_write(services.context).status == HL_STATUS_OK);
    HL_CHECK(services.memory->publish_code(services.context, code.handle, 0, 5).status == HL_STATUS_OK);
    HL_CHECK(memcmp((const void *)(uintptr_t)code.executable_address, "code", 5) == 0);
    pid_t child = fork();
    HL_CHECK(child >= 0);
    if (child == 0) {
        if (services.memory->repair_code_after_fork(services.context, &code, 1).status != HL_STATUS_OK) _exit(10);
        memcpy((void *)(uintptr_t)code.writable_address, "fork", 5);
        if (services.memory->publish_code(services.context, code.handle, 0, 5).status != HL_STATUS_OK) _exit(11);
        _exit(memcmp((const void *)(uintptr_t)code.executable_address, "fork", 5) == 0 ? 0 : 12);
    }
    int status = 0;
    HL_CHECK(waitpid(child, &status, 0) == child);
    HL_CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    HL_CHECK(services.memory->release(services.context, code.handle).status == HL_STATUS_OK);
    memset(&code, 0, sizeof code);
    HL_CHECK(services.memory->reserve_code(services.context, 16384, 16384, 0, &code).status == HL_STATUS_OK);
    HL_CHECK(code.writable_address == code.executable_address);
    HL_CHECK(services.memory->begin_code_write(services.context).status == HL_STATUS_OK);
    memcpy((void *)(uintptr_t)code.writable_address, "single", 7);
    HL_CHECK(services.memory->end_code_write(services.context).status == HL_STATUS_OK);
    HL_CHECK(services.memory->publish_code(services.context, code.handle, 0, 7).status == HL_STATUS_OK);
    HL_CHECK(memcmp((const void *)(uintptr_t)code.executable_address, "single", 7) == 0);
    HL_CHECK(services.memory->release(services.context, code.handle).status == HL_STATUS_OK);
    {
        process_wait_context cleanup_waiter = {&services, 0, {0}};
        pthread_t cleanup_thread;
        struct timespec settle = {0, 10000000};
        process = services.process->spawn_cloned(services.context, child_pause, NULL);
        HL_CHECK(process.status == HL_STATUS_OK);
        cleanup_waiter.process = process.value;
        HL_CHECK(pthread_create(&cleanup_thread, NULL, wait_for_process, &cleanup_waiter) == 0);
        nanosleep(&settle, NULL);
        hl_host_macos_destroy(host);
        HL_CHECK(pthread_join(cleanup_thread, NULL) == 0);
        HL_CHECK(cleanup_waiter.result.status == HL_STATUS_OK &&
                 cleanup_waiter.result.detail == HL_HOST_PROCESS_EXIT_SIGNAL && cleanup_waiter.result.value == SIGKILL);
    }
    return EXIT_SUCCESS;
}
