#include "test.h"

#include "../../src/production/os/linux/container/readonly/table.h"

#include <string.h>

int main(void) {
    hl_readonly_table table;
    char path[HL_READONLY_PATH_CAPACITY + 1];

    hl_readonly_table_init(&table);
    HL_CHECK(hl_readonly_table_empty(&table));
    HL_CHECK(hl_readonly_table_add(&table, "relative") == -1);
    HL_CHECK(hl_readonly_table_add(&table, "/srv/data") == 0);
    HL_CHECK(hl_readonly_table_add(&table, "/srv/data") == 0);
    HL_CHECK(!hl_readonly_table_empty(&table));
    HL_CHECK(hl_readonly_table_denies(&table, "/srv/data"));
    HL_CHECK(hl_readonly_table_denies(&table, "/srv/data/index.db"));
    HL_CHECK(!hl_readonly_table_denies(&table, "/srv/database"));
    HL_CHECK(!hl_readonly_table_denies(&table, "/srv"));

    for (int index = 1; index < HL_READONLY_TABLE_CAPACITY; ++index) {
        char entry[32];
        snprintf(entry, sizeof(entry), "/mount/%d", index);
        HL_CHECK(hl_readonly_table_add(&table, entry) == 0);
    }
    HL_CHECK(hl_readonly_table_add(&table, "/overflow") == -1);

    memset(path, 'x', sizeof(path));
    path[0] = '/';
    path[sizeof(path) - 1] = '\0';
    HL_CHECK(hl_readonly_table_add(&table, path) == -1);
    hl_readonly_table_init(&table);
    HL_CHECK(hl_readonly_table_empty(&table));
    HL_CHECK(!hl_readonly_table_denies(&table, "/srv/data"));
    return EXIT_SUCCESS;
}
