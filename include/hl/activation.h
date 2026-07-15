#ifndef HL_ACTIVATION_H
#define HL_ACTIVATION_H

#include "base.h"
#include "engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Reexecutes the current application and activates the embedded guest backend
 * before application main. config_path names an ABI5 launch file and is
 * consumed by the child. Both paths must be absolute. */
HL_API hl_status hl_activation_spawn(const char *executable, uint32_t guest_isa, const char *config_path,
                                     hl_engine_exit *out_exit);

#ifdef __cplusplus
}
#endif
#endif
