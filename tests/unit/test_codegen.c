#include "test.h"

#include "hl/codegen.h"

#include <string.h>

static hl_ir_instruction instruction(uint16_t opcode, uint16_t result_type) {
    hl_ir_instruction value;
    memset(&value, 0, sizeof(value));
    value.opcode = opcode;
    value.result.type = result_type;
    return value;
}

static uint32_t word(const uint8_t *data) {
    return (uint32_t)data[0] | (uint32_t)data[1] << 8 | (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
}

int main(void) {
    hl_ir_instruction instructions[8];
    hl_ir_block block;
    hl_ir_instruction current;
    hl_ir_value forty;
    hl_ir_value two;
    hl_ir_value sum;
    uint8_t storage[64];
    hl_code_buffer code;
    static const uint8_t expected_x86_64[] = {
        0x48, 0xb8, 0x28, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* movabs rax, 40 */
        0x48, 0xb9, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* movabs rcx, 2 */
        0x48, 0x89, 0xc2,                                           /* mov rdx, rax */
        0x48, 0x01, 0xca,                                           /* add rdx, rcx */
        0x48, 0x89, 0xd0,                                           /* mov rax, rdx */
        0xc3                                                        /* ret */
    };

    HL_CHECK(hl_ir_block_init(&block, 0x400000, instructions, HL_ARRAY_COUNT(instructions)) == HL_STATUS_OK);
    current = instruction(HL_IR_OP_CONSTANT, HL_IR_TYPE_I64);
    current.immediate = 40;
    HL_CHECK(hl_ir_append(&block, &current, &forty) == HL_STATUS_OK);
    current.immediate = 2;
    HL_CHECK(hl_ir_append(&block, &current, &two) == HL_STATUS_OK);
    current = instruction(HL_IR_OP_ADD, HL_IR_TYPE_I64);
    current.operand_count = 2;
    current.operands[0] = forty;
    current.operands[1] = two;
    HL_CHECK(hl_ir_append(&block, &current, &sum) == HL_STATUS_OK);
    current = instruction(HL_IR_OP_GUEST_RETURN, HL_IR_TYPE_NONE);
    current.operand_count = 1;
    current.operands[0] = sum;
    HL_CHECK(hl_ir_append(&block, &current, NULL) == HL_STATUS_OK);

    HL_CHECK(hl_code_buffer_init(&code, storage, sizeof(storage)) == HL_STATUS_OK);
    HL_CHECK(hl_codegen_block(HL_HOST_ISA_AARCH64, &block, &code) == HL_STATUS_OK);
    HL_CHECK(code.code_size == 20);
    HL_CHECK(word(storage + 0) == UINT32_C(0xd2800500));
    HL_CHECK(word(storage + 4) == UINT32_C(0xd2800041));
    HL_CHECK(word(storage + 8) == UINT32_C(0x8b010002));
    HL_CHECK(word(storage + 12) == UINT32_C(0xaa0203e0));
    HL_CHECK(word(storage + 16) == UINT32_C(0xd65f03c0));

    memset(storage, 0, sizeof(storage));
    HL_CHECK(hl_code_buffer_init(&code, storage, sizeof(storage)) == HL_STATUS_OK);
    HL_CHECK(hl_codegen_block(HL_HOST_ISA_X86_64, &block, &code) == HL_STATUS_OK);
    HL_CHECK(code.code_size == sizeof(expected_x86_64));
    HL_CHECK(memcmp(storage, expected_x86_64, sizeof(expected_x86_64)) == 0);
    return EXIT_SUCCESS;
}
