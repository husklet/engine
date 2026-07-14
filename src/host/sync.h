#ifndef HL_HOST_SYNC_H
#define HL_HOST_SYNC_H

#include "hl/host_services.h"

typedef struct hl_host_sync_registry hl_host_sync_registry;

hl_status hl_host_sync_registry_create(hl_host_sync_registry **output);
void hl_host_sync_registry_destroy(hl_host_sync_registry *registry);
hl_host_result hl_host_sync_mutex_create(hl_host_sync_registry *registry);
hl_host_result hl_host_sync_mutex_lock(hl_host_sync_registry *registry, hl_host_handle handle);
hl_host_result hl_host_sync_mutex_unlock(hl_host_sync_registry *registry, hl_host_handle handle);
hl_host_result hl_host_sync_mutex_close(hl_host_sync_registry *registry, hl_host_handle handle);
hl_host_result hl_host_sync_fork_prepare(hl_host_sync_registry *registry);
hl_host_result hl_host_sync_fork_complete(hl_host_sync_registry *registry);

#endif
