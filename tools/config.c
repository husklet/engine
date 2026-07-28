#include "config.h"

#include <errno.h>
#include <stdlib.h>

static const char *environment_value(const char *name) {
    const char *value = getenv(name);
    return value != NULL && value[0] != '\0' ? value : NULL;
}

uint64_t hl_tool_config_matrix_timeout_ms(uint64_t fallback) {
    const char *value = environment_value("HL_MATRIX_CASE_TIMEOUT_MS");
    char *end;
    unsigned long parsed;
    if (value == NULL) return fallback;
    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed < 1000 || parsed > 3600000) return fallback;
    return (uint64_t)parsed;
}

int hl_tool_config_github_actions(void) {
    return environment_value("GITHUB_ACTIONS") != NULL;
}

const char *hl_tool_config_log_selector(void) {
    return environment_value("HL_LOG");
}

const char *hl_tool_config_matrix_scratch(void) {
    return environment_value("HL_MATRIX_SCRATCH_DIR");
}

const char *hl_tool_config_docker_command(void) {
    const char *value = environment_value("DOCKER");
    return value != NULL ? value : "docker";
}

const char *hl_tool_config_docker_image(void) {
    const char *value = environment_value("DOCKER_IMAGE");
    return value != NULL ? value : "debian:stable-slim";
}
