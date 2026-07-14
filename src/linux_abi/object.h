#ifndef HL_LINUX_OBJECT_H
#define HL_LINUX_OBJECT_H

#include "hl/linux_abi.h"

enum {
    HL_LINUX_READY_READ = 1u << 0,
    HL_LINUX_READY_WRITE = 1u << 1,
    HL_LINUX_READY_PRIORITY = 1u << 2,
    HL_LINUX_READY_ERROR = 1u << 3,
    HL_LINUX_READY_HANGUP = 1u << 4
};

typedef struct hl_linux_object_ops {
    int64_t (*read)(void *context, void *buffer, size_t size);
    int64_t (*write)(void *context, const void *buffer, size_t size);
    int64_t (*status)(void *context, hl_linux_file_status *status);
    uint32_t (*readiness)(void *context, uint32_t interests);
    hl_status (*clone)(void *context, void **child_context);
    hl_status (*close)(void *context);
} hl_linux_object_ops;

typedef struct hl_linux_object_pin {
    hl_linux_abi *linux_abi;
    hl_linux_ofd ofd;
    uint32_t generation;
    const hl_linux_object_ops *ops;
    void *context;
} hl_linux_object_pin;

hl_status hl_linux_object_install(hl_linux_abi *linux_abi, const hl_linux_object_ops *ops, void *context, uint32_t kind,
                                  uint32_t status_flags, uint32_t descriptor_flags, hl_linux_fd *out_fd);
hl_status hl_linux_object_install_at(hl_linux_abi *linux_abi, hl_linux_fd fd, const hl_linux_object_ops *ops,
                                     void *context, uint32_t kind, uint32_t status_flags, uint32_t descriptor_flags);
hl_status hl_linux_object_pin_fd(hl_linux_abi *linux_abi, hl_linux_fd fd, hl_linux_object_pin *pin);
void hl_linux_object_unpin(hl_linux_object_pin *pin);
uint32_t hl_linux_object_ready(hl_linux_object_pin *pin, uint32_t interests);

#endif
