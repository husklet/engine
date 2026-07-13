#ifndef HL_HOST_SERVICES_H
#define HL_HOST_SERVICES_H

#include "hl/base.h"

HL_EXTERN_C_BEGIN

#define HL_HOST_SERVICES_ABI 1u
#define HL_HOST_MEMORY_ABI 1u
#define HL_HOST_CLOCK_ABI 1u
#define HL_HOST_LOG_ABI 1u
#define HL_HOST_FILE_ABI 1u
#define HL_HOST_PROCESS_ABI 1u
#define HL_HOST_EVENT_ABI 1u
#define HL_HOST_NETWORK_ABI 1u
#define HL_HOST_SHARED_MEMORY_ABI 1u
#define HL_HOST_GPU_ABI 1u

typedef uint64_t hl_host_handle;

enum {
    HL_HOST_HANDLE_INVALID = 0,
    HL_HOST_CAP_MEMORY = UINT64_C(1) << 0,
    HL_HOST_CAP_CLOCK = UINT64_C(1) << 1,
    HL_HOST_CAP_LOG = UINT64_C(1) << 2,
    HL_HOST_CAP_FAST_CLONE = UINT64_C(1) << 3,
    HL_HOST_CAP_GPU = UINT64_C(1) << 4,
    HL_HOST_CAP_FILE = UINT64_C(1) << 5,
    HL_HOST_CAP_PROCESS = UINT64_C(1) << 6,
    HL_HOST_CAP_EVENT = UINT64_C(1) << 7,
    HL_HOST_CAP_NETWORK = UINT64_C(1) << 8,
    HL_HOST_CAP_SHARED_MEMORY = UINT64_C(1) << 9,
    HL_HOST_CAP_CODE_MAPPING = UINT64_C(1) << 10
};

enum {
    HL_HOST_FILE_READ = 1u << 0,
    HL_HOST_FILE_WRITE = 1u << 1,
    HL_HOST_FILE_APPEND = 1u << 2,
    HL_HOST_FILE_DIRECTORY = 1u << 3
};

enum { HL_HOST_FILE_CREATE = 1u << 0, HL_HOST_FILE_EXCLUSIVE = 1u << 1, HL_HOST_FILE_TRUNCATE = 1u << 2 };

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
    HL_HOST_READY_ONESHOT = 1u << 5
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
} hl_host_memory_services;

typedef struct hl_host_clock_services {
    HL_ABI_HEADER;
    hl_host_result (*monotonic_ns)(void *context);
    hl_host_result (*realtime_ns)(void *context);
} hl_host_clock_services;

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

typedef struct hl_host_file_services {
    HL_ABI_HEADER;
    hl_host_result (*open_relative)(void *context, hl_host_handle directory, const char *path, size_t path_size,
                                    uint32_t access, uint32_t creation);
    hl_host_result (*read_at)(void *context, hl_host_handle file, uint64_t offset, hl_host_bytes output);
    hl_host_result (*write_at)(void *context, hl_host_handle file, uint64_t offset, hl_host_const_bytes input);
    hl_host_result (*metadata)(void *context, hl_host_handle file, hl_host_file_metadata *output);
    hl_host_result (*close)(void *context, hl_host_handle file);
} hl_host_file_services;

typedef struct hl_host_process_services {
    HL_ABI_HEADER;
    hl_host_result (*spawn)(void *context, hl_host_const_bytes state, const hl_host_handle *handles,
                            size_t handle_count);
    hl_host_result (*wait)(void *context, hl_host_handle process, uint64_t deadline_ns);
    hl_host_result (*terminate)(void *context, hl_host_handle process, uint32_t reason);
    hl_host_result (*close)(void *context, hl_host_handle process);
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
    hl_host_result (*create)(void *context, uint64_t size, uint32_t flags);
    hl_host_result (*open)(void *context, uint64_t identity, uint32_t flags);
    hl_host_result (*resize)(void *context, hl_host_handle object, uint64_t size);
    hl_host_result (*close)(void *context, hl_host_handle object);
} hl_host_shared_memory_services;

typedef struct hl_host_gpu_services {
    HL_ABI_HEADER;
    hl_host_result (*allocate)(void *context, uint32_t width, uint32_t height, uint32_t format, uint32_t usage);
    hl_host_result (*identity)(void *context, hl_host_handle allocation);
    hl_host_result (*close)(void *context, hl_host_handle allocation);
} hl_host_gpu_services;

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
    const hl_host_gpu_services *gpu;
} hl_host_services;

HL_API hl_status hl_host_services_validate(const hl_host_services *services, uint64_t required_capabilities);

HL_EXTERN_C_END

#endif
