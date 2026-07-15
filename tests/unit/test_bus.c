#include "test.h"
#include "../../src/core/bus.h"

typedef struct capture {
    int active, begin, end;
} capture;

static int activate(void *p) {
    ((capture *)p)->active++;
    return 1;
}

static void begin(void *p) {
    ((capture *)p)->begin++;
}

static void end(void *p) {
    ((capture *)p)->end++;
}

static uint64_t query(uint64_t a, uint64_t s) {
    return a ^ s;
}

int main(void) {
    capture c = {0};
    hl_guest_bus_ops o = {activate, begin, end};
    hl_guest_bus b;
    hl_guest_bus_init(&b, &o, &c);
    HL_CHECK(!hl_guest_bus_active(&b) && hl_guest_bus_fault(&b, 3, 5) == 0);
    hl_guest_bus_bind(&b, query, 0, 2);
    hl_guest_bus_changed(&b, 1, 1);
    HL_CHECK(!hl_guest_bus_active(&b));
    hl_guest_bus_changed(&b, 3, 1);
    hl_guest_bus_changed(&b, 4, 0);
    hl_guest_bus_changed(&b, 5, 1);
    HL_CHECK(hl_guest_bus_active(&b) && c.active == 1 && hl_guest_bus_fault(&b, 3, 5) == 6);
    hl_guest_bus_begin(&b);
    hl_guest_bus_end(&b);
    HL_CHECK(c.begin == 1 && c.end == 1);
    return 0;
}
