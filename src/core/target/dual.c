#include "hl/host_services.h"
#include "hl/linux_abi.h"

/* Compatibility entry for the legacy config-file launcher.  The typed engine
 * API selects either embedded backend by config.guest_isa; this untyped entry
 * retains the native AArch64 default used by the macOS command launcher. */
int hl_aarch64_run_linux_guest(const hl_host_services *host, hl_linux_abi *box, const char *rootfs,
                               uint32_t argc, char *const argv[]);

int hl_run_linux_guest(const hl_host_services *host, hl_linux_abi *box, const char *rootfs, uint32_t argc,
                       char *const argv[]) {
    return hl_aarch64_run_linux_guest(host, box, rootfs, argc, argv);
}
