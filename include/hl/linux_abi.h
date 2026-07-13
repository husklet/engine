#ifndef HL_LINUX_ABI_H
#define HL_LINUX_ABI_H

#include "hl/base.h"
#include "hl/host_services.h"

HL_EXTERN_C_BEGIN

#define HL_LINUX_ABI_VERSION 1u
#define HL_LINUX_FD_LIMIT 65536u
#define HL_LINUX_OFD_LIMIT 65536u

typedef uint32_t hl_linux_fd;
typedef uint32_t hl_linux_ofd;

typedef struct hl_linux_ofd_entry {
    hl_host_handle host_handle;
    uint64_t offset;
    uint32_t status_flags;
    uint32_t references;
    uint32_t generation;
    uint32_t kind;
} hl_linux_ofd_entry;

typedef struct hl_linux_fd_entry {
    hl_linux_ofd ofd;
    uint32_t descriptor_flags;
    uint32_t generation;
} hl_linux_fd_entry;

typedef struct hl_linux_abi {
    HL_ABI_HEADER;
    const hl_host_services *host;
    hl_linux_fd_entry *fds;
    uint32_t fd_capacity;
    hl_linux_ofd_entry *ofds;
    uint32_t ofd_capacity;
} hl_linux_abi;

HL_API hl_status hl_linux_abi_init(hl_linux_abi *linux_abi, const hl_host_services *host, hl_linux_fd_entry *fd_storage,
                                   uint32_t fd_capacity, hl_linux_ofd_entry *ofd_storage, uint32_t ofd_capacity);
HL_API hl_status hl_linux_fd_install(hl_linux_abi *linux_abi, hl_host_handle host_handle, uint32_t status_flags,
                                     uint32_t descriptor_flags, hl_linux_fd *out_fd);
HL_API hl_status hl_linux_fd_dup(hl_linux_abi *linux_abi, hl_linux_fd source, uint32_t descriptor_flags,
                                 hl_linux_fd *out_fd);
HL_API hl_status hl_linux_fd_close(hl_linux_abi *linux_abi, hl_linux_fd fd, hl_host_handle *last_host_handle);
HL_API hl_status hl_linux_fd_get(const hl_linux_abi *linux_abi, hl_linux_fd fd, const hl_linux_fd_entry **fd_entry,
                                 const hl_linux_ofd_entry **ofd_entry);

HL_EXTERN_C_END

#endif
