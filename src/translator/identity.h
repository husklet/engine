#ifndef HL_TRANSLATOR_IDENTITY_H
#define HL_TRANSLATOR_IDENTITY_H

#include <stdint.h>

#include <hl/host_services.h>

uint64_t hl_identity_name(const char *name);
uint64_t hl_identity_file(const hl_host_file_metadata *metadata);
uint64_t hl_identity_source(const hl_host_services *services, const char *path);
uint64_t hl_identity_mix(uint64_t program, uint64_t interpreter, uint64_t engine, uint64_t name);
uint64_t hl_identity_configuration(uint64_t build, uint32_t guest_isa, uint32_t host_isa, uint64_t modes);

#endif
