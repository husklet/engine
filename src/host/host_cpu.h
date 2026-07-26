#ifndef HL_HOST_CPU_H
#define HL_HOST_CPU_H

/* The host-CPU axis, on its own.
 *
 * DOCS.md section 1 names three independent axes: guest OS, guest ISA, and
 * host platform. "Host platform" is really two axes -- host OS and host CPU --
 * and the tree only ever had a name for the first (src/host/<os>/, the
 * hostBackends table in flake.nix). Everything that depended on the host CPU
 * spelled it `defined(__aarch64__)` inline, or did not test for it at all
 * because there was only ever one answer.
 *
 * This header gives the second axis a name so the two can be composed. Use
 * HL_HOST_CPU_* rather than the compiler's own predefines, for two reasons:
 * the predefines are spelled differently per compiler (__aarch64__ vs
 * _M_ARM64), and a bare `defined(__x86_64__)` says nothing about which OS's
 * context and calling conventions apply -- which is exactly the confusion that
 * left an Apple-shaped `uc_mcontext->__ss.__rip` sitting under a plain
 * `#elif defined(__x86_64__)` in linux_abi/signal.c.
 *
 * Related, and deliberately NOT the same thing:
 *   HL_GUEST_ISA_*  (include/hl/config.h) -- the ISA of the program being run.
 *   HL_HOST_ISA_*   (include/hl/codegen.h) -- the runtime value naming a
 *                   codegen target, used for cache identity.
 * A guest ISA equal to the host CPU permits same-ISA transliteration; it is
 * never an excuse to conflate the two.
 */

#if defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
#define HL_HOST_CPU_AARCH64 1
#define HL_HOST_CPU_NAME "aarch64"
#elif defined(__x86_64__) || defined(__amd64__) || defined(_M_X64)
#define HL_HOST_CPU_X86_64 1
#define HL_HOST_CPU_NAME "x86_64"
#else
#error "hl engine has no host-CPU definition for this target"
#endif

/* HL_HOST_ISA_* values, available to preprocessor-time code that must not
 * include the public codegen header. Keep in step with include/hl/codegen.h;
 * the static assertion in src/translator/identity.c enforces it. */
#define HL_HOST_CPU_ISA_AARCH64 1
#define HL_HOST_CPU_ISA_X86_64 2

#if defined(HL_HOST_CPU_AARCH64)
#define HL_HOST_CPU_ISA HL_HOST_CPU_ISA_AARCH64
#else
#define HL_HOST_CPU_ISA HL_HOST_CPU_ISA_X86_64
#endif

#endif
