#include "hl/ir.h"

#include <stdlib.h>
#include <string.h>

typedef struct hl_ir_runtime_value {
    uint64_t bits;
    uint16_t type;
} hl_ir_runtime_value;

hl_status hl_ir_interpret(const hl_ir_block *block, hl_ir_exit *out_exit) {
    hl_ir_runtime_value *values;
    uint32_t i;
    hl_status status;
    if (out_exit == NULL) return HL_STATUS_INVALID_ARGUMENT;
    memset(out_exit, 0, sizeof(*out_exit));
    status = hl_ir_validate(block, NULL);
    if (status != HL_STATUS_OK) return status;
    values = calloc(block->next_value_id, sizeof(*values));
    if (values == NULL) return HL_STATUS_OUT_OF_MEMORY;
    for (i = 0; i < block->instruction_count; ++i) {
        const hl_ir_instruction *instruction = &block->instructions[i];
        uint64_t result = 0;
        switch (instruction->opcode) {
        case HL_IR_OP_CONSTANT: result = instruction->immediate; break;
        case HL_IR_OP_COPY: result = values[instruction->operands[0].id].bits; break;
        case HL_IR_OP_ADD:
            result = values[instruction->operands[0].id].bits + values[instruction->operands[1].id].bits;
            break;
        case HL_IR_OP_SUB:
            result = values[instruction->operands[0].id].bits - values[instruction->operands[1].id].bits;
            break;
        case HL_IR_OP_AND:
            result = values[instruction->operands[0].id].bits & values[instruction->operands[1].id].bits;
            break;
        case HL_IR_OP_OR:
            result = values[instruction->operands[0].id].bits | values[instruction->operands[1].id].bits;
            break;
        case HL_IR_OP_XOR:
            result = values[instruction->operands[0].id].bits ^ values[instruction->operands[1].id].bits;
            break;
        case HL_IR_OP_SAFEPOINT: break;
        case HL_IR_OP_GUEST_RETURN:
            out_exit->kind = HL_IR_EXIT_RETURN;
            out_exit->value =
                instruction->operand_count == 0 ? instruction->immediate : values[instruction->operands[0].id].bits;
            free(values);
            return HL_STATUS_OK;
        case HL_IR_OP_SYSCALL_EXIT:
            out_exit->kind = HL_IR_EXIT_SYSCALL;
            out_exit->value = instruction->immediate;
            free(values);
            return HL_STATUS_OK;
        case HL_IR_OP_FAULT_EXIT:
            out_exit->kind = HL_IR_EXIT_FAULT;
            out_exit->value = instruction->immediate;
            free(values);
            return HL_STATUS_OK;
        default: free(values); return HL_STATUS_NOT_SUPPORTED;
        }
        if (instruction->result.type == HL_IR_TYPE_I32) result &= UINT32_MAX;
        if (instruction->result.id != 0) {
            values[instruction->result.id].bits = result;
            values[instruction->result.id].type = instruction->result.type;
        }
    }
    free(values);
    return HL_STATUS_CORRUPT;
}
