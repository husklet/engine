#ifndef HL_ACTIVATION_H
#define HL_ACTIVATION_H

#include "base.h"
#include "engine.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hl_activation_process hl_activation_process;
typedef struct hl_activation_stdio {
    /* Borrowed host descriptors. A nonnegative descriptor is duplicated into
     * the reexecuted child during start; -1 inherits the application's stream.
     * Ownership never transfers, and the caller may close it as soon as start
     * returns, whether start succeeds or fails. */
    int32_t input;
    int32_t output;
    int32_t error;
} hl_activation_stdio;
typedef struct hl_terminal_size {
    uint16_t rows;
    uint16_t columns;
} hl_terminal_size;
typedef struct hl_process_domain {
    uint64_t identity[2];
} hl_process_domain;

HL_API hl_status hl_activation_start(const char *executable, uint32_t guest_isa, const char *config_path,
                                     hl_activation_process **out_process);
HL_API hl_status hl_activation_start_with_stdio(const char *executable, uint32_t guest_isa,
                                                const char *config_path, const hl_activation_stdio *stdio,
                                                hl_activation_process **out_process);
/* Experimental activation plumbing for an engine/provider transport. The descriptor is borrowed and
 * transferred with SCM_RIGHTS; it is not a guest descriptor and is never exposed through discovery. */
HL_API hl_status hl_activation_start_with_transport(const char *executable, uint32_t guest_isa,
                                                    const char *config_path, const hl_activation_stdio *stdio,
                                                    int32_t transport, hl_activation_process **out_process);
/* Starts the child in a new session with a controlling terminal. The returned
 * master descriptor is owned by the caller; stdin/stdout/stderr are merged on
 * it. The initial size is applied before the child can execute. */
HL_API hl_status hl_activation_start_terminal(const char *executable, uint32_t guest_isa,
                                              const char *config_path, hl_terminal_size size,
                                              int32_t *out_master, hl_activation_process **out_process);
HL_API hl_status hl_terminal_resize(int32_t master, hl_terminal_size size);
/* Returns the native child process identifier while the opaque handle exists. */
HL_API hl_status hl_activation_process_id(const hl_activation_process *process, uint64_t *out_process_id);
/* wait is idempotent: completed status/result values are cached in the handle. */
HL_API hl_status hl_activation_wait(hl_activation_process *process, hl_engine_exit *out_exit);
/* try_wait sets out_ready=0 without modifying out_exit while the child runs. */
HL_API hl_status hl_activation_try_wait(hl_activation_process *process, uint32_t *out_ready,
                                       hl_engine_exit *out_exit);
HL_API hl_status hl_activation_kill(hl_activation_process *process);
/* Terminates every live member whose native PID and birth identity match this domain registry.
 * Repeated termination is successful; zero identities are rejected. */
HL_API hl_status hl_activation_domain_terminate(hl_process_domain domain);
/* Destroying a live handle force-stops and reaps it; destroying a waited handle
 * only releases cached parent-side state. */
HL_API void hl_activation_process_destroy(hl_activation_process *process);

/* Reexecutes the current application and activates the embedded guest backend
 * before application main. config_path names an ABI5 launch file and is
 * consumed by the child. Both paths must be absolute. */
HL_API hl_status hl_activation_spawn(const char *executable, uint32_t guest_isa, const char *config_path,
                                     hl_engine_exit *out_exit);

#ifdef __cplusplus
}
#endif
#endif
