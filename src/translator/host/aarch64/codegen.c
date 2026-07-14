#include "aarch64_codegen.h"

#include <stdint.h>

#define HL_AARCH64_VALUE_REGISTERS 16u

static hl_status hl_aarch64_emit(hl_code_buffer *output, uint32_t instruction) {
    if (output->code_size > output->capacity || output->capacity - output->code_size < 4)
        return HL_STATUS_RESOURCE_LIMIT;
    output->data[output->code_size + 0] = (uint8_t)instruction;
    output->data[output->code_size + 1] = (uint8_t)(instruction >> 8);
    output->data[output->code_size + 2] = (uint8_t)(instruction >> 16);
    output->data[output->code_size + 3] = (uint8_t)(instruction >> 24);
    output->code_size += 4;
    return HL_STATUS_OK;
}

static hl_status hl_aarch64_constant(hl_code_buffer *output, uint32_t destination, uint64_t value, int wide) {
    uint32_t half;
    hl_status status;
    status = hl_aarch64_emit(output, (wide ? UINT32_C(0xd2800000) : UINT32_C(0x52800000)) |
                                         ((uint32_t)value & UINT32_C(0xffff)) << 5 | destination);
    if (status != HL_STATUS_OK) return status;
    for (half = 1; half < (wide ? 4u : 2u); ++half) {
        uint32_t immediate = (uint32_t)(value >> (half * 16u)) & UINT32_C(0xffff);
        if (immediate != 0) {
            status = hl_aarch64_emit(output, (wide ? UINT32_C(0xf2800000) : UINT32_C(0x72800000)) | half << 21 |
                                                 immediate << 5 | destination);
            if (status != HL_STATUS_OK) return status;
        }
    }
    return HL_STATUS_OK;
}

static uint32_t hl_aarch64_register(hl_ir_value value) {
    return value.id - 1u;
}

static int hl_aarch64_type_supported(uint16_t type) {
    return type == HL_IR_TYPE_NONE || type == HL_IR_TYPE_I32 || type == HL_IR_TYPE_I64 ||
           type == HL_IR_TYPE_GUEST_ADDRESS;
}

hl_status hl_codegen_aarch64(const hl_ir_block *block, hl_code_buffer *output) {
    uint32_t i;
    for (i = 0; i < block->instruction_count; ++i) {
        const hl_ir_instruction *instruction = &block->instructions[i];
        const uint32_t destination = instruction->result.id == 0 ? 0 : hl_aarch64_register(instruction->result);
        uint32_t encoding;
        int wide = instruction->result.type != HL_IR_TYPE_I32;
        uint32_t operand;
        hl_status status;
        if (!hl_aarch64_type_supported(instruction->result.type)) return HL_STATUS_NOT_SUPPORTED;
        if (instruction->result.id > HL_AARCH64_VALUE_REGISTERS) return HL_STATUS_RESOURCE_LIMIT;
        for (operand = 0; operand < instruction->operand_count; ++operand) {
            if (!hl_aarch64_type_supported(instruction->operands[operand].type)) return HL_STATUS_NOT_SUPPORTED;
            if (instruction->operands[operand].id > HL_AARCH64_VALUE_REGISTERS) return HL_STATUS_RESOURCE_LIMIT;
        }
        switch (instruction->opcode) {
        case HL_IR_OP_CONSTANT: status = hl_aarch64_constant(output, destination, instruction->immediate, wide); break;
        case HL_IR_OP_COPY:
            encoding = (wide ? UINT32_C(0xaa0003e0) : UINT32_C(0x2a0003e0)) |
                       hl_aarch64_register(instruction->operands[0]) << 16 | destination;
            status = hl_aarch64_emit(output, encoding);
            break;
        case HL_IR_OP_ADD:
            encoding = (wide ? UINT32_C(0x8b000000) : UINT32_C(0x0b000000)) |
                       hl_aarch64_register(instruction->operands[1]) << 16 |
                       hl_aarch64_register(instruction->operands[0]) << 5 | destination;
            status = hl_aarch64_emit(output, encoding);
            break;
        case HL_IR_OP_SUB:
            encoding = (wide ? UINT32_C(0xcb000000) : UINT32_C(0x4b000000)) |
                       hl_aarch64_register(instruction->operands[1]) << 16 |
                       hl_aarch64_register(instruction->operands[0]) << 5 | destination;
            status = hl_aarch64_emit(output, encoding);
            break;
        case HL_IR_OP_AND:
            encoding = (wide ? UINT32_C(0x8a000000) : UINT32_C(0x0a000000)) |
                       hl_aarch64_register(instruction->operands[1]) << 16 |
                       hl_aarch64_register(instruction->operands[0]) << 5 | destination;
            status = hl_aarch64_emit(output, encoding);
            break;
        case HL_IR_OP_OR:
            encoding = (wide ? UINT32_C(0xaa000000) : UINT32_C(0x2a000000)) |
                       hl_aarch64_register(instruction->operands[1]) << 16 |
                       hl_aarch64_register(instruction->operands[0]) << 5 | destination;
            status = hl_aarch64_emit(output, encoding);
            break;
        case HL_IR_OP_XOR:
            encoding = (wide ? UINT32_C(0xca000000) : UINT32_C(0x4a000000)) |
                       hl_aarch64_register(instruction->operands[1]) << 16 |
                       hl_aarch64_register(instruction->operands[0]) << 5 | destination;
            status = hl_aarch64_emit(output, encoding);
            break;
        case HL_IR_OP_SAFEPOINT: status = HL_STATUS_OK; break;
        case HL_IR_OP_GUEST_RETURN:
            if (instruction->operand_count == 1 && hl_aarch64_register(instruction->operands[0]) != 0) {
                encoding =
                    (instruction->operands[0].type == HL_IR_TYPE_I32 ? UINT32_C(0x2a0003e0) : UINT32_C(0xaa0003e0)) |
                    hl_aarch64_register(instruction->operands[0]) << 16;
                status = hl_aarch64_emit(output, encoding);
                if (status != HL_STATUS_OK) return status;
            } else if (instruction->operand_count == 0) {
                status = hl_aarch64_constant(output, 0, instruction->immediate, 1);
                if (status != HL_STATUS_OK) return status;
            }
            status = hl_aarch64_emit(output, UINT32_C(0xd65f03c0));
            break;
        default: return HL_STATUS_NOT_SUPPORTED;
        }
        if (status != HL_STATUS_OK) return status;
    }
    return HL_STATUS_OK;
}
