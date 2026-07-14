#include "aarch64_codegen.h"

#include <stdint.h>

#define HL_AARCH64_VALUE_REGISTERS 15u

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
    /* x0 carries hl_ir_exit * for the complete generated-block call. */
    return value.id;
}

static uint32_t hl_aarch64_spill_offset(hl_ir_value value) {
    return (value.id - HL_AARCH64_VALUE_REGISTERS - 1u) * 8u;
}

static hl_status hl_aarch64_load_value(hl_code_buffer *output, hl_ir_value value, uint32_t scratch,
                                       uint32_t *out_register) {
    if (value.id <= HL_AARCH64_VALUE_REGISTERS) {
        *out_register = hl_aarch64_register(value);
        return HL_STATUS_OK;
    }
    *out_register = scratch;
    return hl_aarch64_emit(output, UINT32_C(0xf94003e0) | (hl_aarch64_spill_offset(value) / 8u) << 10 | scratch);
}

static hl_status hl_aarch64_store_spill(hl_code_buffer *output, hl_ir_value value, uint32_t source) {
    if (value.id <= HL_AARCH64_VALUE_REGISTERS) return HL_STATUS_OK;
    return hl_aarch64_emit(output, UINT32_C(0xf90003e0) | (hl_aarch64_spill_offset(value) / 8u) << 10 | source);
}

static hl_status hl_aarch64_exit(hl_code_buffer *output, const hl_ir_instruction *instruction, uint32_t kind,
                                 uint32_t frame_size) {
    const uint32_t scratch = 17;
    uint32_t value_register;
    hl_status status;
    status = hl_aarch64_constant(output, scratch, kind, 0);
    if (status != HL_STATUS_OK) return status;
    status = hl_aarch64_emit(output, UINT32_C(0xb9000000) | scratch | 0u << 5);
    if (status != HL_STATUS_OK) return status;
    status = hl_aarch64_emit(output, UINT32_C(0xb9000000) | 1u << 10 | 31u | 0u << 5);
    if (status != HL_STATUS_OK) return status;
    if (instruction->operand_count == 0) {
        status = hl_aarch64_constant(output, scratch, instruction->immediate, 1);
        if (status != HL_STATUS_OK) return status;
        status = hl_aarch64_emit(output, UINT32_C(0xf9000000) | 1u << 10 | scratch | 0u << 5);
    } else {
        status = hl_aarch64_load_value(output, instruction->operands[0], scratch, &value_register);
        if (status != HL_STATUS_OK) return status;
        status = hl_aarch64_emit(output, UINT32_C(0xf9000000) | 1u << 10 | value_register);
    }
    if (status != HL_STATUS_OK) return status;
    if (instruction->operand_count < 2) {
        status = hl_aarch64_emit(output, UINT32_C(0xf9000000) | 2u << 10 | 31u | 0u << 5);
    } else {
        status = hl_aarch64_load_value(output, instruction->operands[1], scratch, &value_register);
        if (status != HL_STATUS_OK) return status;
        status = hl_aarch64_emit(output, UINT32_C(0xf9000000) | 2u << 10 | value_register);
    }
    if (status != HL_STATUS_OK) return status;
    status = hl_aarch64_emit(output, UINT32_C(0x52800000));
    if (status != HL_STATUS_OK) return status;
    if (frame_size != 0) {
        status = hl_aarch64_emit(output, UINT32_C(0x910003ff) | frame_size << 10);
        if (status != HL_STATUS_OK) return status;
    }
    return hl_aarch64_emit(output, UINT32_C(0xd65f03c0));
}

static int hl_aarch64_type_supported(uint16_t type) {
    return type == HL_IR_TYPE_NONE || type == HL_IR_TYPE_I32 || type == HL_IR_TYPE_I64 ||
           type == HL_IR_TYPE_GUEST_ADDRESS;
}

hl_status hl_codegen_aarch64(const hl_ir_block *block, hl_code_buffer *output) {
    uint32_t i;
    uint32_t spill_count = block->next_value_id > HL_AARCH64_VALUE_REGISTERS + 1u
                               ? block->next_value_id - HL_AARCH64_VALUE_REGISTERS - 1u
                               : 0;
    uint32_t frame_size = (spill_count * 8u + 15u) & ~15u;
    if (frame_size > 4080u) return HL_STATUS_RESOURCE_LIMIT;
    if (frame_size != 0) {
        hl_status prologue = hl_aarch64_emit(output, UINT32_C(0xd10003ff) | frame_size << 10);
        if (prologue != HL_STATUS_OK) return prologue;
    }
    for (i = 0; i < block->instruction_count; ++i) {
        const hl_ir_instruction *instruction = &block->instructions[i];
        const uint32_t destination = instruction->result.id == 0 ? 0
                                                                 : (instruction->result.id <= HL_AARCH64_VALUE_REGISTERS
                                                                        ? hl_aarch64_register(instruction->result)
                                                                        : 16);
        uint32_t operand_registers[HL_IR_MAX_OPERANDS];
        uint32_t encoding;
        int wide = instruction->result.type != HL_IR_TYPE_I32;
        uint32_t operand;
        hl_status status;
        if (!hl_aarch64_type_supported(instruction->result.type)) return HL_STATUS_NOT_SUPPORTED;
        for (operand = 0; operand < instruction->operand_count; ++operand) {
            if (!hl_aarch64_type_supported(instruction->operands[operand].type)) return HL_STATUS_NOT_SUPPORTED;
            status = hl_aarch64_load_value(output, instruction->operands[operand], 16u + operand,
                                           &operand_registers[operand]);
            if (status != HL_STATUS_OK) return status;
        }
        switch (instruction->opcode) {
        case HL_IR_OP_CONSTANT: status = hl_aarch64_constant(output, destination, instruction->immediate, wide); break;
        case HL_IR_OP_COPY:
            encoding = (wide ? UINT32_C(0xaa0003e0) : UINT32_C(0x2a0003e0)) | operand_registers[0] << 16 | destination;
            status = hl_aarch64_emit(output, encoding);
            break;
        case HL_IR_OP_ADD:
            encoding = (wide ? UINT32_C(0x8b000000) : UINT32_C(0x0b000000)) | operand_registers[1] << 16 |
                       operand_registers[0] << 5 | destination;
            status = hl_aarch64_emit(output, encoding);
            break;
        case HL_IR_OP_SUB:
            encoding = (wide ? UINT32_C(0xcb000000) : UINT32_C(0x4b000000)) | operand_registers[1] << 16 |
                       operand_registers[0] << 5 | destination;
            status = hl_aarch64_emit(output, encoding);
            break;
        case HL_IR_OP_AND:
            encoding = (wide ? UINT32_C(0x8a000000) : UINT32_C(0x0a000000)) | operand_registers[1] << 16 |
                       operand_registers[0] << 5 | destination;
            status = hl_aarch64_emit(output, encoding);
            break;
        case HL_IR_OP_OR:
            encoding = (wide ? UINT32_C(0xaa000000) : UINT32_C(0x2a000000)) | operand_registers[1] << 16 |
                       operand_registers[0] << 5 | destination;
            status = hl_aarch64_emit(output, encoding);
            break;
        case HL_IR_OP_XOR:
            encoding = (wide ? UINT32_C(0xca000000) : UINT32_C(0x4a000000)) | operand_registers[1] << 16 |
                       operand_registers[0] << 5 | destination;
            status = hl_aarch64_emit(output, encoding);
            break;
        case HL_IR_OP_SAFEPOINT: status = HL_STATUS_OK; break;
        case HL_IR_OP_GUEST_RETURN: status = hl_aarch64_exit(output, instruction, HL_IR_EXIT_RETURN, frame_size); break;
        case HL_IR_OP_SYSCALL_EXIT:
            status = hl_aarch64_exit(output, instruction, HL_IR_EXIT_SYSCALL, frame_size);
            break;
        case HL_IR_OP_FAULT_EXIT: status = hl_aarch64_exit(output, instruction, HL_IR_EXIT_FAULT, frame_size); break;
        default: return HL_STATUS_NOT_SUPPORTED;
        }
        if (status != HL_STATUS_OK) return status;
        if (instruction->result.id > HL_AARCH64_VALUE_REGISTERS) {
            status = hl_aarch64_store_spill(output, instruction->result, destination);
            if (status != HL_STATUS_OK) return status;
        }
    }
    return HL_STATUS_OK;
}
