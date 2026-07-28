#ifndef HL_TOOLS_CONFIG_H
#define HL_TOOLS_CONFIG_H

#include <stdint.h>

uint64_t hl_tool_config_matrix_timeout_ms(uint64_t fallback);
int hl_tool_config_github_actions(void);
const char *hl_tool_config_log_selector(void);
const char *hl_tool_config_matrix_scratch(void);
const char *hl_tool_config_docker_command(void);
const char *hl_tool_config_docker_image(void);
const char *hl_tool_config_path(void);

#endif
