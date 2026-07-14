#include "hl/codegen.h"

#include "host/aarch64/aarch64_codegen.h"
#include "host/x86_64/x86_64_codegen.h"

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
    hl_ir_function function;
    hl_ir_block wrapper;
    if (block == NULL || output == NULL) return HL_STATUS_INVALID_ARGUMENT;
    if (output->abi != HL_CODEGEN_ABI || output->size < sizeof(*output)) return HL_STATUS_ABI_MISMATCH;
    output->code_size = 0;
    if (hl_ir_validate(block, NULL) != HL_STATUS_OK) return HL_STATUS_CORRUPT;
    wrapper = *block;
    wrapper.label = 1;
    memset(&function, 0, sizeof(function));
    function.abi = HL_IR_ABI;
    function.size = sizeof(function);
    function.blocks = &wrapper;
    function.block_count = function.block_capacity = 1;
    function.next_value_id = block->next_value_id;
    function.entry_label = 1;
    if (host_isa == HL_HOST_ISA_AARCH64) return hl_codegen_aarch64_function(&function, output);
    if (host_isa == HL_HOST_ISA_X86_64) return hl_codegen_x86_64_function(&function, output);
    return HL_STATUS_NOT_SUPPORTED;
}

hl_status hl_codegen_function(uint32_t host_isa, const hl_ir_function *function, hl_code_buffer *output) {
    hl_status status;
    if (function == NULL || output == NULL) return HL_STATUS_INVALID_ARGUMENT;
    if (output->abi != HL_CODEGEN_ABI || output->size < sizeof(*output)) return HL_STATUS_ABI_MISMATCH;
    output->code_size = 0;
    status = hl_ir_function_validate(function, NULL, NULL);
    if (status != HL_STATUS_OK) return status == HL_STATUS_OUT_OF_MEMORY ? status : HL_STATUS_CORRUPT;
    if (host_isa == HL_HOST_ISA_AARCH64) return hl_codegen_aarch64_function(function, output);
    if (host_isa == HL_HOST_ISA_X86_64) return hl_codegen_x86_64_function(function, output);
    return HL_STATUS_NOT_SUPPORTED;
}
