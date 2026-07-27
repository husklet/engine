/*
 * Toolchain viability probe for the HL Engine Windows port.
 *
 * This compiles exactly the GNU C constructs the engine source relies on, plus
 * the Win32 surfaces the host backend will need. If this does not build and run
 * clean with the engine's own warning flags, the toolchain is not viable and no
 * backend code should be written against it.
 *
 * Engine usages this mirrors (branch feat/amd64-linux-host):
 *   __attribute__((constructor))  -- 11 sites, incl. hl_target_register_backend
 *   file-scope __asm__            -- 3 sites (dispatch.c, guest/x86_64/translate.c)
 *   __uint128_t                   -- 5 files
 *   _Static_assert               -- translator/identity.c pins the host-ISA enum
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <windows.h>

/* --- 1. constructor attribute: how production backends self-register ------ */
static int constructor_ran;

__attribute__((constructor)) static void probe_constructor(void) { constructor_ran = 1; }

/* --- 2. file-scope __asm__: the run_block/block_return trampoline shape --- */
__asm__(".text\n"
        ".globl probe_trampoline\n"
        "probe_trampoline:\n"
        "  movq %rcx, %rax\n"
        "  addq $1, %rax\n"
        "  ret\n");
uint64_t probe_trampoline(uint64_t value);

/* --- 3. __uint128_t: used by the AArch64 vector-register accessors -------- */
static uint64_t probe_u128(uint64_t a, uint64_t b) {
    __uint128_t wide = (__uint128_t)a * (__uint128_t)b;
    return (uint64_t)(wide >> 64);
}

/* --- 4. _Static_assert ---------------------------------------------------- */
_Static_assert(sizeof(void *) == 8, "the Windows host backend is 64-bit only");

/* --- 5. VEH: the POSIX-signal replacement for guest fault interception ---- */
static volatile LONG veh_hits;
static void *veh_guard_page;

static LONG CALLBACK probe_veh(EXCEPTION_POINTERS *info) {
    DWORD old;
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    InterlockedIncrement(&veh_hits);
    /* Make the faulting store succeed and resume, the way a guest write-protect
       fault on a JIT page must. This is the Windows analogue of mutating the
       ucontext and returning from a POSIX handler. */
    if (!VirtualProtect(veh_guard_page, 4096, PAGE_READWRITE, &old)) return EXCEPTION_CONTINUE_SEARCH;
    return EXCEPTION_CONTINUE_EXECUTION;
}

/* --- 6. W^X JIT memory via placeholder split + dual view ------------------ */
static int probe_dual_alias_jit(void) {
    HANDLE section;
    void *placeholder;
    unsigned char *rw;
    unsigned char *rx;
    uint64_t (*fn)(uint64_t);
    int ok = 0;
    /* mov rax, rcx ; inc rax ; ret  -- Win64 passes arg0 in rcx. */
    static const unsigned char code[] = {0x48, 0x89, 0xC8, 0x48, 0xFF, 0xC0, 0xC3};

    section = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_EXECUTE_READWRITE, 0, 4096, NULL);
    if (section == NULL) return 0;

    /* Reserve 8K as a placeholder, split it, and map the same section twice:
       one RW view for the emitter, one RX view for execution. This is the
       modern Win32 equivalent of the dual-alias MAP_JIT arrangement the macOS
       backend uses (hl_host_code_mapping.writable_address/executable_address). */
    placeholder = VirtualAlloc2(NULL, NULL, 2 * 4096, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, NULL, 0);
    if (placeholder == NULL) {
        CloseHandle(section);
        return 0;
    }
    if (!VirtualFree(placeholder, 4096, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
        CloseHandle(section);
        return 0;
    }

    rw = MapViewOfFile3(section, NULL, placeholder, 0, 4096, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, NULL, 0);
    rx = MapViewOfFile3(section, NULL, (char *)placeholder + 4096, 0, 4096, MEM_REPLACE_PLACEHOLDER, PAGE_EXECUTE_READ,
                        NULL, 0);
    if (rw != NULL && rx != NULL) {
        memcpy(rw, code, sizeof code);
        FlushInstructionCache(GetCurrentProcess(), rx, sizeof code);
        fn = (uint64_t(*)(uint64_t))(void *)rx;
        ok = fn(41) == 42;
    }
    if (rw != NULL) UnmapViewOfFile(rw);
    if (rx != NULL) UnmapViewOfFile(rx);
    CloseHandle(section);
    return ok;
}

int main(void) {
    int failures = 0;
    void *handler;
    volatile unsigned char *probe_page;

    printf("constructor attribute        : %s\n", constructor_ran ? "ok" : "FAIL");
    failures += constructor_ran ? 0 : 1;

    printf("file-scope __asm__ trampoline: %s\n", probe_trampoline(41) == 42 ? "ok" : "FAIL");
    failures += probe_trampoline(41) == 42 ? 0 : 1;

    printf("__uint128_t                  : %s\n", probe_u128(UINT64_MAX, 2) == 1 ? "ok" : "FAIL");
    failures += probe_u128(UINT64_MAX, 2) == 1 ? 0 : 1;

    /* VEH round trip: fault on a PAGE_READONLY page, have the handler make it
       writable and resume. */
    handler = AddVectoredExceptionHandler(1, probe_veh);
    veh_guard_page = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READONLY);
    probe_page = veh_guard_page;
    if (handler != NULL && probe_page != NULL) {
        probe_page[0] = 7;
        printf("VEH fault + resume           : %s\n", (veh_hits == 1 && probe_page[0] == 7) ? "ok" : "FAIL");
        failures += (veh_hits == 1 && probe_page[0] == 7) ? 0 : 1;
    } else {
        printf("VEH fault + resume           : FAIL (setup)\n");
        failures++;
    }
    if (handler != NULL) RemoveVectoredExceptionHandler(handler);
    if (veh_guard_page != NULL) VirtualFree(veh_guard_page, 0, MEM_RELEASE);

    printf("W^X dual-alias JIT mapping    : %s\n", probe_dual_alias_jit() ? "ok" : "FAIL");
    failures += probe_dual_alias_jit() ? 0 : 1;

    printf("\n%s\n", failures == 0 ? "TOOLCHAIN VIABLE" : "TOOLCHAIN NOT VIABLE");
    return failures == 0 ? 0 : 1;
}
