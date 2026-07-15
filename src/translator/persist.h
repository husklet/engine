#ifndef HL_TRANSLATOR_PERSIST_H
#define HL_TRANSLATOR_PERSIST_H

#include "hl/host_services.h"

/* Trusted opaque-byte persistence for translator artifacts.  Format parsing stays
 * in the translator; all namespace, ownership and publication policy stays here. */
int hl_persist_prepare(const hl_host_services *services, const char *directory);
int hl_persist_load(const hl_host_services *services, const char *path, uint64_t limit, void **data, size_t *size);
int hl_persist_store(const hl_host_services *services, const char *path, const void *data, size_t size);
void hl_persist_remove(const hl_host_services *services, const char *path);

typedef struct hl_persist_cursor { const unsigned char *data; size_t size; size_t offset; } hl_persist_cursor;
int hl_persist_take(hl_persist_cursor *cursor, void *output, size_t size);
int hl_persist_metadata(const hl_host_services *services, const char *path, hl_host_file_metadata *metadata);

#endif
