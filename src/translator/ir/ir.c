#include "hl/ir.h"

#include <string.h>

int hl_ir_opcode_is_terminator(uint16_t opcode) {
    return opcode == HL_IR_OP_BRANCH || opcode == HL_IR_OP_BRANCH_CONDITIONAL || opcode == HL_IR_OP_GUEST_RETURN ||
           opcode == HL_IR_OP_SYSCALL_EXIT || opcode == HL_IR_OP_FAULT_EXIT;
}

hl_status hl_ir_block_init(hl_ir_block *block, uint64_t guest_pc, hl_ir_instruction *storage, uint32_t capacity) {
    if (block == NULL || storage == NULL || capacity == 0) return HL_STATUS_INVALID_ARGUMENT;
    memset(block, 0, sizeof(*block));
    memset(storage, 0, sizeof(*storage) * capacity);
    block->abi = HL_IR_ABI;
    block->size = sizeof(*block);
    block->guest_pc = guest_pc;
    block->instructions = storage;
    block->instruction_capacity = capacity;
    block->next_value_id = 1;
    return HL_STATUS_OK;
}

hl_status hl_ir_append(hl_ir_block *block, const hl_ir_instruction *instruction, hl_ir_value *result) {
    hl_ir_instruction copy;
    if (block == NULL || instruction == NULL) return HL_STATUS_INVALID_ARGUMENT;
    if (block->abi != HL_IR_ABI || block->size < sizeof(*block)) return HL_STATUS_ABI_MISMATCH;
    if (block->terminated) return HL_STATUS_INVALID_ARGUMENT;
    if (instruction->opcode == HL_IR_OP_INVALID || instruction->opcode >= HL_IR_OP_COUNT ||
        instruction->operand_count > HL_IR_MAX_OPERANDS)
        return HL_STATUS_INVALID_ARGUMENT;
    if (block->instruction_count == block->instruction_capacity) return HL_STATUS_RESOURCE_LIMIT;
    copy = *instruction;
    if (copy.result.type != HL_IR_TYPE_NONE) {
        if (copy.result.type > HL_IR_TYPE_CONDITION) return HL_STATUS_INVALID_ARGUMENT;
        copy.result.id = block->next_value_id++;
    } else {
        copy.result.id = 0;
    }
    block->instructions[block->instruction_count++] = copy;
    block->terminated = (uint32_t)hl_ir_opcode_is_terminator(copy.opcode);
    if (result != NULL) *result = copy.result;
    return HL_STATUS_OK;
}

hl_status hl_ir_validate(const hl_ir_block *block, uint32_t *bad_instruction) {
    uint32_t defined = 0;
    uint32_t i;
    if (bad_instruction != NULL) *bad_instruction = UINT32_MAX;
    if (block == NULL || block->instructions == NULL) return HL_STATUS_INVALID_ARGUMENT;
    if (block->abi != HL_IR_ABI || block->size < sizeof(*block)) return HL_STATUS_ABI_MISMATCH;
    if (block->instruction_count > block->instruction_capacity) return HL_STATUS_CORRUPT;
    for (i = 0; i < block->instruction_count; ++i) {
        const hl_ir_instruction *instruction = &block->instructions[i];
        uint32_t operand;
        if (instruction->opcode == HL_IR_OP_INVALID || instruction->opcode >= HL_IR_OP_COUNT ||
            instruction->operand_count > HL_IR_MAX_OPERANDS ||
            (hl_ir_opcode_is_terminator(instruction->opcode) && i + 1 != block->instruction_count))
            goto corrupt;
        for (operand = 0; operand < instruction->operand_count; ++operand) {
            const hl_ir_value value = instruction->operands[operand];
            if (value.id == 0 || value.id > defined || value.type == HL_IR_TYPE_NONE ||
                value.type > HL_IR_TYPE_CONDITION)
                goto corrupt;
        }
        if (instruction->result.type != HL_IR_TYPE_NONE) {
            if (instruction->result.id != defined + 1) goto corrupt;
            defined = instruction->result.id;
        } else if (instruction->result.id != 0) {
            goto corrupt;
        }
    }
    if (block->instruction_count != 0 &&
        !hl_ir_opcode_is_terminator(block->instructions[block->instruction_count - 1].opcode))
        return HL_STATUS_CORRUPT;
    return HL_STATUS_OK;

corrupt:
    if (bad_instruction != NULL) *bad_instruction = i;
    return HL_STATUS_CORRUPT;
}
