#ifndef HL_HOST_SERVICES_H
#define HL_HOST_SERVICES_H

#include "hl/base.h"

HL_EXTERN_C_BEGIN

#define HL_HOST_SERVICES_ABI 2u
#define HL_HOST_MEMORY_ABI 2u
#define HL_HOST_CLOCK_ABI 2u
#define HL_HOST_LOG_ABI 1u
#define HL_HOST_FILE_ABI 10u
#define HL_HOST_PROCESS_ABI 3u
#define HL_HOST_EVENT_ABI 2u
#define HL_HOST_NETWORK_ABI 1u
#define HL_HOST_SHARED_MEMORY_ABI 1u
#define HL_HOST_COUNTER_ABI 2u
#define HL_HOST_SYNC_ABI 2u
#define HL_HOST_TRANSFER_ABI 2u
#define HL_HOST_DIRECTORY_ABI 1u

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
    HL_HOST_CAP_DIRECTORY = UINT64_C(1) << 14
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

enum { HL_HOST_COUNTER_SEMAPHORE = 1u << 0, HL_HOST_COUNTER_NONBLOCK = 1u << 1 };

enum { HL_HOST_FILE_CREATE = 1u << 0, HL_HOST_FILE_EXCLUSIVE = 1u << 1, HL_HOST_FILE_TRUNCATE = 1u << 2 };

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

enum { HL_HOST_CODE_DUAL_ALIAS = 1u << 0 };

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
    uint64_t reserved;
} hl_host_code_mapping;

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
    uint32_t type;
    uint32_t permissions;
} hl_host_file_metadata;

typedef struct hl_host_iovec {
    uint64_t address;
    uint64_t size;
} hl_host_iovec;

enum { HL_HOST_FILE_IOV_MAX = 1024 };

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
} hl_host_file_services;

#define HL_HOST_DEADLINE_INFINITE UINT64_MAX

typedef int32_t (*hl_host_process_entry)(void *entry_context);

typedef enum hl_host_process_exit_kind {
    HL_HOST_PROCESS_EXIT_CODE = 1,
    HL_HOST_PROCESS_EXIT_SIGNAL = 2
} hl_host_process_exit_kind;

enum { HL_HOST_PROCESS_TERMINATE_INTERRUPT = 1, HL_HOST_PROCESS_TERMINATE_FORCE = 2 };

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
} hl_host_sync_services;

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
} hl_host_services;

HL_API hl_status hl_host_services_validate(const hl_host_services *services, uint64_t required_capabilities);

HL_EXTERN_C_END

#endif
