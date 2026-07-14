#include "../../src/core/cli.h"
#include "test.h"

int main(void) {
    char *guest[] = {"hl-engine", "--rootfs", "root", "program", NULL};
    char *config[] = {"hl-engine", "--configfile", "launch.bin", "ignored", NULL};
    char *short_config[] = {"hl-engine", "--configfile", NULL};
    char *server[] = {"hl-engine", "--rootfs", "root", "--server", "socket", NULL};
    char *client[] = {"hl-engine", "--client", "socket", "program", NULL};
    char *first_wins[] = {"hl-engine", "--client", "socket", "--server", "other", NULL};
    hl_cli_route route;

    route = hl_cli_route_parse(4, guest);
    HL_CHECK(route.mode == HL_CLI_GUEST && route.config_path == NULL);
    route = hl_cli_route_parse(4, config);
    HL_CHECK(route.mode == HL_CLI_CONFIG && route.config_path == config[2]);
    HL_CHECK(hl_cli_route_parse(2, short_config).mode == HL_CLI_GUEST);
    HL_CHECK(hl_cli_route_parse(5, server).mode == HL_CLI_SERVER);
    HL_CHECK(hl_cli_route_parse(4, client).mode == HL_CLI_CLIENT);
    HL_CHECK(hl_cli_route_parse(5, first_wins).mode == HL_CLI_CLIENT);
    HL_CHECK(hl_cli_route_parse(0, NULL).mode == HL_CLI_GUEST);
    return EXIT_SUCCESS;
}
