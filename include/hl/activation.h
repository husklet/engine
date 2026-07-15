#ifndef HL_ACTIVATION_H
#define HL_ACTIVATION_H

#include "base.h"
#include "engine.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hl_activation_process hl_activation_process;
typedef struct hl_activation_stdio {
    int32_t input;
    int32_t output;
    int32_t error;
} hl_activation_stdio;

HL_API hl_status hl_activation_start(const char *executable, uint32_t guest_isa, const char *config_path,
                                     hl_activation_process **out_process);
HL_API hl_status hl_activation_start_with_stdio(const char *executable, uint32_t guest_isa,
                                                const char *config_path, const hl_activation_stdio *stdio,
                                                hl_activation_process **out_process);
HL_API hl_status hl_activation_wait(hl_activation_process *process, hl_engine_exit *out_exit);
HL_API hl_status hl_activation_try_wait(hl_activation_process *process, uint32_t *out_ready,
                                       hl_engine_exit *out_exit);
HL_API hl_status hl_activation_kill(hl_activation_process *process);
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
