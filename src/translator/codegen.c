#include "hl/codegen.h"

#include "host/aarch64/aarch64_codegen.h"

#include <string.h>

hl_status hl_code_buffer_init(hl_code_buffer *buffer, void *storage, size_t capacity) {
    if (buffer == NULL || storage == NULL || capacity == 0) return HL_STATUS_INVALID_ARGUMENT;
    memset(buffer, 0, sizeof(*buffer));
    buffer->abi = HL_CODEGEN_ABI;
    buffer->size = sizeof(*buffer);
    buffer->data = storage;
    buffer->capacity = capacity;
    return HL_STATUS_OK;
}

hl_status hl_codegen_block(uint32_t host_isa, const hl_ir_block *block, hl_code_buffer *output) {
    if (block == NULL || output == NULL) return HL_STATUS_INVALID_ARGUMENT;
    if (output->abi != HL_CODEGEN_ABI || output->size < sizeof(*output)) return HL_STATUS_ABI_MISMATCH;
    output->code_size = 0;
    if (hl_ir_validate(block, NULL) != HL_STATUS_OK) return HL_STATUS_CORRUPT;
    if (host_isa == HL_HOST_ISA_AARCH64) return hl_codegen_aarch64(block, output);
    return HL_STATUS_NOT_SUPPORTED;
}
