#include "test.h"

#include "../../src/linux_abi/container/ports.h"

int main(void) {
    hl_linux_ports ports;
    uint32_t index;

    hl_linux_ports_init(&ports);
    HL_CHECK(hl_linux_ports_count(&ports) == 0);
    HL_CHECK(hl_linux_ports_host(&ports, 8080) == 8080);
    HL_CHECK(hl_linux_ports_address(&ports, 8080) == 0);
    HL_CHECK(!hl_linux_ports_contains(&ports, 8080));

    HL_CHECK(hl_linux_ports_add(&ports, 18080, 8080) == 0);
    HL_CHECK(hl_linux_ports_add(&ports, 28080, 8080) == 0);
    HL_CHECK(hl_linux_ports_add_address(&ports, UINT32_C(0x0100007f), 38080, 9080) == 0);
    HL_CHECK(hl_linux_ports_count(&ports) == 3);
    HL_CHECK(hl_linux_ports_contains(&ports, 8080));
    HL_CHECK(hl_linux_ports_host(&ports, 8080) == 18080);
    HL_CHECK(hl_linux_ports_host(&ports, 9090) == 9090);
    HL_CHECK(hl_linux_ports_address(&ports, 9080) == UINT32_C(0x0100007f));
    HL_CHECK(hl_linux_ports_address(&ports, 9090) == 0);

    HL_CHECK(hl_linux_ports_add(&ports, 0, 1) == -1);
    HL_CHECK(hl_linux_ports_add(&ports, 1, 0) == -1);
    HL_CHECK(hl_linux_ports_count(&ports) == 3);
    for (index = 3; index < HL_LINUX_PORT_CAPACITY; ++index)
        HL_CHECK(hl_linux_ports_add(&ports, (uint16_t)(1000 + index), (uint16_t)(2000 + index)) == 0);
    HL_CHECK(hl_linux_ports_count(&ports) == HL_LINUX_PORT_CAPACITY);
    HL_CHECK(hl_linux_ports_add(&ports, 3000, 4000) == -1);
    HL_CHECK(hl_linux_ports_count(&ports) == HL_LINUX_PORT_CAPACITY);

    HL_CHECK(hl_linux_ports_add(NULL, 1, 1) == -1);
    HL_CHECK(hl_linux_ports_host(NULL, 8080) == 8080);
    HL_CHECK(hl_linux_ports_address(NULL, 8080) == 0);
    HL_CHECK(!hl_linux_ports_contains(NULL, 8080));
    HL_CHECK(hl_linux_ports_count(NULL) == 0);
    return EXIT_SUCCESS;
}
