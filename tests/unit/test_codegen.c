#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "test.h"

#include "hl/codegen.h"

#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

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

static hl_status execute_native(const uint8_t *code, size_t code_size, hl_ir_exit *out_exit) {
    long page_size = sysconf(_SC_PAGESIZE);
    size_t mapping_size;
    void *mapping;
    hl_code_entry entry;
    hl_status status;
    HL_CHECK(page_size > 0);
    mapping_size = (code_size + (size_t)page_size - 1u) & ~((size_t)page_size - 1u);
    mapping = mmap(NULL, mapping_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    HL_CHECK(mapping != MAP_FAILED);
    memcpy(mapping, code, code_size);
    HL_CHECK(mprotect(mapping, mapping_size, PROT_READ | PROT_EXEC) == 0);
    memcpy(&entry, &mapping, sizeof(entry));
    status = entry(out_exit);
    HL_CHECK(munmap(mapping, mapping_size) == 0);
    return status;
}

static int check_native_exit(uint16_t opcode, uint32_t expected_kind) {
    hl_ir_instruction instructions[3];
    hl_ir_instruction current;
    hl_ir_block block;
    hl_ir_value value;
    hl_ir_value detail;
    uint8_t storage[128];
    hl_code_buffer code;
    hl_ir_exit native_exit;
    uint32_t host_isa;

    HL_CHECK(hl_ir_block_init(&block, UINT64_C(0x500000), instructions, HL_ARRAY_COUNT(instructions)) == HL_STATUS_OK);
    current = instruction(HL_IR_OP_CONSTANT, HL_IR_TYPE_I64);
    current.immediate = UINT64_C(0x1122334455667788);
    HL_CHECK(hl_ir_append(&block, &current, &value) == HL_STATUS_OK);
    current.immediate = UINT64_C(0x8877665544332211);
    HL_CHECK(hl_ir_append(&block, &current, &detail) == HL_STATUS_OK);
    current = instruction(opcode, HL_IR_TYPE_NONE);
    current.operand_count = 2;
    current.operands[0] = value;
    current.operands[1] = detail;
    HL_CHECK(hl_ir_append(&block, &current, NULL) == HL_STATUS_OK);

#if defined(__aarch64__)
    host_isa = HL_HOST_ISA_AARCH64;
#elif defined(__x86_64__)
    host_isa = HL_HOST_ISA_X86_64;
#else
    host_isa = 0;
#endif
    if (host_isa != 0) {
        HL_CHECK(hl_code_buffer_init(&code, storage, sizeof(storage)) == HL_STATUS_OK);
        HL_CHECK(hl_codegen_block(host_isa, &block, &code) == HL_STATUS_OK);
        memset(&native_exit, 0xa5, sizeof(native_exit));
        HL_CHECK(execute_native(storage, code.code_size, &native_exit) == HL_STATUS_OK);
        HL_CHECK(native_exit.kind == expected_kind && native_exit.reserved == 0 &&
                 native_exit.value == UINT64_C(0x1122334455667788) &&
                 native_exit.detail == UINT64_C(0x8877665544332211));
    }
    return EXIT_SUCCESS;
}

int main(void) {
    hl_ir_instruction instructions[8];
    hl_ir_block block;
    hl_ir_instruction current;
    hl_ir_value forty;
    hl_ir_value two;
    hl_ir_value sum;
    uint8_t storage[128];
    hl_code_buffer code;
    hl_ir_exit native_exit;

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
    HL_CHECK(code.code_size == 40);
    HL_CHECK(word(storage + 0) == UINT32_C(0xd2800501));
    HL_CHECK(word(storage + 4) == UINT32_C(0xd2800042));
    HL_CHECK(word(storage + 8) == UINT32_C(0x8b020023));
#if defined(__aarch64__)
    memset(&native_exit, 0xa5, sizeof(native_exit));
    HL_CHECK(execute_native(storage, code.code_size, &native_exit) == HL_STATUS_OK);
    HL_CHECK(native_exit.kind == HL_IR_EXIT_RETURN && native_exit.reserved == 0 && native_exit.value == 42 &&
             native_exit.detail == 0);
#endif

    memset(storage, 0, sizeof(storage));
    HL_CHECK(hl_code_buffer_init(&code, storage, sizeof(storage)) == HL_STATUS_OK);
    HL_CHECK(hl_codegen_block(HL_HOST_ISA_X86_64, &block, &code) == HL_STATUS_OK);
    HL_CHECK(code.code_size == 60);
    HL_CHECK(storage[0] == UINT8_C(0x48) && storage[1] == UINT8_C(0xb8));
#if defined(__x86_64__)
    memset(&native_exit, 0xa5, sizeof(native_exit));
    HL_CHECK(execute_native(storage, code.code_size, &native_exit) == HL_STATUS_OK);
    HL_CHECK(native_exit.kind == HL_IR_EXIT_RETURN && native_exit.reserved == 0 && native_exit.value == 42 &&
             native_exit.detail == 0);
#endif
    HL_CHECK(check_native_exit(HL_IR_OP_SYSCALL_EXIT, HL_IR_EXIT_SYSCALL) == EXIT_SUCCESS);
    HL_CHECK(check_native_exit(HL_IR_OP_FAULT_EXIT, HL_IR_EXIT_FAULT) == EXIT_SUCCESS);
    return EXIT_SUCCESS;
}
