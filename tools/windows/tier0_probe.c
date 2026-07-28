/*
 * Tier-0 probe: run a Linux guest on this host WITHOUT the production backend's
 * spawn-a-child step.
 *
 * The shipped engine runs a guest in a CHILD process: hl_engine_run asks the
 * backend to start one, waits for it, and reads the guest's status back. On this
 * host that child is a cold CreateProcess re-execution of the engine's own
 * image, sharing no memory with the parent -- so the entry context the backend
 * hands it, a pointer into the parent's heap, cannot cross. The spawn refuses,
 * in the parent, with a typed NOT_SUPPORTED rather than producing a child that
 * faults on its first dereference.
 *
 * That refusal is about the PROCESS MODEL and nothing else. Everything below it
 * -- the ELF loader, the address-space layout, the translator, the syscall
 * dispatch, the fault primitive -- is independent of whether a child exists, and
 * this probe is how that half is measured while the other half is built. It
 * creates the native host services and calls the target TU's in-process guest
 * entry directly, which is the same call the child would make.
 *
 * It is a measurement tool, not a product: a guest run this way shares the
 * engine's process, so a guest that crashes takes the engine with it and a guest
 * exit is the probe's exit. That is exactly why the production path uses a child
 * and this does not replace it.
 *
 * Exit status is the guest's. Build it against a target TU compiled with
 * -DHL_ENGINE_NO_MAIN=1, since the target root defines its own main().
 */

#include "hl/engine.h"
#include "hl/host_services.h"
#include "hl/linux_abi.h"
#include "hl/windows.h"

#include <stdint.h>
#include <stdio.h>

int hl_run_linux_guest(const hl_host_services *host, hl_linux_abi *box, const char *rootfs,
                       hl_host_handle executable, const void *executable_image, size_t executable_size,
                       uint32_t argc, char *const argv[]);

int main(int argc, char **argv) {
    hl_host_windows *native = NULL;
    hl_host_services services = {0};
    hl_status status;
    if (argc < 2) {
        fprintf(stderr, "usage: %s <x86-64-linux-elf> [args...]\n", argv[0]);
        return 2;
    }
    status = hl_host_windows_create(&native, &services);
    if (status != HL_STATUS_OK) {
        fprintf(stderr, "tier0: host services unavailable (status %d)\n", (int)status);
        return 70;
    }
    /* rootfs NULL is a bare launch: the guest path is a host path. executable
     * INVALID makes the loader open argv[0] itself rather than adopt a handle,
     * which is what keeps this probe independent of the file-binding work. */
    return hl_run_linux_guest(&services, NULL, NULL, HL_HOST_HANDLE_INVALID, NULL, 0, (uint32_t)(argc - 1),
                              argv + 1);
}
