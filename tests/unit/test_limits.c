#include "test.h"

#include "../../src/production/os/linux/container/limits/table.h"

#include <pthread.h>
#include <stdatomic.h>

typedef struct limit_writer_context {
    hl_limit_table *table;
    _Atomic int done;
} limit_writer_context;

static void *write_limits(void *opaque) {
    limit_writer_context *context = opaque;
    for (int iteration = 0; iteration < 100000; ++iteration) {
        hl_limit_table_set(context->table, 3, 100, 200);
        hl_limit_table_set(context->table, 3, 300, 400);
    }
    atomic_store_explicit(&context->done, 1, memory_order_release);
    return NULL;
}

int main(void) {
    hl_limit_table table;
    uint64_t current = 11;
    uint64_t maximum = 22;

    hl_limit_table_init(&table);
    HL_CHECK(!hl_limit_table_get(&table, 7, &current, &maximum));
    HL_CHECK(current == 11 && maximum == 22);
    HL_CHECK(hl_limit_table_set(&table, -1, 1, 2) == -1);
    HL_CHECK(hl_limit_table_set(&table, HL_LIMIT_COUNT, 1, 2) == -1);
    HL_CHECK(hl_limit_table_set(&table, 7, 20480, 1048576) == 0);
    HL_CHECK(hl_limit_table_get(&table, 7, &current, &maximum));
    HL_CHECK(current == 20480 && maximum == 1048576);
    HL_CHECK(hl_limit_table_set(&table, 7, 4096, UINT64_MAX) == 0);
    HL_CHECK(hl_limit_table_get(&table, 7, &current, &maximum));
    HL_CHECK(current == 4096 && maximum == UINT64_MAX);
    HL_CHECK(hl_limit_table_get(&table, 7, NULL, NULL));

    {
        limit_writer_context context = {&table, 0};
        pthread_t writer;
        HL_CHECK(pthread_create(&writer, NULL, write_limits, &context) == 0);
        while (!atomic_load_explicit(&context.done, memory_order_acquire)) {
            if (hl_limit_table_get(&table, 3, &current, &maximum)) {
                HL_CHECK((current == 100 && maximum == 200) || (current == 300 && maximum == 400));
            }
        }
        HL_CHECK(pthread_join(writer, NULL) == 0);
    }
    hl_limit_table_init(&table);
    HL_CHECK(!hl_limit_table_get(&table, 7, &current, &maximum));
    return EXIT_SUCCESS;
}
