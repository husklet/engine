#ifndef HL_HOST_SERVICES_H
#define HL_HOST_SERVICES_H

#include "hl/base.h"

HL_EXTERN_C_BEGIN

#define HL_HOST_SERVICES_ABI 4u
#define HL_HOST_MEMORY_ABI 8u
/* Oldest memory group still accepted. An ABI 6 provider ends at repair_signal_page and an ABI 7
 * provider ends at unwire_range; callbacks appended after a provider's own version are absent
 * rather than NULL, so validation checks the prefix that version is required to carry and only
 * demands an appended callback from a provider that declares the version which added it. */
#define HL_HOST_MEMORY_ABI_MIN 6u
#define HL_HOST_FILE_MAPPING_ABI 1u
#define HL_HOST_MEMORY_MAPPING_ABI 1u
#define HL_HOST_CLOCK_ABI 4u
#define HL_HOST_LOG_ABI 1u
#define HL_HOST_FILE_ABI 23u
#define HL_HOST_PROCESS_ABI 3u
#define HL_HOST_EVENT_ABI 2u
#define HL_HOST_NETWORK_ABI 1u
#define HL_HOST_SHARED_MEMORY_ABI 1u
#define HL_HOST_COUNTER_ABI 2u
#define HL_HOST_SYNC_ABI 3u
/* Oldest sync group still accepted. An ABI 2 provider ends at fork_child; the parking operations
 * appended in ABI 3 are absent rather than NULL, so validation checks the ABI 2 prefix and only
 * demands the appended callbacks from an ABI 3 provider. */
#define HL_HOST_SYNC_ABI_MIN 2u
#define HL_HOST_TERMINAL_ABI 1u
#define HL_HOST_TRANSFER_ABI 2u
#define HL_HOST_DIRECTORY_ABI 1u
#define HL_HOST_WATCH_ABI 1u
#define HL_HOST_STREAM_ABI 1u
#define HL_HOST_POSIX_ATTACHMENT_ABI 2u

typedef uint64_t hl_host_handle;

enum {
    HL_HOST_HANDLE_INVALID = 0,
    HL_HOST_CAP_MEMORY = UINT64_C(1) << 0,
    HL_HOST_CAP_CLOCK = UINT64_C(1) << 1,
    HL_HOST_CAP_LOG = UINT64_C(1) << 2,
    HL_HOST_CAP_FAST_CLONE = UINT64_C(1) << 3,
    HL_HOST_CAP_FILE = UINT64_C(1) << 4,
    HL_HOST_CAP_PROCESS = UINT64_C(1) << 5,
    HL_HOST_CAP_EVENT = UINT64_C(1) << 6,
    HL_HOST_CAP_NETWORK = UINT64_C(1) << 7,
    HL_HOST_CAP_SHARED_MEMORY = UINT64_C(1) << 8,
    HL_HOST_CAP_CODE_MAPPING = UINT64_C(1) << 9,
    HL_HOST_CAP_SYNC = UINT64_C(1) << 10,
    HL_HOST_CAP_EVENT_TIMER = UINT64_C(1) << 11,
    HL_HOST_CAP_COUNTER = UINT64_C(1) << 12,
    HL_HOST_CAP_TRANSFER = UINT64_C(1) << 13,
    HL_HOST_CAP_DIRECTORY = UINT64_C(1) << 14,
    HL_HOST_CAP_WATCH = UINT64_C(1) << 15,
    HL_HOST_CAP_STREAM = UINT64_C(1) << 16,
    HL_HOST_CAP_POSIX_ATTACHMENT = UINT64_C(1) << 17,
    HL_HOST_CAP_TERMINAL = UINT64_C(1) << 18
};

enum {
    HL_HOST_FILE_READ = 1u << 0,
    HL_HOST_FILE_WRITE = 1u << 1,
    HL_HOST_FILE_APPEND = 1u << 2,
    HL_HOST_FILE_DIRECTORY = 1u << 3,
    HL_HOST_FILE_NONBLOCK = 1u << 4,
    HL_HOST_FILE_NOFOLLOW = 1u << 5,
    HL_HOST_FILE_PATH_ONLY = 1u << 6
};

enum { HL_HOST_STANDARD_INPUT = 0, HL_HOST_STANDARD_OUTPUT = 1, HL_HOST_STANDARD_ERROR = 2 };

/* Linux-compatible logical seek modes; hosts translate these rather than exposing native ABI numbers. */
enum {
    HL_HOST_FILE_SEEK_SET = 0,
    HL_HOST_FILE_SEEK_CUR = 1,
    HL_HOST_FILE_SEEK_END = 2,
    HL_HOST_FILE_SEEK_DATA = 3,
    HL_HOST_FILE_SEEK_HOLE = 4
};

enum { HL_HOST_COUNTER_SEMAPHORE = 1u << 0, HL_HOST_COUNTER_NONBLOCK = 1u << 1 };

enum { HL_HOST_FILE_CREATE = 1u << 0, HL_HOST_FILE_EXCLUSIVE = 1u << 1, HL_HOST_FILE_TRUNCATE = 1u << 2 };

enum {
    HL_HOST_FILE_ALLOC_KEEP_SIZE = 1u << 0,
    HL_HOST_FILE_ALLOC_PUNCH_HOLE = 1u << 1,
    HL_HOST_FILE_ALLOC_COLLAPSE_RANGE = 1u << 3,
    HL_HOST_FILE_ALLOC_ZERO_RANGE = 1u << 4,
    HL_HOST_FILE_ALLOC_INSERT_RANGE = 1u << 5,
    HL_HOST_FILE_ALLOC_UNSHARE_RANGE = 1u << 6
};

enum {
    HL_HOST_RESOLVE_NOFOLLOW_FINAL = 1u << 0,
    HL_HOST_RESOLVE_NO_SYMLINKS = 1u << 1,
    HL_HOST_RESOLVE_ALLOW_MISSING = 1u << 2
};

/* Host-independent object kinds returned by hl_host_file_metadata. */
typedef enum hl_host_file_type {
    HL_HOST_FILE_TYPE_UNKNOWN = 0,
    HL_HOST_FILE_TYPE_REGULAR = 1,
    HL_HOST_FILE_TYPE_DIRECTORY = 2,
    HL_HOST_FILE_TYPE_SYMLINK = 3,
    HL_HOST_FILE_TYPE_CHARACTER = 4,
    HL_HOST_FILE_TYPE_BLOCK = 5,
    HL_HOST_FILE_TYPE_FIFO = 6,
    HL_HOST_FILE_TYPE_SOCKET = 7
} hl_host_file_type;

typedef enum hl_host_network_family {
    HL_HOST_NETWORK_IPV4 = 1,
    HL_HOST_NETWORK_IPV6 = 2,
    HL_HOST_NETWORK_LOCAL = 3
} hl_host_network_family;

typedef enum hl_host_network_type { HL_HOST_NETWORK_STREAM = 1, HL_HOST_NETWORK_DATAGRAM = 2 } hl_host_network_type;

typedef struct hl_host_network_address {
    uint32_t family;
    uint16_t port;
    uint16_t size;
    uint8_t address[16];
    char local_path[108];
} hl_host_network_address;

#define HL_HOST_HANDLE_CWD UINT64_MAX

enum { HL_HOST_MEMORY_READ = 1u << 0, HL_HOST_MEMORY_WRITE = 1u << 1, HL_HOST_MEMORY_EXECUTE = 1u << 2 };

enum {
    HL_HOST_MEMORY_SHARED = 1u << 0,
    HL_HOST_MEMORY_PRIVATE = 1u << 1,
    HL_HOST_MEMORY_FIXED = 1u << 2,
    HL_HOST_MEMORY_FIXED_NOREPLACE = 1u << 3
};

enum { HL_HOST_CODE_DUAL_ALIAS = 1u << 0 };

/* What sync_address is being asked to guarantee when it returns. Zero -- no bit set -- is the
 * strong form and matches the handle-keyed sync: the range is durable in its backing object
 * before the call returns. These are named rather than passed through as native flag words so a
 * host is never handed a number it has to reinterpret, and so the mutually exclusive pair that
 * the POSIX word encodes cannot be expressed at all: ASYNC set means "scheduled", ASYNC clear
 * means "durable", and there is no third state to reject. */
enum { HL_HOST_MEMORY_SYNC_ASYNC = 1u << 0, HL_HOST_MEMORY_SYNC_INVALIDATE = 1u << 1 };

/* What a host actually does when asked to wire a range. A caller must not assume that a
 * successful wire means the same thing everywhere: Linux and Darwin mlock(2) pin pages
 * against reclaim, while the nearest Windows primitive (VirtualLock) only grows the
 * process working set and leaves the pages reclaimable. Whose limit applies and what a
 * refusal means differ with it, so wire_range reports the kind it applied instead of
 * letting the caller guess from the name. */
typedef enum hl_host_wire_kind {
    HL_HOST_WIRE_NONE = 0,
    HL_HOST_WIRE_RESIDENT = 1,
    HL_HOST_WIRE_WORKING_SET = 2
} hl_host_wire_kind;

enum { HL_HOST_EVENT_ADD = 1, HL_HOST_EVENT_MODIFY = 2, HL_HOST_EVENT_DELETE = 3 };

enum {
    HL_HOST_READY_READ = 1u << 0,
    HL_HOST_READY_WRITE = 1u << 1,
    HL_HOST_READY_ERROR = 1u << 2,
    HL_HOST_READY_HANGUP = 1u << 3,
    HL_HOST_READY_EDGE = 1u << 4,
    HL_HOST_READY_ONESHOT = 1u << 5,
    HL_HOST_READY_TIMER = 1u << 6
};

typedef struct hl_host_bytes {
    void *data;
    size_t size;
} hl_host_bytes;

typedef struct hl_host_const_bytes {
    const void *data;
    size_t size;
} hl_host_const_bytes;

typedef struct hl_host_result {
    int32_t status;
    uint32_t detail_domain;
    uint64_t value;
    uint64_t detail;
} hl_host_result;

typedef struct hl_host_code_mapping {
    HL_ABI_HEADER;
    hl_host_handle handle;
    uint64_t writable_address;
    uint64_t executable_address;
    uint64_t mapped_size;
    uint64_t content_size;
} hl_host_code_mapping;

typedef struct hl_host_file_mapping {
    HL_ABI_HEADER;
    hl_host_handle handle;
    uint64_t address;
    uint64_t mapped_size;
    uint64_t reserved;
} hl_host_file_mapping;

/* An owned anonymous host mapping. The address is process-local; the handle is
 * the only token accepted by protect/release and must not expose a native fd. */
typedef struct hl_host_memory_mapping {
    HL_ABI_HEADER;
    hl_host_handle handle;
    uint64_t address;
    uint64_t mapped_size;
    uint64_t reserved;
} hl_host_memory_mapping;

typedef struct hl_host_memory_services {
    HL_ABI_HEADER;
    hl_host_result (*reserve)(void *context, uint64_t size, uint64_t alignment, uint32_t flags);
    hl_host_result (*protect)(void *context, hl_host_handle mapping, uint64_t offset, uint64_t size, uint32_t flags);
    hl_host_result (*release)(void *context, hl_host_handle mapping);
    hl_host_result (*publish_code)(void *context, hl_host_handle mapping, uint64_t offset, uint64_t size);
    hl_host_result (*reserve_code)(void *context, uint64_t size, uint64_t alignment, uint32_t flags,
                                   hl_host_code_mapping *output);
    hl_host_result (*repair_code_after_fork)(void *context, hl_host_code_mapping *mapping, uint32_t preserve);
    /* Per-thread W^X gate. begin enables writes; end restores execution. Dual-alias hosts may no-op. */
    hl_host_result (*begin_code_write)(void *context);
    hl_host_result (*end_code_write)(void *context);
    /* Map an opaque file handle. offset and size must satisfy the native VM page granularity. */
    hl_host_result (*map_file)(void *context, hl_host_handle file, uint64_t address, uint64_t offset, uint64_t size,
                               uint32_t protection, uint32_t flags, hl_host_file_mapping *output);
    /* Flush a range of a shared file mapping to its backing object. */
    hl_host_result (*sync)(void *context, hl_host_handle mapping, uint64_t offset, uint64_t size);
    /* Unmap a page-aligned subrange. A full-range unmap consumes the mapping handle. */
    hl_host_result (*unmap_range)(void *context, hl_host_handle mapping, uint64_t offset, uint64_t size);
    /* Create an anonymous private or fork-shared mapping, optionally at an exact address.
     * The returned pages are zero-filled: private mappings are fresh anonymous memory and
     * fork-shared mappings are a freshly sized memfd, both of which the kernel guarantees to
     * read as zero. Callers (e.g. hl_linux_memory_create) rely on this instead of eagerly
     * memset()ing the whole region, which would only fault in pages that discard() then drops. */
    hl_host_result (*map_anonymous)(void *context, uint64_t requested_address, uint64_t size, uint32_t protection,
                                    uint32_t flags, hl_host_memory_mapping *output);
    /* Retire an ownership handle without changing the process address space. */
    hl_host_result (*discard)(void *context, hl_host_handle mapping);
    /* Signal-context VM repair. This is an engine contract, not a claim that
     * arbitrary POSIX VM APIs are generally async-signal-safe. Supported host
     * implementations use only direct VM operations: no userspace allocation,
     * locks, logging, ownership registries, or errno-dependent decisions. The
     * operation first protects an existing exact range, then claims a vacant
     * exact range without replacement. It never invalidates an owned mapping. */
    int (*repair_signal_page)(void *context, uint64_t address, uint64_t size, uint32_t protection);
    /* --- appended in HL_HOST_MEMORY_ABI 7 --- */
    /*
     * Release a page-aligned range the engine owns NO mapping handle for. Two populations
     * reach it: a range a provider placed at a fixed address, and a range whose ownership
     * handle a later fixed placement already retired. unmap_range cannot express either,
     * because it is keyed on an owning handle and by construction there is none.
     *
     * This is deliberately not a handle-free alias for unmap_range. A range that overlaps
     * any live mapping handle is refused whole with HL_STATUS_BUSY and nothing is unmapped,
     * so an address-keyed caller can never silently invalidate owned memory or strand a
     * handle over a hole. A range with no mapping at all succeeds, matching the host.
     */
    hl_host_result (*unmap_address)(void *context, uint64_t address, uint64_t size);
    /*
     * Wire a range into memory. address is page aligned; size follows the host rule for
     * wiring and is rounded up to whole pages. flags is reserved and must be zero.
     * On success detail is the hl_host_wire_kind the host applied. A host with no wiring
     * primitive returns HL_STATUS_NOT_SUPPORTED with detail HL_HOST_WIRE_NONE; callers
     * whose contract is best-effort (guest mlockall) treat that as a range left pageable.
     */
    hl_host_result (*wire_range)(void *context, uint64_t address, uint64_t size, uint32_t flags);
    hl_host_result (*unwire_range)(void *context, uint64_t address, uint64_t size);
    /* --- appended in HL_HOST_MEMORY_ABI 8 --- */
    /*
     * Change the protection of a range keyed by address rather than by an ownership handle.
     * protect() and sync() are keyed on a mapping handle; a caller holding only an address --
     * the same two populations unmap_address serves, plus every range a guest may legitimately
     * re-protect -- cannot reach them at all. These complete that pair.
     *
     * address is page aligned. size is non-zero and is rounded up to whole pages, which is what
     * the underlying host operations do. This differs deliberately from unmap_address, which
     * demands an exact multiple: rounding an unmap up destroys pages the caller did not name,
     * so it must state them, while rounding a protect or a flush up only touches pages it was
     * already going to leave intact.
     *
     * protect_address does NOT carry unmap_address's live-handle refusal, and the difference is
     * the point. unmap_address refuses a range overlapping a live handle because unmapping it
     * would leave that handle claiming address space which no longer exists, and a later
     * whole-handle teardown would then unmap whatever the host had since placed there -- an
     * unrecoverable, non-local corruption. Re-protecting has no such consequence: the frame, the
     * hole set, the contents and the handle all survive verbatim, the owner can restore the
     * protection through the handle-keyed protect() at any time, and no registry invariant reads
     * protection at all. Refusing here would also refuse the ordinary case, since re-protecting
     * a mapping you own is exactly what mprotect is for.
     *
     * One population is still refused whole with HL_STATUS_BUSY and nothing changed: a range
     * overlapping a live CODE mapping. There the protection is an engine invariant rather than a
     * caller preference -- it is what the per-thread W^X gate and the writable/executable alias
     * pair are made of -- and an address-keyed caller cannot restore it, because it does not hold
     * the handle that knows the pair. That is the narrow case where the unmap argument does
     * apply, so it keeps the unmap answer.
     *
     * protection uses HL_HOST_MEMORY_*; zero means no access.
     */
    hl_host_result (*protect_address)(void *context, uint64_t address, uint64_t size, uint32_t protection);
    /*
     * Flush a range keyed by address. flags is an HL_HOST_MEMORY_SYNC_* mask; zero is a durable
     * flush. A range with no shared backing carries nothing to write back, so a host is free to
     * succeed for it. No live-handle rule: flushing changes neither the address space nor any
     * protection, so there is nothing an owner could observe as taken away.
     */
    hl_host_result (*sync_address)(void *context, uint64_t address, uint64_t size, uint32_t flags);
} hl_host_memory_services;

typedef struct hl_host_clock_services {
    HL_ABI_HEADER;
    hl_host_result (*monotonic_ns)(void *context);
    hl_host_result (*realtime_ns)(void *context);
    hl_host_result (*raw_monotonic_ns)(void *context);
    hl_host_result (*process_cpu_ns)(void *context);
    hl_host_result (*thread_cpu_ns)(void *context);
    /* Sleep until an absolute deadline. EINTR is returned as HL_STATUS_INTERRUPTED, never retried here. */
    hl_host_result (*sleep_until)(void *context, uint32_t clock_kind, uint64_t deadline_ns);
    /* Effective frequency of the host architectural counter used by generated
     * code. Hosts without a userspace-readable architectural counter return
     * HL_STATUS_NOT_SUPPORTED. This lets translators validate a hardware
     * counter without importing a platform clock API. */
    hl_host_result (*architectural_counter_hz)(void *context);
    /* Relative timed backoff complements sleep_until, so it remains in the
     * clock group rather than creating a thread service with one operation.
     * This is an engine signal-context contract, not a general claim about
     * POSIX APIs: providers must use immutable state, must not allocate, lock,
     * or log, must consume interruptions, and must not return early for a
     * valid interval. */
    hl_host_result (*backoff_ns)(void *context, uint64_t interval_ns);
} hl_host_clock_services;

typedef enum hl_host_clock_kind {
    HL_HOST_CLOCK_MONOTONIC = 1,
    HL_HOST_CLOCK_REALTIME = 2,
    HL_HOST_CLOCK_RAW_MONOTONIC = 3,
    HL_HOST_CLOCK_PROCESS_CPU = 4,
    HL_HOST_CLOCK_THREAD_CPU = 5
} hl_host_clock_kind;

typedef struct hl_host_log_services {
    HL_ABI_HEADER;
    void (*emit)(void *context, uint32_t event, const char *message, size_t message_size);
} hl_host_log_services;

typedef struct hl_host_file_metadata {
    uint64_t stable_device;
    uint64_t stable_object;
    uint64_t size;
    uint64_t allocated_size;
    uint64_t modified_ns;
    uint64_t accessed_ns;
    uint64_t changed_ns;
    uint64_t created_ns;
    uint64_t device;
    uint64_t link_count;
    uint32_t type;
    uint32_t permissions;
    uint32_t user;
    uint32_t group;
} hl_host_file_metadata;

typedef struct hl_host_filesystem_metadata {
    uint64_t blocks;
    uint64_t blocks_free;
    uint64_t blocks_available;
    uint64_t files;
    uint64_t files_free;
    uint64_t filesystem_id[2];
    uint64_t block_size;
    uint64_t fragment_size;
    uint64_t name_max;
    uint64_t flags;
} hl_host_filesystem_metadata;

typedef struct hl_host_iovec {
    uint64_t address;
    uint64_t size;
} hl_host_iovec;

typedef struct hl_host_file_resolution {
    hl_host_handle parent;
    hl_host_handle target;
    uint32_t target_type;
    uint32_t reserved;
    size_t final_size;
    char final[256];
} hl_host_file_resolution;

typedef enum hl_host_file_time_mode {
    HL_HOST_FILE_TIME_EXPLICIT = 0,
    HL_HOST_FILE_TIME_NOW = 1,
    HL_HOST_FILE_TIME_OMIT = 2
} hl_host_file_time_mode;

typedef struct hl_host_file_time {
    int64_t seconds;
    uint32_t nanoseconds;
    uint32_t mode;
} hl_host_file_time;

enum {
    HL_HOST_DIRECTORY_TYPE_UNKNOWN = 0,
    HL_HOST_DIRECTORY_TYPE_FIFO = 1,
    HL_HOST_DIRECTORY_TYPE_CHARACTER = 2,
    HL_HOST_DIRECTORY_TYPE_DIRECTORY = 4,
    HL_HOST_DIRECTORY_TYPE_BLOCK = 6,
    HL_HOST_DIRECTORY_TYPE_REGULAR = 8,
    HL_HOST_DIRECTORY_TYPE_LINK = 10,
    HL_HOST_DIRECTORY_TYPE_SOCKET = 12
};

typedef struct hl_host_file_entry {
    uint64_t object;
    uint64_t next_offset;
    uint32_t type;
    uint32_t name_size;
    char name[256];
} hl_host_file_entry;

enum { HL_HOST_FILE_IOV_MAX = 1024 };

enum {
    HL_HOST_FILE_SYNC_WAIT_BEFORE = 1u << 0,
    HL_HOST_FILE_SYNC_WRITE = 1u << 1,
    HL_HOST_FILE_SYNC_WAIT_AFTER = 1u << 2
};

typedef struct hl_host_file_services {
    HL_ABI_HEADER;
    hl_host_result (*open_relative)(void *context, hl_host_handle directory, const char *path, size_t path_size,
                                    uint32_t access, uint32_t creation, uint32_t permissions);
    hl_host_result (*read_at)(void *context, hl_host_handle file, uint64_t offset, hl_host_bytes output);
    hl_host_result (*write_at)(void *context, hl_host_handle file, uint64_t offset, hl_host_const_bytes input);
    /*
     * One indivisible append on a handle opened with HL_HOST_FILE_APPEND.
     * value is bytes written. The native open-file-description position remains authoritative.
     * The host, not the guest ABI, owns cross-thread/process append atomicity.
     */
    hl_host_result (*append)(void *context, hl_host_handle file, hl_host_const_bytes input);
    hl_host_result (*metadata)(void *context, hl_host_handle file, hl_host_file_metadata *output);
    hl_host_result (*close)(void *context, hl_host_handle file);
    /* Sequential operations for streams and other non-seekable descriptors. */
    hl_host_result (*read)(void *context, hl_host_handle file, void *output, uint64_t output_size);
    hl_host_result (*write)(void *context, hl_host_handle file, const void *input, uint64_t input_size);
    hl_host_result (*clone_for_fork)(void *context, hl_host_handle file);
    hl_host_result (*seek)(void *context, hl_host_handle file, int64_t offset, uint32_t whence);
    hl_host_result (*readv)(void *context, hl_host_handle file, const hl_host_iovec *vectors, uint32_t count);
    hl_host_result (*writev)(void *context, hl_host_handle file, const hl_host_iovec *vectors, uint32_t count);
    hl_host_result (*readv_at)(void *context, hl_host_handle file, const hl_host_iovec *vectors, uint32_t count,
                               uint64_t offset);
    hl_host_result (*writev_at)(void *context, hl_host_handle file, const hl_host_iovec *vectors, uint32_t count,
                                uint64_t offset);
    hl_host_result (*appendv)(void *context, hl_host_handle file, const hl_host_iovec *vectors, uint32_t count);
    hl_host_result (*truncate)(void *context, hl_host_handle file, uint64_t size);
    hl_host_result (*sync)(void *context, hl_host_handle file);
    hl_host_result (*data_sync)(void *context, hl_host_handle file);
    /* Path namespace operations are appended in ABI 8; rename replaces the destination atomically. */
    hl_host_result (*rename_relative)(void *context, hl_host_handle old_directory, const char *old_path,
                                      size_t old_path_size, hl_host_handle new_directory, const char *new_path,
                                      size_t new_path_size);
    hl_host_result (*unlink_relative)(void *context, hl_host_handle directory, const char *path, size_t path_size);
    /* Copy the native absolute path of an open path-backed file. value is the bytes copied, without a NUL. */
    hl_host_result (*path)(void *context, hl_host_handle file, hl_host_bytes output);
    /* Duplicate a process standard stream into an opaque handle. detail contains HL_HOST_FILE_* state. */
    hl_host_result (*standard_stream)(void *context, uint32_t stream);
    /* Read the target of a link-node handle opened with PATH_ONLY|NOFOLLOW. */
    hl_host_result (*readlink)(void *context, hl_host_handle file, hl_host_bytes output);
    /* Apply guest ownership after creation without exposing a native descriptor. */
    hl_host_result (*set_owner)(void *context, hl_host_handle file, uint32_t uid, uint32_t gid);
    /* Resolve beneath a pinned directory; returned handles are independently closeable. */
    hl_host_result (*resolve_beneath)(void *context, hl_host_handle root, const char *path, size_t path_size,
                                      uint32_t policy, hl_host_file_resolution *output);
    hl_host_result (*sync_range)(void *context, hl_host_handle file, uint64_t offset, uint64_t size, uint32_t flags);
    hl_host_result (*sync_filesystem)(void *context, hl_host_handle file);
    /*
     * Atomically open a relative path beneath root. Intermediate components are
     * resolved beneath pinned directories and the final component is always
     * opened without following a symlink. Creation therefore cannot escape root.
     */
    hl_host_result (*open_beneath)(void *context, hl_host_handle root, const char *path, size_t path_size,
                                   uint32_t access, uint32_t creation, uint32_t permissions, uint32_t resolve_policy);
    hl_host_result (*allocate_range)(void *context, hl_host_handle file, uint32_t mode, uint64_t offset, uint64_t size);
    hl_host_result (*filesystem_metadata)(void *context, hl_host_handle file, hl_host_filesystem_metadata *output);
    /* Change only permission bits on an opaque file. Guest ownership virtualization is a Linux-front job. */
    hl_host_result (*set_permissions)(void *context, hl_host_handle file, uint32_t permissions);
    /* Atomically update access and modification times on the open object. */
    hl_host_result (*set_times)(void *context, hl_host_handle file, const hl_host_file_time times[2]);
    /* Consume complete entries from the open directory's shared OFD cursor. */
    hl_host_result (*read_directory)(void *context, hl_host_handle file, hl_host_file_entry *entries,
                                     uint32_t entry_capacity, uint32_t byte_capacity);
    /* Relative namespace construction. Paths are byte spans without a trailing NUL. */
    hl_host_result (*make_directory)(void *context, hl_host_handle directory, const char *path, size_t path_size,
                                     uint32_t permissions);
    hl_host_result (*make_symlink)(void *context, const char *target, size_t target_size, hl_host_handle directory,
                                   const char *path, size_t path_size);
    hl_host_result (*make_link)(void *context, hl_host_handle old_directory, const char *old_path, size_t old_path_size,
                                hl_host_handle new_directory, const char *new_path, size_t new_path_size,
                                uint32_t flags);
    hl_host_result (*make_fifo)(void *context, hl_host_handle directory, const char *path, size_t path_size,
                                uint32_t permissions);
    /* Verify host-private cache input without exposing a native uid to portable layers. */
    hl_host_result (*validate_private_regular)(void *context, hl_host_handle file);
    /* Publish a complete private file through a unique temporary and atomic replacement. */
    hl_host_result (*store_private_atomic)(void *context, hl_host_handle directory, const char *path, size_t path_size,
                                           hl_host_const_bytes input, uint32_t permissions);
    /* Verify a pinned directory is owner-private before namespace transactions use it. */
    hl_host_result (*validate_private_directory)(void *context, hl_host_handle directory);
    /* Remove an empty directory relative to an opaque directory handle. */
    hl_host_result (*remove_directory)(void *context, hl_host_handle directory, const char *path, size_t path_size);
} hl_host_file_services;

#define HL_HOST_DEADLINE_INFINITE UINT64_MAX

typedef int32_t (*hl_host_process_entry)(void *entry_context);

typedef enum hl_host_process_exit_kind {
    HL_HOST_PROCESS_EXIT_CODE = 1,
    HL_HOST_PROCESS_EXIT_SIGNAL = 2
} hl_host_process_exit_kind;

enum {
    HL_HOST_PROCESS_TERMINATE_INTERRUPT = 1,
    HL_HOST_PROCESS_TERMINATE_FORCE = 2,
    /* Add a Linux guest signal number (1..64) to this base. */
    HL_HOST_PROCESS_TERMINATE_SIGNAL = 0x100
};

typedef struct hl_host_process_services {
    HL_ABI_HEADER;
    /* Run an already-loaded entry in an isolated clone of the current process. */
    hl_host_result (*spawn_cloned)(void *context, hl_host_process_entry entry, void *entry_context);
    /*
     * deadline_ns is an absolute host monotonic-clock timestamp. Zero polls and
     * HL_HOST_DEADLINE_INFINITE blocks without a deadline. Completion is retained
     * until close, so concurrent and repeated waiters receive the same result.
     * On success, value is the exit value and detail is hl_host_process_exit_kind.
     */
    hl_host_result (*wait)(void *context, hl_host_handle process, uint64_t deadline_ns);
    hl_host_result (*terminate)(void *context, hl_host_handle process, uint32_t reason);
    hl_host_result (*close)(void *context, hl_host_handle process);
    /* Consume a fork bracket previously acquired through sync.fork_prepare. */
    hl_host_result (*spawn_prepared)(void *context, hl_host_process_entry entry, void *entry_context);
} hl_host_process_services;

typedef struct hl_host_event_record {
    uint64_t token;
    uint32_t readiness;
    uint32_t flags;
} hl_host_event_record;

typedef struct hl_host_event_services {
    HL_ABI_HEADER;
    hl_host_result (*create)(void *context);
    hl_host_result (*control)(void *context, hl_host_handle pollset, uint32_t operation, hl_host_handle object,
                              uint64_t token, uint32_t interests);
    hl_host_result (*wait)(void *context, hl_host_handle pollset, hl_host_event_record *events, size_t event_capacity,
                           uint64_t deadline_ns);
    hl_host_result (*wake)(void *context, hl_host_handle pollset);
    hl_host_result (*close)(void *context, hl_host_handle pollset);
    /* Timers use absolute host-monotonic deadlines. interval_ns zero selects one-shot delivery. */
    hl_host_result (*arm_timer)(void *context, hl_host_handle pollset, uint64_t token, uint64_t deadline_ns,
                                uint64_t interval_ns);
    hl_host_result (*disarm_timer)(void *context, hl_host_handle pollset, uint64_t token);
} hl_host_event_services;

typedef struct hl_host_network_services {
    HL_ABI_HEADER;
    hl_host_result (*socket)(void *context, uint32_t family, uint32_t type, uint32_t protocol);
    hl_host_result (*bind)(void *context, hl_host_handle socket, const hl_host_network_address *address);
    hl_host_result (*connect)(void *context, hl_host_handle socket, const hl_host_network_address *address);
    hl_host_result (*send)(void *context, hl_host_handle socket, hl_host_const_bytes data, uint32_t flags);
    hl_host_result (*receive)(void *context, hl_host_handle socket, hl_host_bytes data, uint32_t flags);
    hl_host_result (*close)(void *context, hl_host_handle socket);
} hl_host_network_services;

typedef struct hl_host_shared_memory_services {
    HL_ABI_HEADER;
    /* create returns a reopen identity in detail; it remains valid while the source handle is live. */
    hl_host_result (*create)(void *context, uint64_t size, uint32_t flags);
    /* open duplicates a live identity into an independently resizable and closeable handle. */
    hl_host_result (*open)(void *context, uint64_t identity, uint32_t flags);
    hl_host_result (*resize)(void *context, hl_host_handle object, uint64_t size);
    hl_host_result (*close)(void *context, hl_host_handle object);
} hl_host_shared_memory_services;

/* Pollable unsigned 64-bit counter. UINT64_MAX is reserved and may never be stored. */
typedef struct hl_host_counter_services {
    HL_ABI_HEADER;
    hl_host_result (*create)(void *context, uint64_t initial, uint32_t flags);
    /* read returns the consumed value; semaphore mode consumes and returns one. */
    hl_host_result (*read)(void *context, hl_host_handle counter);
    hl_host_result (*write)(void *context, hl_host_handle counter, uint64_t value);
    hl_host_result (*get_flags)(void *context, hl_host_handle counter);
    hl_host_result (*set_flags)(void *context, hl_host_handle counter, uint32_t flags);
    hl_host_result (*duplicate)(void *context, hl_host_handle counter);
    /* Non-consuming readiness; value is a HL_HOST_READY_* mask. */
    hl_host_result (*readiness)(void *context, hl_host_handle counter, uint32_t interests);
    /* subscribe returns an independently closeable subscription handle. */
    hl_host_result (*subscribe)(void *context, hl_host_handle counter, void (*notify)(void *, uint64_t), void *observer,
                                uint64_t token);
    /* Synchronously quiesces the callback before returning. */
    hl_host_result (*unsubscribe)(void *context, hl_host_handle subscription);
    hl_host_result (*close)(void *context, hl_host_handle counter);
} hl_host_counter_services;

enum { HL_HOST_TRANSFER_MAX_DATA = 256, HL_HOST_TRANSFER_MAX_ATTACHMENTS = 4, HL_HOST_TRANSFER_KIND_COUNTER = 1 };

enum {
    HL_HOST_TRANSFER_READ = 1u << 0,
    HL_HOST_TRANSFER_WRITE = 1u << 1,
    HL_HOST_TRANSFER_WAIT = 1u << 2,
    HL_HOST_TRANSFER_CONTROL = 1u << 3
};

typedef struct hl_host_transfer_attachment {
    hl_host_handle object;
    uint32_t kind;
    uint32_t rights;
} hl_host_transfer_attachment;

/*
 * Host-owned message channels transfer object identity, never native descriptor numbers.
 * send retains each object until receive creates a receiver-local handle or the channel is closed.
 */
typedef struct hl_host_transfer_services {
    HL_ABI_HEADER;
    /* Returns the two independently closeable endpoints in value and detail. */
    hl_host_result (*channel_pair)(void *context);
    hl_host_result (*send)(void *context, hl_host_handle channel, hl_host_const_bytes data,
                           const hl_host_transfer_attachment *attachments, uint32_t attachment_count);
    /* value is byte count and detail is attachment count. A successful receive consumes one message. */
    hl_host_result (*receive)(void *context, hl_host_handle channel, hl_host_bytes data,
                              hl_host_transfer_attachment *attachments, uint32_t attachment_capacity);
    /* Duplicate aliases the same endpoint and queued-message stream. */
    hl_host_result (*duplicate)(void *context, hl_host_handle channel);
    hl_host_result (*close)(void *context, hl_host_handle channel);
} hl_host_transfer_services;

enum {
    HL_HOST_DIRECTORY_ACCESS = 1u << 0,
    HL_HOST_DIRECTORY_MODIFY = 1u << 1,
    HL_HOST_DIRECTORY_CREATE = 1u << 2,
    HL_HOST_DIRECTORY_DELETE = 1u << 3,
    HL_HOST_DIRECTORY_RENAME = 1u << 4,
    HL_HOST_DIRECTORY_ATTRIB = 1u << 5,
    HL_HOST_DIRECTORY_IGNORED = 1u << 6
};

#define HL_HOST_DIRECTORY_ONESHOT UINT32_C(0x80000000)

typedef struct hl_host_directory_record {
    uint64_t token;
    uint32_t changes;
    uint32_t flags;
} hl_host_directory_record;

/* Host-owned, pollable directory-change queue. read consumes complete records only. */
typedef struct hl_host_directory_services {
    HL_ABI_HEADER;
    hl_host_result (*create)(void *context);
    hl_host_result (*add)(void *context, hl_host_handle instance, hl_host_handle file, uint64_t token,
                          uint32_t interests);
    hl_host_result (*modify)(void *context, hl_host_handle instance, uint64_t token, uint32_t interests);
    hl_host_result (*remove)(void *context, hl_host_handle instance, uint64_t token);
    hl_host_result (*read)(void *context, hl_host_handle instance, hl_host_directory_record *records,
                           uint32_t capacity);
    hl_host_result (*duplicate)(void *context, hl_host_handle instance);
    hl_host_result (*close)(void *context, hl_host_handle instance);
} hl_host_directory_services;

/*
 * Which population of processes can see a parking spot.
 *
 * PRIVATE spots are visible only inside the calling process and may be identified by a plain
 * virtual address. SHARED spots must be reachable from a process that has never seen the waiter's
 * address, so the caller supplies a key it has already canonicalised across processes as well as
 * its own mapping of the word. A host is free to use either -- one whose primitive is keyed on
 * physical page identity ignores the key, one whose primitive is a named kernel object ignores
 * the address -- so a caller must supply both consistently rather than choosing.
 */
enum { HL_HOST_PARK_PRIVATE = 0, HL_HOST_PARK_SHARED = 1 };

/* Opaque, non-recursive host mutexes. Callers must pair lock/unlock and exclude close while in use. */
typedef struct hl_host_sync_services {
    HL_ABI_HEADER;
    hl_host_result (*mutex_create)(void *context);
    hl_host_result (*mutex_lock)(void *context, hl_host_handle mutex);
    hl_host_result (*mutex_unlock)(void *context, hl_host_handle mutex);
    hl_host_result (*mutex_close)(void *context, hl_host_handle mutex);
    hl_host_result (*fork_prepare)(void *context);
    hl_host_result (*fork_parent)(void *context);
    hl_host_result (*fork_child)(void *context);
    /* --- appended in HL_HOST_SYNC_ABI 3 --- */
    /*
     * Compare-and-block on a word, the one primitive the contract had no form of at all. A guest
     * futex, a process-shared mutex, a semaphore and a thread join are all this operation plus
     * bookkeeping the guest layer already owns; without it each one is a busy loop or a lie.
     *
     * waiter names the blocking thread in the CALLER's own numbering. It is deliberately not a
     * native thread id: no other operation in this contract hands out or accepts one, the caller
     * already has a thread table, and a provider only needs the identity to be stable and unique
     * among live waiters. A waiter identity may be reused once its previous owner is gone.
     *
     * The compare and the enqueue must be one indivisible step, which is why expected and
     * compare_size are here and not left to the caller: a caller that tests the word itself and
     * then asks to sleep has already lost the wake that landed in between. compare_size is 4 or
     * 8; a host whose primitive cannot compare that width answers HL_STATUS_NOT_SUPPORTED rather
     * than comparing a different one.
     *
     * deadline_ns is an absolute host monotonic timestamp; HL_HOST_DEADLINE_INFINITE blocks
     * without one and zero polls.
     *
     * Spurious wakes are permitted: HL_STATUS_OK means only "re-read the word". That is the
     * futex contract, and it is what lets a host wake at queue or bucket granularity instead of
     * pretending to an exactness its primitive does not have.
     *
     *   HL_STATUS_OK             woken, or spuriously woken -- re-check and decide
     *   HL_STATUS_WOULD_BLOCK    the word already differed; nothing was enqueued
     *   HL_STATUS_TIMED_OUT      deadline reached
     *   HL_STATUS_INTERRUPTED    interrupt_park named this waiter
     *   HL_STATUS_NOT_SUPPORTED  this scope or width has no host primitive
     */
    hl_host_result (*park)(void *context, uint64_t waiter, uint32_t scope, uint64_t key, const void *address,
                           uint64_t expected, uint32_t compare_size, uint64_t deadline_ns);
    /*
     * Release at most count waiters on (scope, key, address); UINT32_MAX releases all of them.
     * value is the number the host can prove it released, which may be fewer than it actually
     * woke -- a host waking a whole queue reports what it knows and leaves exact selection to the
     * caller's own bookkeeping. The word is NOT read: releasing a waiter whose word is unchanged
     * is legal and is what makes interruption expressible at all.
     */
    hl_host_result (*unpark)(void *context, uint32_t scope, uint64_t key, const void *address, uint32_t count);
    /*
     * Make one waiter's park return HL_STATUS_INTERRUPTED, so a guest signal can be delivered to
     * a thread blocked in a wait. This is why park exists in this contract rather than being left
     * to whatever the guest layer could build over a mutex: EINTR on a wait is load-bearing for
     * signal delivery, and a wait primitive that cannot be interrupted cannot carry it.
     *
     * The interruption is recorded against the waiter, not against an outstanding wait, so the
     * signal that arrives just before the thread parks is not lost: the next park consumes the
     * record and returns HL_STATUS_INTERRUPTED without blocking. It is consumed by exactly one
     * park -- a repeated call before that park still interrupts once.
     *
     * Idempotent, safe to call whether or not the waiter is parked or has ever parked, and usable
     * from a signal-delivery context: providers must not allocate, take a lock that ordinary code
     * can hold, or log.
     */
    hl_host_result (*interrupt_park)(void *context, uint64_t waiter);
} hl_host_sync_services;

enum {
    HL_HOST_WATCH_SIZE = 1u << 0,
    HL_HOST_WATCH_DATA = 1u << 1,
    HL_HOST_WATCH_IDENTITY = 1u << 2,
    HL_HOST_WATCH_DELETED = 1u << 3
};

typedef struct hl_host_watch_record {
    uint64_t generation;
    uint64_t stable_device;
    uint64_t stable_object;
    uint64_t size;
    uint32_t changes;
    uint32_t reserved;
} hl_host_watch_record;

/* Pull-based file notifications. Watch handles are accepted by event.control;
   readiness wakes the dispatcher, which drains/querys at an engine safe point.
   query returns current state even when the host coalesced notifications. */
typedef struct hl_host_watch_services {
    HL_ABI_HEADER;
    hl_host_result (*open)(void *context, hl_host_handle file);
    hl_host_result (*query)(void *context, hl_host_handle watch, hl_host_watch_record *record);
    hl_host_result (*drain)(void *context, hl_host_handle watch, hl_host_watch_record *records, size_t capacity);
    hl_host_result (*close)(void *context, hl_host_handle watch);
} hl_host_watch_services;

enum {
    HL_HOST_STREAM_NONBLOCK = 1u << 0,
    HL_HOST_STREAM_SOURCE_POSITIONED = 1u << 1,
    HL_HOST_STREAM_DESTINATION_POSITIONED = 1u << 2
};

/* Opaque byte streams. move consumes exactly the bytes reported in value and never consumes bytes
 * which the destination did not accept. source_offset/destination_offset are used only with their
 * corresponding POSITIONED flag; sequential endpoints advance their owned position atomically. */
typedef struct hl_host_stream_services {
    HL_ABI_HEADER;
    /* value is the read endpoint and detail is the write endpoint. */
    hl_host_result (*pipe_pair)(void *context, uint32_t flags);
    hl_host_result (*read)(void *context, hl_host_handle stream, hl_host_bytes output);
    hl_host_result (*write)(void *context, hl_host_handle stream, hl_host_const_bytes input);
    hl_host_result (*duplicate)(void *context, hl_host_handle stream);
    hl_host_result (*close)(void *context, hl_host_handle stream);
    hl_host_result (*set_status_flags)(void *context, hl_host_handle stream, uint32_t flags);
    /* value is a HL_HOST_READY_* mask. */
    hl_host_result (*readiness)(void *context, hl_host_handle stream, uint32_t interests);
    /* Endpoint kinds come from the host handle table. File endpoints must be POSITIONED so this
     * operation never races or mutates a file OFD offset; file-to-file movement is rejected. */
    hl_host_result (*move)(void *context, hl_host_handle source, uint64_t source_offset, hl_host_handle destination,
                           uint64_t destination_offset, uint64_t size, uint32_t flags);
} hl_host_stream_services;

/*
 * What the host device itself is doing to the byte stream. Abstract capabilities, not a native
 * mode word: a host is asked which of these it is applying and told which to apply, and nothing
 * about the guest's terminal vocabulary crosses the seam.
 */
enum {
    /* Input bytes arrive as the device produced them: no host line editing or buffering. */
    HL_HOST_TERMINAL_RAW_INPUT = 1u << 0,
    /* The host echoes input bytes to the display. */
    HL_HOST_TERMINAL_ECHO = 1u << 1,
    /* The host turns interrupt/quit/suspend keys into an out-of-band event of its own instead of
     * delivering them as bytes. Clearing this is what makes those keys readable. */
    HL_HOST_TERMINAL_SIGNALS = 1u << 2,
    /* The host consumes start/stop flow-control bytes rather than delivering them. */
    HL_HOST_TERMINAL_FLOW_CONTROL = 1u << 3,
    /* The host rewrites outbound bytes (line endings, escape handling). */
    HL_HOST_TERMINAL_OUTPUT_PROCESSING = 1u << 4
};

typedef struct hl_host_terminal_size {
    uint32_t columns;
    uint32_t rows;
    uint32_t pixel_width;
    uint32_t pixel_height;
} hl_host_terminal_size;

/*
 * Facts about a terminal device, and nothing about Linux.
 *
 * This group exists because file.metadata provably cannot answer "is this a terminal": a host
 * whose null device reports the same character-device type as its console makes the file type a
 * necessary and not a sufficient test, so a guest isatty answered from metadata alone would say
 * yes to /dev/null. probe is the discriminator, and it is the reason this is a group rather than
 * a field.
 *
 * The split is deliberate and is the whole design. The host owns facts about the DEVICE: whether
 * it is one, the bytes it produces and accepts, which processing it is applying, its size, and
 * when that size changes. The guest layer owns everything about LINUX: the terminal attribute
 * structure and its control characters, canonical-mode buffering and line editing, echo policy,
 * signal generation, minimum-and-timeout reads, output post-processing. Putting a generic
 * device-control call in this contract instead would push Linux request numbers and structure
 * layouts into every host, which is precisely what every other group here is typed to avoid.
 *
 * Handles are the same opaque file handles the file group produces.
 */
typedef struct hl_host_terminal_services {
    HL_ABI_HEADER;
    /* value is non-zero when the handle names an interactive terminal. */
    hl_host_result (*probe)(void *context, hl_host_handle handle);
    hl_host_result (*get_mode)(void *context, hl_host_handle handle, uint32_t *mode);
    /* Apply exactly the named HL_HOST_TERMINAL_* set. A host that cannot express one of them
     * refuses rather than applying a near miss: leaving echo on when it was asked to be off is a
     * disclosure, not a cosmetic difference. */
    hl_host_result (*set_mode)(void *context, hl_host_handle handle, uint32_t mode);
    hl_host_result (*get_size)(void *context, hl_host_handle handle, hl_host_terminal_size *size);
    hl_host_result (*set_size)(void *context, hl_host_handle handle, const hl_host_terminal_size *size);
    /* Raw bytes to and from the device. value is the count transferred. read must not block on
     * device activity that yields no bytes -- a size change is such activity on at least one host,
     * where the input object is signalled and a blocking read of it then never returns. */
    hl_host_result (*read)(void *context, hl_host_handle handle, hl_host_bytes output);
    hl_host_result (*write)(void *context, hl_host_handle handle, hl_host_const_bytes input);
    /*
     * A borrowed, independently closeable object that becomes ready when the size changes, for a
     * caller that must learn about a resize without reading. This is an operation rather than a
     * size query alone because on a host where the resize is delivered in the input stream, the
     * obvious composition -- wait for input, then read it -- deadlocks: the wait is satisfied by
     * the resize and the read then blocks for a keystroke that never comes.
     *
     * A host that delivers resizes by some other means it does not own -- a process-directed
     * signal, say -- answers HL_STATUS_NOT_SUPPORTED, and its caller uses that other means.
     */
    hl_host_result (*size_change_event)(void *context, hl_host_handle handle);
} hl_host_terminal_services;

/* Optional POSIX-host adapter for native ancillary descriptor transport. A borrow aliases the same
 * open-file description, is CLOEXEC, and must be released on every path. */
typedef struct hl_host_posix_attachment_services {
    HL_ABI_HEADER;
    hl_host_result (*borrow_file)(void *context, hl_host_handle file);
    hl_host_result (*borrow_file_at_least)(void *context, hl_host_handle file, uint32_t minimum_descriptor);
    hl_host_result (*release)(void *context, uint64_t borrowed_descriptor);
} hl_host_posix_attachment_services;

typedef struct hl_host_services {
    HL_ABI_HEADER;
    uint64_t capabilities;
    void *context;
    const hl_host_memory_services *memory;
    const hl_host_clock_services *clock;
    const hl_host_log_services *log;
    const hl_host_file_services *file;
    const hl_host_process_services *process;
    const hl_host_event_services *event;
    const hl_host_network_services *network;
    const hl_host_shared_memory_services *shared_memory;
    const hl_host_sync_services *sync;
    const hl_host_counter_services *counter;
    const hl_host_transfer_services *transfer;
    const hl_host_directory_services *directory;
    const hl_host_watch_services *watch;
    const hl_host_stream_services *stream;
    const hl_host_posix_attachment_services *posix_attachment;
    const hl_host_terminal_services *terminal;
} hl_host_services;

HL_API hl_status hl_host_services_validate(const hl_host_services *services, uint64_t required_capabilities);

HL_EXTERN_C_END

#endif
