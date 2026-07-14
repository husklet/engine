#ifndef HL_CORE_OPTIONS_H
#define HL_CORE_OPTIONS_H

const char *hl_option_get(const char *name);
int hl_option_set(const char *name, const char *value, int overwrite);
int hl_option_unset(const char *name);
void hl_option_reset(void);

#endif
