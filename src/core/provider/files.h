#ifndef HL_CORE_PROVIDER_FILES_H
#define HL_CORE_PROVIDER_FILES_H

#include "hl/host_services.h"
#include "client.h"

/* Process-isolated activation wrapper. Untagged handles delegate to the captured host file table. */
int hl_provider_files_install(hl_host_services *services, hl_provider_client *client);
hl_host_result hl_provider_files_open_service(uint64_t service, uint32_t access);
void hl_provider_files_revoke(void);
int hl_provider_files_is_handle(hl_host_handle handle);
uint32_t hl_provider_files_readiness(hl_host_handle handle, uint32_t interests);
uint32_t hl_provider_files_cached_readiness(hl_host_handle handle, uint32_t interests);
int hl_provider_files_subscribe(hl_host_handle handle, uint32_t interests,
                                void (*notify)(void *, uint64_t), void *opaque, uint64_t token);
void hl_provider_files_unsubscribe(hl_host_handle handle, void *opaque, uint64_t token);

#endif
