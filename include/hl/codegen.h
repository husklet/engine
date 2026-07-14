#ifndef HL_CODEGEN_H
#define HL_CODEGEN_H

#include "hl/ir.h"

HL_EXTERN_C_BEGIN

#define HL_CODEGEN_ABI 1u

typedef enum hl_host_isa { HL_HOST_ISA_AARCH64 = 1, HL_HOST_ISA_X86_64 = 2 } hl_host_isa;

typedef struct hl_code_buffer {
    HL_ABI_HEADER;
    uint8_t *data;
    size_t capacity;
    size_t code_size;
} hl_code_buffer;

/*
 * A generated block receives writable exit storage and returns an engine
 * status.  Every successful terminator initializes the complete exit record.
 * The entry owns neither the record nor any memory reachable from it.
 */
typedef hl_status (*hl_code_entry)(hl_ir_exit *out_exit);

HL_API hl_status hl_code_buffer_init(hl_code_buffer *buffer, void *storage, size_t capacity);
HL_API hl_status hl_codegen_block(uint32_t host_isa, const hl_ir_block *block, hl_code_buffer *output);

HL_EXTERN_C_END

#endif
