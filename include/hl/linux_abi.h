#ifndef HL_LINUX_ABI_H
#define HL_LINUX_ABI_H

#include "hl/base.h"
#include "hl/host_services.h"

#include <stdatomic.h>
#include <threads.h>

HL_EXTERN_C_BEGIN

#define HL_LINUX_ABI_VERSION 1u
#define HL_LINUX_FD_LIMIT 65536u
#define HL_LINUX_OFD_LIMIT 65536u

typedef uint32_t hl_linux_fd;
typedef uint32_t hl_linux_ofd;

/* Linux guest errno numbers returned by this library as negative syscall results. */
typedef enum hl_linux_errno {
    HL_LINUX_ENOENT = 2,
    HL_LINUX_EINTR = 4,
    HL_LINUX_EIO = 5,
    HL_LINUX_EBADF = 9,
    HL_LINUX_EAGAIN = 11,
    HL_LINUX_ENOMEM = 12,
    HL_LINUX_EACCES = 13,
    HL_LINUX_EBUSY = 16,
    HL_LINUX_EEXIST = 17,
    HL_LINUX_EINVAL = 22,
    HL_LINUX_ENFILE = 23,
    HL_LINUX_EMFILE = 24,
    HL_LINUX_ESPIPE = 29,
    HL_LINUX_ENOSYS = 38,
    HL_LINUX_EOVERFLOW = 75
} hl_linux_errno;

enum {
    HL_LINUX_O_ACCMODE = 00000003u,
    HL_LINUX_O_RDONLY = 00000000u,
    HL_LINUX_O_WRONLY = 00000001u,
    HL_LINUX_O_RDWR = 00000002u,
    HL_LINUX_O_CREAT = 00000100u,
    HL_LINUX_O_EXCL = 00000200u,
    HL_LINUX_O_TRUNC = 00001000u,
    HL_LINUX_O_APPEND = 00002000u,
    HL_LINUX_O_DIRECTORY = 00200000u,
    HL_LINUX_O_CLOEXEC = 02000000u,
    HL_LINUX_FD_CLOEXEC = 1u
};

enum {
    HL_LINUX_SEEK_SET = 0,
    HL_LINUX_SEEK_CUR = 1,
    HL_LINUX_SEEK_END = 2,
    HL_LINUX_F_DUPFD = 0,
    HL_LINUX_F_GETFD = 1,
    HL_LINUX_F_SETFD = 2,
    HL_LINUX_F_GETFL = 3,
    HL_LINUX_F_DUPFD_CLOEXEC = 1030
};

enum {
    HL_LINUX_S_IFIFO = 0010000u,
    HL_LINUX_S_IFCHR = 0020000u,
    HL_LINUX_S_IFDIR = 0040000u,
    HL_LINUX_S_IFBLK = 0060000u,
    HL_LINUX_S_IFREG = 0100000u,
    HL_LINUX_S_IFLNK = 0120000u,
    HL_LINUX_S_IFSOCK = 0140000u
};

#define HL_LINUX_AT_FDCWD (-100)

typedef struct hl_linux_ofd_entry {
    /* Host object owned by this OFD. Closed when the final descriptor is closed. */
    hl_host_handle host_handle;
    /* Linux open-file-description offset, shared by dup'd descriptors. */
    uint64_t offset;
    /* Open status flags belong to the OFD; descriptor flags do not. */
    uint32_t status_flags;
    uint32_t references;
    /* Operations pin the entry while table_lock is dropped around host calls. */
    uint32_t active_operations;
    /* Final close claimed this slot; allocation cannot recycle it yet. */
    uint32_t closing;
    uint32_t generation;
    uint32_t kind;
    /* Serializes the shared offset and final close for this OFD only. */
    mtx_t io_lock;
} hl_linux_ofd_entry;

typedef struct hl_linux_fd_entry {
    /* Index zero means unused; live descriptors refer to one shared OFD. */
    hl_linux_ofd ofd;
    /* Per-descriptor flags such as FD_CLOEXEC. */
    uint32_t descriptor_flags;
    uint32_t generation;
} hl_linux_fd_entry;

/* Copyable descriptor metadata captured atomically under table ownership. */
typedef struct hl_linux_fd_snapshot {
    hl_linux_fd fd;
    hl_linux_ofd ofd;
    hl_host_handle host_handle;
    uint64_t offset;
    uint32_t status_flags;
    uint32_t descriptor_flags;
    uint32_t descriptor_generation;
    uint32_t ofd_generation;
    uint32_t descriptor_references;
    uint32_t kind;
} hl_linux_fd_snapshot;

/* Host-neutral values which the syscall marshaller can encode for either Linux guest ISA. */
typedef struct hl_linux_file_status {
    uint64_t device;
    uint64_t object;
    uint64_t size;
    uint64_t blocks_512;
    uint64_t modified_ns;
    uint32_t mode;
} hl_linux_file_status;

typedef struct hl_linux_abi {
    HL_ABI_HEADER;
    const hl_host_services *host;
    hl_linux_fd_entry *fds;
    uint32_t fd_capacity;
    hl_linux_ofd_entry *ofds;
    uint32_t ofd_capacity;
    /*
     * Serializes only descriptor lookup and OFD lifetime counters. Host calls
     * never hold it; each OFD has independent I/O ownership instead.
     */
    atomic_flag table_lock;
} hl_linux_abi;

/*
 * An initialized hl_linux_abi owns its synchronization state and must not be
 * copied. Host service callbacks invoked by it must not re-enter the same
 * hl_linux_abi instance. Contending operations use C11 synchronization only;
 * unrelated OFDs never wait for one another's host I/O.
 */

HL_API hl_status hl_linux_abi_init(hl_linux_abi *linux_abi, const hl_host_services *host, hl_linux_fd_entry *fd_storage,
                                   uint32_t fd_capacity, hl_linux_ofd_entry *ofd_storage, uint32_t ofd_capacity);
/*
 * Requires every descriptor to be closed and releases the per-OFD C11 mutexes.
 * The owner must externally exclude every concurrent call on this instance from
 * destroy's entry onward; after success the instance may only be initialized.
 */
HL_API hl_status hl_linux_abi_destroy(hl_linux_abi *linux_abi);
HL_API hl_status hl_linux_fd_install(hl_linux_abi *linux_abi, hl_host_handle host_handle, uint32_t status_flags,
                                     uint32_t descriptor_flags, hl_linux_fd *out_fd);
HL_API hl_status hl_linux_fd_dup(hl_linux_abi *linux_abi, hl_linux_fd source, uint32_t descriptor_flags,
                                 hl_linux_fd *out_fd);
HL_API hl_status hl_linux_fd_close(hl_linux_abi *linux_abi, hl_linux_fd fd, hl_host_handle *last_host_handle);
/* Returns a stable value snapshot; internal entries and mutex-bearing OFDs never escape. */
HL_API hl_status hl_linux_fd_snapshot_get(const hl_linux_abi *linux_abi, hl_linux_fd fd,
                                          hl_linux_fd_snapshot *snapshot);

/*
 * Host-agnostic Linux file-I/O syscall semantics.
 *
 * Buffers are already validated/translated guest memory owned by the caller. The
 * library resolves the guest fd to its OFD, calls only hl_host_file_services,
 * translates hl_status to a negative Linux errno, and applies Linux offset rules.
 * read() advances the shared OFD offset only on success; pread64() never changes it.
 */
HL_API int64_t hl_linux_read(hl_linux_abi *linux_abi, hl_linux_fd fd, void *buffer, size_t size);
HL_API int64_t hl_linux_pread64(hl_linux_abi *linux_abi, hl_linux_fd fd, void *buffer, size_t size, uint64_t offset);
HL_API int64_t hl_linux_write(hl_linux_abi *linux_abi, hl_linux_fd fd, const void *buffer, size_t size);
HL_API int64_t hl_linux_pwrite64(hl_linux_abi *linux_abi, hl_linux_fd fd, const void *buffer, size_t size,
                                 uint64_t offset);
/* path is translated guest memory; mode is used only with O_CREAT. */
HL_API int64_t hl_linux_openat(hl_linux_abi *linux_abi, int32_t directory_fd, const char *path, size_t path_size,
                               uint32_t flags, uint32_t mode);
/* close() invalidates this descriptor even if the host reports a late close error. */
HL_API int64_t hl_linux_close(hl_linux_abi *linux_abi, hl_linux_fd fd);
/* dup and supported fcntl commands operate only on the guest fd/OFD tables. */
HL_API int64_t hl_linux_dup(hl_linux_abi *linux_abi, hl_linux_fd fd);
HL_API int64_t hl_linux_fcntl(hl_linux_abi *linux_abi, hl_linux_fd fd, int32_t command, uint64_t argument);
/* lseek changes the shared OFD offset. SEEK_END and fstat use normalized host metadata. */
HL_API int64_t hl_linux_lseek(hl_linux_abi *linux_abi, hl_linux_fd fd, int64_t offset, int32_t whence);
HL_API int64_t hl_linux_fstat(hl_linux_abi *linux_abi, hl_linux_fd fd, hl_linux_file_status *output);

HL_EXTERN_C_END

#endif
