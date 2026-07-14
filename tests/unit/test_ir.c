#include "test.h"

#include "hl/ir.h"

#include <string.h>

static hl_ir_instruction instruction(uint16_t opcode, uint16_t result_type) {
    hl_ir_instruction value;
    memset(&value, 0, sizeof(value));
    value.opcode = opcode;
    value.result.type = result_type;
    return value;
}

static int check_cfg(void) {
    hl_ir_function function;
    hl_ir_block blocks[4], *entry, *loop, *body, *done;
    hl_ir_instruction code[4][8], current;
    hl_ir_value address, one, limit, loaded, condition, incremented;
    hl_ir_execution execution;
    uint8_t memory[8] = {0};
    uint32_t bad_block, bad_instruction;
    HL_CHECK(hl_ir_function_init(&function, 10, blocks, HL_ARRAY_COUNT(blocks)) == HL_STATUS_OK);
    HL_CHECK(hl_ir_function_add_block(&function, 10, 0x1000, code[0], HL_ARRAY_COUNT(code[0]), &entry) == HL_STATUS_OK);
    HL_CHECK(hl_ir_function_add_block(&function, 20, 0x1004, code[1], HL_ARRAY_COUNT(code[1]), &loop) == HL_STATUS_OK);
    HL_CHECK(hl_ir_function_add_block(&function, 30, 0x1008, code[2], HL_ARRAY_COUNT(code[2]), &body) == HL_STATUS_OK);
    HL_CHECK(hl_ir_function_add_block(&function, 40, 0x100c, code[3], HL_ARRAY_COUNT(code[3]), &done) == HL_STATUS_OK);
    current = instruction(HL_IR_OP_CONSTANT, HL_IR_TYPE_GUEST_ADDRESS);
    current.immediate = 0;
    HL_CHECK(hl_ir_function_append(&function, entry, &current, &address) == HL_STATUS_OK);
    current = instruction(HL_IR_OP_CONSTANT, HL_IR_TYPE_I32);
    current.immediate = 1;
    HL_CHECK(hl_ir_function_append(&function, entry, &current, &one) == HL_STATUS_OK);
    current.immediate = 5;
    HL_CHECK(hl_ir_function_append(&function, entry, &current, &limit) == HL_STATUS_OK);
    current = instruction(HL_IR_OP_BRANCH, HL_IR_TYPE_NONE);
    current.true_label = 20;
    HL_CHECK(hl_ir_function_append(&function, entry, &current, NULL) == HL_STATUS_OK);
    current = instruction(HL_IR_OP_LOAD, HL_IR_TYPE_I32);
    current.operand_count = 1;
    current.operands[0] = address;
    HL_CHECK(hl_ir_function_append(&function, loop, &current, &loaded) == HL_STATUS_OK);
    current = instruction(HL_IR_OP_COMPARE, HL_IR_TYPE_CONDITION);
    current.flags = HL_IR_CONDITION_UNSIGNED_LT;
    current.operand_count = 2;
    current.operands[0] = loaded;
    current.operands[1] = limit;
    HL_CHECK(hl_ir_function_append(&function, loop, &current, &condition) == HL_STATUS_OK);
    current = instruction(HL_IR_OP_BRANCH_CONDITIONAL, HL_IR_TYPE_NONE);
    current.operand_count = 1;
    current.operands[0] = condition;
    current.true_label = 30;
    current.false_label = 40;
    HL_CHECK(hl_ir_function_append(&function, loop, &current, NULL) == HL_STATUS_OK);
    current = instruction(HL_IR_OP_ADD, HL_IR_TYPE_I32);
    current.operand_count = 2;
    current.operands[0] = loaded;
    current.operands[1] = one;
    HL_CHECK(hl_ir_function_append(&function, body, &current, &incremented) == HL_STATUS_OK);
    current = instruction(HL_IR_OP_STORE, HL_IR_TYPE_NONE);
    current.operand_count = 2;
    current.operands[0] = address;
    current.operands[1] = incremented;
    HL_CHECK(hl_ir_function_append(&function, body, &current, NULL) == HL_STATUS_OK);
    current = instruction(HL_IR_OP_BRANCH, HL_IR_TYPE_NONE);
    current.true_label = 20;
    HL_CHECK(hl_ir_function_append(&function, body, &current, NULL) == HL_STATUS_OK);
    current = instruction(HL_IR_OP_GUEST_RETURN, HL_IR_TYPE_NONE);
    current.operand_count = 1;
    current.operands[0] = loaded;
    HL_CHECK(hl_ir_function_append(&function, done, &current, NULL) == HL_STATUS_OK);
    HL_CHECK(hl_ir_function_validate(&function, &bad_block, &bad_instruction) == HL_STATUS_OK);
    memset(&execution, 0, sizeof(execution));
    execution.abi = HL_IR_EXECUTION_ABI;
    execution.size = sizeof(execution);
    execution.memory = memory;
    execution.memory_size = sizeof(memory);
    HL_CHECK(hl_ir_function_interpret(&function, &execution) == HL_STATUS_OK);
    HL_CHECK(execution.exit.kind == HL_IR_EXIT_RETURN && execution.exit.value == 5 && memory[0] == 5);

    /* Missing targets and non-dominating SSA uses are rejected deterministically. */
    function.entry_label = 99;
    HL_CHECK(hl_ir_function_validate(&function, &bad_block, &bad_instruction) == HL_STATUS_CORRUPT);
    function.entry_label = 10;
    entry->instructions[3].true_label = 99;
    HL_CHECK(hl_ir_function_validate(&function, &bad_block, &bad_instruction) == HL_STATUS_CORRUPT && bad_block == 0 &&
             bad_instruction == 3);
    entry->instructions[3].true_label = 20;
    loop->instructions[1].flags = 99;
    HL_CHECK(hl_ir_function_validate(&function, &bad_block, &bad_instruction) == HL_STATUS_CORRUPT && bad_block == 1 &&
             bad_instruction == 1);
    loop->instructions[1].flags = HL_IR_CONDITION_UNSIGNED_LT;
    done->instruction_count = 0;
    HL_CHECK(hl_ir_function_validate(&function, &bad_block, &bad_instruction) == HL_STATUS_CORRUPT);
    done->instruction_count = 1;
    done->label = 30;
    HL_CHECK(hl_ir_function_validate(&function, &bad_block, &bad_instruction) == HL_STATUS_CORRUPT);
    done->label = 40;
    done->instructions[0].operands[0] = incremented;
    HL_CHECK(hl_ir_function_validate(&function, &bad_block, &bad_instruction) == HL_STATUS_CORRUPT && bad_block == 3);
    return EXIT_SUCCESS;
}

int main(void) {
    hl_ir_instruction storage[8];
    hl_ir_block block;
    hl_ir_instruction constant;
    hl_ir_instruction add;
    hl_ir_instruction exit_instruction;
    hl_ir_value first;
    hl_ir_value second;
    hl_ir_value sum;
    hl_ir_execution execution;
    hl_ir_exit *ir_exit = &execution.exit;
    uint32_t bad;
    static const uint16_t reserved_opcodes[] = {HL_IR_OP_BRANCH, HL_IR_OP_BRANCH_CONDITIONAL, HL_IR_OP_GUEST_CALL};

    HL_CHECK(hl_ir_block_init(&block, UINT64_C(0x400000), storage, HL_ARRAY_COUNT(storage)) == HL_STATUS_OK);
    constant = instruction(HL_IR_OP_CONSTANT, HL_IR_TYPE_I64);
    constant.immediate = 40;
    HL_CHECK(hl_ir_append(&block, &constant, &first) == HL_STATUS_OK);
    constant.immediate = 2;
    HL_CHECK(hl_ir_append(&block, &constant, &second) == HL_STATUS_OK);

    add = instruction(HL_IR_OP_ADD, HL_IR_TYPE_I64);
    add.operand_count = 2;
    add.operands[0] = first;
    add.operands[1] = second;
    HL_CHECK(hl_ir_append(&block, &add, &sum) == HL_STATUS_OK);

    exit_instruction = instruction(HL_IR_OP_GUEST_RETURN, HL_IR_TYPE_NONE);
    exit_instruction.operand_count = 1;
    exit_instruction.operands[0] = sum;
    HL_CHECK(hl_ir_append(&block, &exit_instruction, NULL) == HL_STATUS_OK);
    HL_CHECK(hl_ir_validate(&block, &bad) == HL_STATUS_OK);
    memset(&execution, 0, sizeof(execution));
    execution.abi = HL_IR_EXECUTION_ABI;
    execution.size = sizeof(execution);
    HL_CHECK(hl_ir_interpret(&block, &execution) == HL_STATUS_OK);
    HL_CHECK(ir_exit->kind == HL_IR_EXIT_RETURN && ir_exit->value == 42);
    HL_CHECK(hl_ir_append(&block, &constant, NULL) == HL_STATUS_INVALID_ARGUMENT);

    storage[2].operands[1].id = 99;
    HL_CHECK(hl_ir_validate(&block, &bad) == HL_STATUS_CORRUPT && bad == 2);

    /* I32 arithmetic wraps at 32 bits independently of the host integer width. */
    HL_CHECK(hl_ir_block_init(&block, UINT64_C(0x400100), storage, HL_ARRAY_COUNT(storage)) == HL_STATUS_OK);
    constant = instruction(HL_IR_OP_CONSTANT, HL_IR_TYPE_I32);
    constant.immediate = UINT64_C(0xffffffff);
    HL_CHECK(hl_ir_append(&block, &constant, &first) == HL_STATUS_OK);
    constant.immediate = 2;
    HL_CHECK(hl_ir_append(&block, &constant, &second) == HL_STATUS_OK);
    add = instruction(HL_IR_OP_ADD, HL_IR_TYPE_I32);
    add.operand_count = 2;
    add.operands[0] = first;
    add.operands[1] = second;
    HL_CHECK(hl_ir_append(&block, &add, &sum) == HL_STATUS_OK);
    exit_instruction = instruction(HL_IR_OP_GUEST_RETURN, HL_IR_TYPE_NONE);
    exit_instruction.operand_count = 1;
    exit_instruction.operands[0] = sum;
    HL_CHECK(hl_ir_append(&block, &exit_instruction, NULL) == HL_STATUS_OK);
    HL_CHECK(hl_ir_interpret(&block, &execution) == HL_STATUS_OK);
    HL_CHECK(ir_exit->kind == HL_IR_EXIT_RETURN && ir_exit->value == 1);

    /* Exit payload semantics are identical for dispatch and fault boundaries. */
    for (uint16_t opcode = HL_IR_OP_SYSCALL_EXIT; opcode <= HL_IR_OP_FAULT_EXIT; ++opcode) {
        HL_CHECK(hl_ir_block_init(&block, UINT64_C(0x400180), storage, HL_ARRAY_COUNT(storage)) == HL_STATUS_OK);
        constant = instruction(HL_IR_OP_CONSTANT, HL_IR_TYPE_I64);
        constant.immediate = UINT64_C(0x1122334455667788);
        HL_CHECK(hl_ir_append(&block, &constant, &first) == HL_STATUS_OK);
        constant.immediate = UINT64_C(0x8877665544332211);
        HL_CHECK(hl_ir_append(&block, &constant, &second) == HL_STATUS_OK);
        exit_instruction = instruction(opcode, HL_IR_TYPE_NONE);
        exit_instruction.operand_count = 2;
        exit_instruction.operands[0] = first;
        exit_instruction.operands[1] = second;
        HL_CHECK(hl_ir_append(&block, &exit_instruction, NULL) == HL_STATUS_OK);
        memset(ir_exit, 0xa5, sizeof(*ir_exit));
        HL_CHECK(hl_ir_interpret(&block, &execution) == HL_STATUS_OK);
        HL_CHECK(ir_exit->kind == (opcode == HL_IR_OP_SYSCALL_EXIT ? HL_IR_EXIT_SYSCALL : HL_IR_EXIT_FAULT));
        HL_CHECK(ir_exit->reserved == 0 && ir_exit->value == UINT64_C(0x1122334455667788) &&
                 ir_exit->detail == UINT64_C(0x8877665544332211));
    }

    /* CFG terminators require a function container; GUEST_CALL has no contract yet. */
    for (size_t i = 0; i < HL_ARRAY_COUNT(reserved_opcodes); ++i) {
        hl_ir_instruction reserved = instruction(reserved_opcodes[i], HL_IR_TYPE_NONE);
        HL_CHECK(hl_ir_block_init(&block, UINT64_C(0x400200), storage, HL_ARRAY_COUNT(storage)) == HL_STATUS_OK);
        HL_CHECK(hl_ir_append(&block, &reserved, NULL) == HL_STATUS_NOT_SUPPORTED);

        storage[0] = reserved;
        block.instruction_count = 1;
        block.terminated = (uint32_t)hl_ir_opcode_is_terminator(reserved.opcode);
        HL_CHECK(hl_ir_validate(&block, &bad) == HL_STATUS_CORRUPT && bad == 0);
    }
    HL_CHECK(check_cfg() == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}
