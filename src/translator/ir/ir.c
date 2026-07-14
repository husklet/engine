#include "hl/ir.h"

#include <stdlib.h>
#include <string.h>

int hl_ir_opcode_is_terminator(uint16_t opcode) {
    return opcode == HL_IR_OP_BRANCH || opcode == HL_IR_OP_BRANCH_CONDITIONAL || opcode == HL_IR_OP_GUEST_RETURN ||
           opcode == HL_IR_OP_SYSCALL_EXIT || opcode == HL_IR_OP_FAULT_EXIT;
}

static int hl_ir_is_integer(uint16_t type) {
    return type == HL_IR_TYPE_I32 || type == HL_IR_TYPE_I64 || type == HL_IR_TYPE_GUEST_ADDRESS;
}

static int hl_ir_condition_valid(uint16_t condition) {
    return condition >= HL_IR_CONDITION_EQ && condition <= HL_IR_CONDITION_UNSIGNED_GE;
}

static int hl_ir_signature_valid(const hl_ir_instruction *instruction) {
    if ((instruction->opcode != HL_IR_OP_COMPARE && instruction->flags != 0) ||
        (instruction->opcode != HL_IR_OP_BRANCH && instruction->opcode != HL_IR_OP_BRANCH_CONDITIONAL &&
         (instruction->true_label != 0 || instruction->false_label != 0)))
        return 0;
    switch (instruction->opcode) {
    case HL_IR_OP_CONSTANT: return instruction->operand_count == 0 && instruction->result.type != HL_IR_TYPE_NONE;
    case HL_IR_OP_COPY:
        return instruction->operand_count == 1 && instruction->result.type == instruction->operands[0].type;
    case HL_IR_OP_ADD:
    case HL_IR_OP_SUB:
    case HL_IR_OP_AND:
    case HL_IR_OP_OR:
    case HL_IR_OP_XOR:
        return instruction->operand_count == 2 && hl_ir_is_integer(instruction->result.type) &&
               instruction->operands[0].type == instruction->result.type &&
               instruction->operands[1].type == instruction->result.type;
    case HL_IR_OP_SAFEPOINT: return instruction->operand_count == 0 && instruction->result.type == HL_IR_TYPE_NONE;
    case HL_IR_OP_LOAD:
        return instruction->operand_count == 1 && hl_ir_is_integer(instruction->result.type) &&
               hl_ir_is_integer(instruction->operands[0].type);
    case HL_IR_OP_STORE:
        return instruction->operand_count == 2 && instruction->result.type == HL_IR_TYPE_NONE &&
               hl_ir_is_integer(instruction->operands[0].type) && hl_ir_is_integer(instruction->operands[1].type);
    case HL_IR_OP_GUEST_RETURN:
        return instruction->operand_count <= 1 && instruction->result.type == HL_IR_TYPE_NONE &&
               (instruction->operand_count == 0 || hl_ir_is_integer(instruction->operands[0].type));
    case HL_IR_OP_SYSCALL_EXIT:
    case HL_IR_OP_FAULT_EXIT:
        return instruction->operand_count <= 2 && instruction->result.type == HL_IR_TYPE_NONE &&
               (instruction->operand_count == 0 || hl_ir_is_integer(instruction->operands[0].type)) &&
               (instruction->operand_count < 2 || hl_ir_is_integer(instruction->operands[1].type));
    case HL_IR_OP_COMPARE:
        return instruction->operand_count == 2 && instruction->result.type == HL_IR_TYPE_CONDITION &&
               hl_ir_is_integer(instruction->operands[0].type) &&
               instruction->operands[0].type == instruction->operands[1].type &&
               hl_ir_condition_valid(instruction->flags) && instruction->true_label == 0 &&
               instruction->false_label == 0;
    case HL_IR_OP_BRANCH:
        return instruction->operand_count == 0 && instruction->result.type == HL_IR_TYPE_NONE &&
               instruction->true_label != 0 && instruction->false_label == 0 && instruction->immediate == 0;
    case HL_IR_OP_BRANCH_CONDITIONAL:
        return instruction->operand_count == 1 && instruction->operands[0].type == HL_IR_TYPE_CONDITION &&
               instruction->result.type == HL_IR_TYPE_NONE && instruction->true_label != 0 &&
               instruction->false_label != 0 && instruction->immediate == 0;
    case HL_IR_OP_GUEST_CALL: return 0;
    default: return 0;
    }
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
    if (instruction->opcode == HL_IR_OP_GUEST_CALL ||
        (block->label == 0 &&
         (instruction->opcode == HL_IR_OP_BRANCH || instruction->opcode == HL_IR_OP_BRANCH_CONDITIONAL)))
        return HL_STATUS_NOT_SUPPORTED;
    if (!hl_ir_signature_valid(instruction)) return HL_STATUS_INVALID_ARGUMENT;
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

hl_status hl_ir_function_init(hl_ir_function *function, uint32_t entry_label, hl_ir_block *storage, uint32_t capacity) {
    if (function == NULL || entry_label == 0 || storage == NULL || capacity == 0) return HL_STATUS_INVALID_ARGUMENT;
    memset(function, 0, sizeof(*function));
    memset(storage, 0, sizeof(*storage) * capacity);
    function->abi = HL_IR_ABI;
    function->size = sizeof(*function);
    function->blocks = storage;
    function->block_capacity = capacity;
    function->next_value_id = 1;
    function->entry_label = entry_label;
    return HL_STATUS_OK;
}

hl_status hl_ir_function_add_block(hl_ir_function *function, uint32_t label, uint64_t guest_pc,
                                   hl_ir_instruction *storage, uint32_t capacity, hl_ir_block **out_block) {
    uint32_t i;
    hl_ir_block *block;
    hl_status status;
    if (function == NULL || function->abi != HL_IR_ABI || function->size < sizeof(*function))
        return function == NULL ? HL_STATUS_INVALID_ARGUMENT : HL_STATUS_ABI_MISMATCH;
    if (label == 0) return HL_STATUS_INVALID_ARGUMENT;
    if (function->block_count == function->block_capacity) return HL_STATUS_RESOURCE_LIMIT;
    for (i = 0; i < function->block_count; ++i)
        if (function->blocks[i].label == label) return HL_STATUS_INVALID_ARGUMENT;
    block = &function->blocks[function->block_count];
    status = hl_ir_block_init(block, guest_pc, storage, capacity);
    if (status != HL_STATUS_OK) return status;
    block->label = label;
    block->next_value_id = function->next_value_id;
    function->block_count++;
    if (out_block != NULL) *out_block = block;
    return HL_STATUS_OK;
}

hl_status hl_ir_function_append(hl_ir_function *function, hl_ir_block *block, const hl_ir_instruction *instruction,
                                hl_ir_value *result) {
    uint32_t block_index;
    hl_status status;
    if (function == NULL || block == NULL) return HL_STATUS_INVALID_ARGUMENT;
    if (function->abi != HL_IR_ABI || function->size < sizeof(*function)) return HL_STATUS_ABI_MISMATCH;
    if (function->blocks == NULL || function->block_count > function->block_capacity) return HL_STATUS_CORRUPT;
    for (block_index = 0; block_index < function->block_count; ++block_index)
        if (block == &function->blocks[block_index]) break;
    if (block_index == function->block_count) return HL_STATUS_INVALID_ARGUMENT;
    block->next_value_id = function->next_value_id;
    status = hl_ir_append(block, instruction, result);
    function->next_value_id = block->next_value_id;
    return status;
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
            (hl_ir_opcode_is_terminator(instruction->opcode) && i + 1 != block->instruction_count) ||
            !hl_ir_signature_valid(instruction))
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

static int hl_ir_label_index(const hl_ir_function *function, uint32_t label) {
    uint32_t i;
    for (i = 0; i < function->block_count; ++i)
        if (function->blocks[i].label == label) return (int)i;
    return -1;
}

hl_status hl_ir_function_validate(const hl_ir_function *function, uint32_t *bad_block, uint32_t *bad_instruction) {
    uint32_t *definition_block = NULL, *definition_instruction = NULL;
    uint8_t *reachable = NULL, *dominators = NULL;
    uint32_t block_index, entry_index, max_value, changed;
    hl_status result = HL_STATUS_CORRUPT;
    if (bad_block != NULL) *bad_block = UINT32_MAX;
    if (bad_instruction != NULL) *bad_instruction = UINT32_MAX;
    if (function == NULL || function->blocks == NULL || function->block_count == 0) return HL_STATUS_INVALID_ARGUMENT;
    if (function->abi != HL_IR_ABI || function->size < sizeof(*function)) return HL_STATUS_ABI_MISMATCH;
    if (function->block_count > function->block_capacity || function->next_value_id == 0) return HL_STATUS_CORRUPT;
    if (function->entry_label == 0) return HL_STATUS_CORRUPT;
    max_value = function->next_value_id;
    if ((size_t)function->block_count > SIZE_MAX / function->block_count) return HL_STATUS_RESOURCE_LIMIT;
    definition_block = malloc(sizeof(*definition_block) * max_value);
    definition_instruction = malloc(sizeof(*definition_instruction) * max_value);
    reachable = calloc(function->block_count, 1);
    dominators = malloc((size_t)function->block_count * function->block_count);
    if (definition_block == NULL || definition_instruction == NULL || reachable == NULL || dominators == NULL) {
        result = HL_STATUS_OUT_OF_MEMORY;
        goto done;
    }
    for (block_index = 0; block_index < max_value; ++block_index)
        definition_block[block_index] = UINT32_MAX;
    for (block_index = 0; block_index < function->block_count; ++block_index) {
        const hl_ir_block *block = &function->blocks[block_index];
        uint32_t i, other;
        if (block->abi != HL_IR_ABI || block->size < sizeof(*block) || block->label == 0 ||
            block->instructions == NULL || block->instruction_count > block->instruction_capacity)
            goto malformed_block;
        for (other = 0; other < block_index; ++other)
            if (function->blocks[other].label == block->label) goto malformed_block;
        for (i = 0; i < block->instruction_count; ++i) {
            const hl_ir_instruction *instruction = &block->instructions[i];
            uint32_t operand;
            if (!hl_ir_signature_valid(instruction) ||
                (hl_ir_opcode_is_terminator(instruction->opcode) && i + 1 != block->instruction_count))
                goto malformed_instruction;
            if (instruction->opcode == HL_IR_OP_BRANCH || instruction->opcode == HL_IR_OP_BRANCH_CONDITIONAL) {
                if (hl_ir_label_index(function, instruction->true_label) < 0 ||
                    (instruction->opcode == HL_IR_OP_BRANCH_CONDITIONAL &&
                     hl_ir_label_index(function, instruction->false_label) < 0))
                    goto malformed_instruction;
            }
            for (operand = 0; operand < instruction->operand_count; ++operand) {
                hl_ir_value value = instruction->operands[operand];
                if (value.id == 0 || value.id >= max_value || value.type == HL_IR_TYPE_NONE ||
                    value.type > HL_IR_TYPE_CONDITION)
                    goto malformed_instruction;
            }
            if (instruction->result.type != HL_IR_TYPE_NONE) {
                uint32_t id = instruction->result.id;
                if (id == 0 || id >= max_value || definition_block[id] != UINT32_MAX) goto malformed_instruction;
                definition_block[id] = block_index;
                definition_instruction[id] = i;
            } else if (instruction->result.id != 0) {
                goto malformed_instruction;
            }
        }
        continue;
    malformed_instruction:
        if (bad_instruction != NULL) *bad_instruction = i;
    malformed_block:
        if (bad_block != NULL) *bad_block = block_index;
        goto done;
    }
    /* Mark the explicitly selected entry's reachable subgraph. */
    entry_index = (uint32_t)hl_ir_label_index(function, function->entry_label);
    if (entry_index >= function->block_count) goto done;
    reachable[entry_index] = 1;
    do {
        changed = 0;
        for (block_index = 0; block_index < function->block_count; ++block_index) {
            const hl_ir_block *block = &function->blocks[block_index];
            const hl_ir_instruction *term;
            int target;
            if (!reachable[block_index] || block->instruction_count == 0) continue;
            term = &block->instructions[block->instruction_count - 1];
            if (term->opcode != HL_IR_OP_BRANCH && term->opcode != HL_IR_OP_BRANCH_CONDITIONAL) continue;
            target = hl_ir_label_index(function, term->true_label);
            if (!reachable[target]) {
                reachable[target] = 1;
                changed = 1;
            }
            if (term->opcode == HL_IR_OP_BRANCH_CONDITIONAL) {
                target = hl_ir_label_index(function, term->false_label);
                if (!reachable[target]) {
                    reachable[target] = 1;
                    changed = 1;
                }
            }
        }
    } while (changed);
    for (block_index = 0; block_index < function->block_count; ++block_index) {
        const hl_ir_block *block = &function->blocks[block_index];
        if (reachable[block_index] &&
            (block->instruction_count == 0 ||
             !hl_ir_opcode_is_terminator(block->instructions[block->instruction_count - 1].opcode))) {
            if (bad_block != NULL) *bad_block = block_index;
            if (bad_instruction != NULL) *bad_instruction = block->instruction_count;
            goto done;
        }
    }
    /* Classic iterative dominators over reachable blocks. */
    for (block_index = 0; block_index < function->block_count; ++block_index) {
        uint32_t d;
        for (d = 0; d < function->block_count; ++d)
            dominators[block_index * function->block_count + d] =
                (uint8_t)(reachable[block_index] && (block_index == entry_index ? d == entry_index : reachable[d]));
    }
    do {
        changed = 0;
        for (block_index = 0; block_index < function->block_count; ++block_index) {
            uint32_t pred, d;
            uint8_t have_pred = 0;
            if (!reachable[block_index] || block_index == entry_index) continue;
            for (d = 0; d < function->block_count; ++d) {
                uint8_t value = 1;
                for (pred = 0; pred < function->block_count; ++pred) {
                    const hl_ir_block *pb = &function->blocks[pred];
                    const hl_ir_instruction *term;
                    int is_pred;
                    if (!reachable[pred] || pb->instruction_count == 0) continue;
                    term = &pb->instructions[pb->instruction_count - 1];
                    is_pred = (term->opcode == HL_IR_OP_BRANCH || term->opcode == HL_IR_OP_BRANCH_CONDITIONAL) &&
                              (term->true_label == function->blocks[block_index].label ||
                               (term->opcode == HL_IR_OP_BRANCH_CONDITIONAL &&
                                term->false_label == function->blocks[block_index].label));
                    if (is_pred) {
                        have_pred = 1;
                        value &= dominators[pred * function->block_count + d];
                    }
                }
                if (d == block_index) value = 1;
                if (have_pred && dominators[block_index * function->block_count + d] != value) {
                    dominators[block_index * function->block_count + d] = value;
                    changed = 1;
                }
            }
        }
    } while (changed);
    for (block_index = 0; block_index < function->block_count; ++block_index) {
        const hl_ir_block *block = &function->blocks[block_index];
        uint32_t i;
        if (!reachable[block_index]) continue;
        for (i = 0; i < block->instruction_count; ++i) {
            const hl_ir_instruction *instruction = &block->instructions[i];
            uint32_t operand;
            for (operand = 0; operand < instruction->operand_count; ++operand) {
                uint32_t id = instruction->operands[operand].id;
                uint32_t def = definition_block[id];
                if (def == UINT32_MAX ||
                    instruction->operands[operand].type !=
                        function->blocks[def].instructions[definition_instruction[id]].result.type ||
                    !dominators[block_index * function->block_count + def] ||
                    (def == block_index && definition_instruction[id] >= i)) {
                    if (bad_block != NULL) *bad_block = block_index;
                    if (bad_instruction != NULL) *bad_instruction = i;
                    goto done;
                }
            }
        }
    }
    result = HL_STATUS_OK;
done:
    free(dominators);
    free(reachable);
    free(definition_instruction);
    free(definition_block);
    return result;
}
