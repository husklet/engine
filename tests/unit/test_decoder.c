#include "../../src/translator/guest/x86_64/decoder.h"
#include "test.h"

#include <stdint.h>

static int decode(const uint8_t *bytes, hl_x86_insn *insn) {
    return hl_x86_decode((uint64_t)(uintptr_t)bytes, insn);
}

int main(void) {
    hl_x86_insn insn;

    const uint8_t nop[] = {0x90};
    HL_CHECK(decode(nop, &insn) == 1);
    HL_CHECK(insn.len == 1 && insn.op == 0x90 && !insn.has_modrm && insn.opsize == 4);

    const uint8_t prefixes[] = {0xf0, 0xf3, 0x64, 0x67, 0x66, 0x01, 0x44, 0x8d, 0xf0};
    HL_CHECK(decode(prefixes, &insn) == 9);
    HL_CHECK(insn.lock && insn.rep && insn.seg == 1 && insn.addr32 && insn.p66 && insn.opsize == 2);
    HL_CHECK(insn.op == 0x01 && insn.mod == 1 && insn.reg == 0 && insn.rm == 4 && insn.is_mem);
    HL_CHECK(insn.m_hasbase && insn.m_base == 5 && insn.m_hasindex && insn.m_index == 1);
    HL_CHECK(insn.m_scale == 2 && insn.disp == -16);

    const uint8_t rex_sib[] = {0x4f, 0x8b, 0x84, 0xcc, 0x78, 0x56, 0x34, 0x12};
    HL_CHECK(decode(rex_sib, &insn) == 8);
    HL_CHECK(insn.has_rex && insn.rexW && insn.rexR && insn.rexX && insn.rexB && insn.opsize == 8);
    HL_CHECK(insn.reg == 8 && insn.m_base == 12 && insn.m_index == 9 && insn.m_scale == 3);
    HL_CHECK(insn.disp == 0x12345678);

    const uint8_t rip_relative[] = {0x48, 0x8b, 0x05, 0xfc, 0xff, 0xff, 0xff};
    HL_CHECK(decode(rip_relative, &insn) == 7);
    HL_CHECK(insn.rip_rel && insn.is_mem && !insn.m_hasbase && insn.disp == -4);

    const uint8_t movabs[] = {0x48, 0xb8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11};
    HL_CHECK(decode(movabs, &insn) == 10);
    HL_CHECK(insn.imm_bytes == 8 && (uint64_t)insn.imm == UINT64_C(0x1122334455667788));

    const uint8_t signed_imm8[] = {0x83, 0xc0, 0x80};
    HL_CHECK(decode(signed_imm8, &insn) == 3);
    HL_CHECK(insn.mod == 3 && insn.rm_reg == 0 && insn.imm_bytes == 1 && insn.imm == -128);

    const uint8_t jcc[] = {0x0f, 0x85, 0x78, 0x56, 0x34, 0x12};
    HL_CHECK(decode(jcc, &insn) == 6);
    HL_CHECK(insn.two && !insn.has_modrm && insn.imm_bytes == 4 && insn.imm == 0x12345678);

    const uint8_t map3[] = {0x66, 0x0f, 0x3a, 0x0f, 0xc1, 0x07};
    HL_CHECK(decode(map3, &insn) == 6);
    HL_CHECK(insn.two && insn.map3 == 3 && insn.op == 0x0f && insn.p66);
    HL_CHECK(insn.has_modrm && insn.mod == 3 && insn.imm_bytes == 1 && insn.imm == 7);

    const uint8_t vex2[] = {0xc5, 0xf9, 0x70, 0xc1, 0x1b};
    HL_CHECK(decode(vex2, &insn) == 5);
    HL_CHECK(insn.vex && !insn.evex && insn.vex_map == 1 && insn.vex_pp == 1 && insn.p66);
    HL_CHECK(insn.vvvv == 0 && insn.vex_l == 0 && insn.op == 0x70 && insn.imm == 0x1b);

    const uint8_t vex3[] = {0xc4, 0x41, 0xa5, 0x70, 0xcc, 0x7f};
    HL_CHECK(decode(vex3, &insn) == 6);
    HL_CHECK(insn.vex && insn.vex_map == 1 && insn.vex_w && insn.opsize == 8);
    HL_CHECK(insn.rexR && !insn.rexX && insn.rexB && insn.vvvv == 11 && insn.vex_l == 1);
    HL_CHECK(insn.reg == 9 && insn.rm_reg == 12 && insn.imm == 0x7f);

    const uint8_t evex[] = {0x62, 0x01, 0xed, 0xd3, 0x70, 0xcc, 0x80};
    HL_CHECK(decode(evex, &insn) == 7);
    HL_CHECK(insn.vex && insn.evex && insn.vex_map == 1 && insn.vex_w && insn.opsize == 8);
    HL_CHECK(insn.rexR && insn.rexX && insn.rexB && insn.vvvv == 18);
    HL_CHECK(insn.vex_l == 2 && insn.evex_z && insn.evex_b && insn.evex_mask == 3);
    HL_CHECK(insn.reg == 9 && insn.rm_reg == 12 && insn.imm == -128);

    const uint8_t vzeroupper[] = {0xc5, 0xf8, 0x77};
    HL_CHECK(decode(vzeroupper, &insn) == 3);
    HL_CHECK(insn.vex && insn.op == 0x77 && !insn.has_modrm && insn.imm_bytes == 0);

    const uint8_t moffs64[] = {0xa1, 1, 2, 3, 4, 5, 6, 7, 8};
    HL_CHECK(decode(moffs64, &insn) == 9 && insn.imm_bytes == 8);
    const uint8_t moffs32[] = {0x67, 0xa1, 1, 2, 3, 4};
    HL_CHECK(decode(moffs32, &insn) == 6 && insn.addr32 && insn.imm_bytes == 4);

    return 0;
}
