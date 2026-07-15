#include "services.h"

#include <string.h>

void hl_target_services_inject(hl_target_services *target, const hl_host_services *injected) {
    if (target != NULL) target->injected = injected;
}

int hl_target_services_bind(hl_target_services *target) {
    if (target == NULL) return -1;
    return hl_native_host_bind(&target->native, &target->bound, target->injected);
}

const hl_host_services *hl_target_services_effective(const hl_target_services *target) {
    if (target == NULL) return NULL;
    return target->injected != NULL ? target->injected : &target->bound;
}

hl_host_services *hl_target_services_bound(hl_target_services *target) {
    return target == NULL ? NULL : &target->bound;
}

void hl_target_services_destroy(hl_target_services *target) {
    if (target == NULL) return;
    hl_native_host_destroy(target->native);
    memset(target, 0, sizeof(*target));
}
