#include "test.h"

#include "../../src/translator/reloc.h"

#include <string.h>

static uint64_t decode(const uint32_t words[4]) {
    return (uint64_t)((words[0] >> 5) & UINT32_C(0xffff)) | ((uint64_t)((words[1] >> 5) & UINT32_C(0xffff)) << 16) |
           ((uint64_t)((words[2] >> 5) & UINT32_C(0xffff)) << 32) |
           ((uint64_t)((words[3] >> 5) & UINT32_C(0xffff)) << 48);
}

int main(void) {
    hl_reloc storage[2], imported[2];
    hl_reloc_table table;
    uint64_t original = UINT64_C(0x123456789abcdef0);
    uint64_t slide = UINT64_C(0x1011223344556677);
    uint32_t rd = 9;
    uint32_t words[4] = {
        UINT32_C(0xd2800000) | ((uint32_t)original & UINT32_C(0xffff)) << 5 | rd,
        UINT32_C(0xf2800000) | 1u << 21 | ((uint32_t)(original >> 16) & UINT32_C(0xffff)) << 5 | rd,
        UINT32_C(0xf2800000) | 2u << 21 | ((uint32_t)(original >> 32) & UINT32_C(0xffff)) << 5 | rd,
        UINT32_C(0xf2800000) | 3u << 21 | ((uint32_t)(original >> 48) & UINT32_C(0xffff)) << 5 | rd,
    };

    memset(storage, 0xa5, sizeof(storage));
    hl_reloc_init(&table, storage, 2);
    HL_CHECK(table.count == 0 && table.capacity == 2 && table.records == storage);
    HL_CHECK(hl_reloc_add(&table, 16, UINT32_C(0x00020301)));
    HL_CHECK(hl_reloc_add(&table, 64, UINT32_C(0x00000003)));
    HL_CHECK(!hl_reloc_add(&table, 80, 4));
    HL_CHECK(table.count == 2 && storage[0].off == 16 && storage[0].info == UINT32_C(0x00020301));

    imported[0].off = 128;
    imported[0].info = 4;
    HL_CHECK(hl_reloc_import(&table, imported, 1));
    HL_CHECK(table.count == 1 && storage[0].off == 128 && storage[0].info == 4);
    HL_CHECK(!hl_reloc_import(&table, imported, 3) && table.count == 1);
    hl_reloc_reset(&table);
    HL_CHECK(table.count == 0);

    hl_reloc_slide(words, slide);
    HL_CHECK(decode(words) == original + slide);
    HL_CHECK((words[0] & UINT32_C(0x1f)) == rd && (words[1] & UINT32_C(0x1f)) == rd &&
             (words[2] & UINT32_C(0x1f)) == rd && (words[3] & UINT32_C(0x1f)) == rd);
    HL_CHECK((words[0] & UINT32_C(0xff800000)) == UINT32_C(0xd2800000));
    HL_CHECK((words[1] & UINT32_C(0xff800000)) == UINT32_C(0xf2800000));
    return EXIT_SUCCESS;
}
