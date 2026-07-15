#include "test.h"
#include "../../src/core/target/bus.h"

typedef struct capture {
    int activate;
    int begin;
    int end;
} capture;

static int activate(void *context) {
    capture *value = context;
    value->activate++;
    return 1;
}

static void begin(void *context) {
    ((capture *)context)->begin++;
}

static void end(void *context) {
    ((capture *)context)->end++;
}

static uint64_t fault(uint64_t address, uint64_t size) {
    return address + size;
}

int main(void) {
    capture value = {0};
    const hl_guest_bus_ops operations = {activate, begin, end};
    hl_target_bus bus;
    hl_target_bus_init(&bus, &operations, &value);
    HL_CHECK(!hl_target_bus_active(&bus));
    hl_target_bus_bind(&bus, fault, 1, 4);
    HL_CHECK(hl_target_bus_active(&bus) && value.activate == 1);
    HL_CHECK(hl_target_bus_fault(&bus, 7, 5) == 12);
    hl_target_bus_changed(&bus, 3, 0);
    HL_CHECK(hl_target_bus_active(&bus));
    hl_target_bus_changed(&bus, 5, 0);
    HL_CHECK(hl_target_bus_active(&bus) && value.activate == 1);
    hl_target_bus_begin(&bus);
    hl_target_bus_end(&bus);
    HL_CHECK(value.begin == 1 && value.end == 1);
    return 0;
}
