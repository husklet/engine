# The x86-64 Linux host

How an x86-64 Linux machine becomes a host, and why the shape of the change is what it is. DOCS.md is
normative; this file records the reasoning behind one addition to it.

## 1. What was actually missing

The obvious reading of "add an x86-64 Linux host" is that `src/host/` gains a backend, the way
`src/host/windows/` is reserved to. That reading is wrong, and it is worth being precise about why,
because it determines the size of the work.

`src/host/<os>/` implements `hl_host_services` — files, processes, clocks, memory, sync. That layer was
already host-CPU-neutral: `src/host/linux/` compiles and passes its provider tests on x86-64 unchanged.
The host CPU does not appear there at all, apart from one `cntfrq_el0` read.

What is host-CPU-specific is the **translator**. `src/translator/host/{aarch64,x86_64}/` used to look like a
per-host-CPU code generator with both members present, dispatched by `src/translator/codegen.c` on a runtime
`host_isa`. That pipeline was real, symmetric, and **not what the engine runs** — `hl_codegen_*` had no callers
outside `tests/unit/test_codegen.c`, and its IR (17 opcodes, no vectors, no flags, no atomics) could express
neither production frontend — so it was deleted, precisely because the layout kept suggesting otherwise
(docs/amd64-host-findings.md 3.1). What is left under `host/` is `aarch64/asm.{c,h}`, the ARM64 assembler.

The production frontends emit host machine code directly:

- `src/translator/guest/x86_64/` is a complete x86-64 frontend whose back end is ARM64. `emit.c`'s own
  header says so: *"arm64 host emitters + NEON/SSE encoders (xmm->v0..15)"*. Its register model is stated
  at the top of `src/core/target/x86_64.c`: guest `rax..r15` in host `x0..x15`, `cpu` pinned in host `x28`.
- `src/translator/guest/aarch64/` is not a translator at all. `translate.c`'s header calls it *"the
  aarch64-Linux -> arm64-host transliterator. Same-ISA: copy most instructions verbatim; MANGLE only
  stolen-register (x18/x28/x30) users."* Its default case writes the guest instruction word straight into
  the host code cache. There is no decoder, because on an AArch64 host it does not need one.

So on an x86-64 host, neither guest ISA has a back end. The aarch64 guest does not even have a front end.
This is new code generation, not a port.

## 2. The seam that makes it additive

The whole of `core/dispatch.c`'s contract with a backend is two calls:

```c
code = translate_block(G_PC(c));   /* produce something callable for this guest PC */
run_block(c, code);                /* call it */
/* on return: c->reason says why it stopped, G_PC(c) is the next guest PC,
   and every piece of guest architectural state is back in *c */
```

Nothing in that contract requires `code` to be host machine code, and `G_OWN_TRAMPOLINES` already existed
as the escape hatch for a backend whose register model differs from the shared one — the x86-64 guest
frontend uses it today. A backend that *decodes and executes* rather than emits therefore satisfies the
same contract, and everything on the other side of it is reused verbatim: the dispatcher, the block cache
and its STW/generation machinery, all of `src/linux_abi` (syscalls, signals, container, VFS, ELF loading),
and checkpoint/restore.

Both target translation units now fork on the host CPU, and nothing else about them changes:

```c
#if defined(HL_HOST_CPU_AARCH64)
#include ".../host/aarch64/asm.h"   /* + the e_* wrappers */
#include ".../guest/<isa>/cache.c"
#include ".../guest/<isa>/stubs.c"  /* x86-64 guest: emit.c + address.h */
#include ".../guest/<isa>/translate.c"
#else
#include ".../guest/<isa>/interp.c"
#endif
```

`struct cpu` is deliberately shared by both backends rather than being specialised per backend. It is the
checkpoint format — `sizeof(struct cpu)` is written into the image and validated on restore — so one
layout is what lets the two backends read each other's guest state. The consequence is that the
interpreter carries `host_save[12]` and `host_v[16]`, which are AArch64 callee-saved slots it never uses.
That waste is the price of a stable image format, and it is the right trade.

The same fork applies to the dispatch seam. `guest/<isa>/dispatch.h` belongs to the JIT: it patches ARM64
branch encodings into the W^X arena and assumes guest registers live in matching host registers.
`guest/<isa>/interp_dispatch.h` is its interpreter counterpart. `abi.h` above it is pure guest ABI and is
shared — which is the correct division, and was already almost right: the seam is per
(guest ISA, host CPU), not per guest ISA alone, and that only became visible once there was a second host
CPU.

## 3. Why an interpreter first

Two backends were possible for each guest ISA on an x86-64 host:

| guest | option | cost |
|---|---|---|
| x86-64 | same-ISA transliterator, the mirror of `guest/aarch64` on an ARM64 host | moderate; fast |
| x86-64 | interpreter over the existing host-neutral `decode.c` | smaller; slow |
| aarch64 | full aarch64 -> x86-64 JIT | very large — a new frontend *and* a new backend |
| aarch64 | interpreter | large; slow |

Correctness comes first, so both start as interpreters. That choice buys one property worth more than the
speed it costs: an interpreter's `struct cpu` is *always* authoritative. A guest fault lands in ordinary C
with `cpu->pc` already exact, so `signal_capture` needs no host-register reconstruction and no
instruction-boundary provenance map — the two most delicate parts of the JIT's fault path
(`jit_instruction_guest_pc`, the folded-fault `mscratch[4..7]` replay) simply do not arise.

The interpreter's fault model is instead: a thread-local marker around every guest access, `sigsetjmp` at
the top of `run_block`, and a `siglongjmp` out of the host handler once the guest signal frame is built.

A same-ISA x86-64 transliterator is the obvious next step for performance and is a strictly additive
third arm of the same fork. It is not a prerequisite for the host being supported.

### 3.1 The register problem the ARM64 diagonal never had

Worth settling before Stage 2 starts, because it is the one place where the x86-64 diagonal is not a
mechanical mirror of the AArch64 one.

`guest/aarch64` on an ARM64 host keeps all 31 guest GPRs in the matching host GPRs and *steals* a handful
(x18/x28/x30, plus x16/x17) for the engine: `x28` holds the `struct cpu *`, `x30` the host link, `x18` is
scratch. It can afford that because 31 registers is more than a guest ABI uses, and `is_stolen()` plus
`emit_mangled_x18()` rewrite the rare instruction that names one.

An x86-64 host has 16 GPRs and an x86-64 guest wants all 16. There is nothing to steal without cost. Three
ways out:

1. **Keep the guest register file in memory** and make every guest instruction load-operate-store against
   `struct cpu`. Simple, and it throws away most of what same-ISA transliteration is for.
2. **Steal a GPR** — say `%r15` — spill guest `r15` to `cpu->r[15]`, and mangle any instruction naming it.
   This is the direct mirror of the x18 treatment and reuses a pattern already proven in this tree. It
   costs on `r15`-using code, which is not rare in register-hungry compiled output.
3. **Steal no GPR: reach `struct cpu` through a segment base.** x86-64 has `%fs` and `%gs` with
   kernel-settable bases, and a Linux x86-64 guest's TLS lives in `%fs`. That leaves `%gs` for the engine,
   so `mov %gs:0, %reg` recovers the cpu pointer at zero register cost.

Option 3 is the right one, and note that it is not a new idea here — it is exactly what the ARM64 host
already does. `hl_a64_load_cpu` (`src/translator/host/aarch64/asm.c`) emits `mrs xN, TPIDRRO_EL0` plus a
masked load to read the pthread TSD from inside emitted code, for the same reason: recovering engine state
without spending a guest-visible register. `%gs` is the x86-64 spelling of that same trick.

The cost of option 3 is that the engine now owns the real `%gs`, so a guest that uses `%gs` itself must be
virtualised. That is already largely paid for: `struct cpu` carries `gs_base` alongside `fs_base`, and the
`arch_prctl` service already models both, so guest `%gs` accesses are rewritten against `cpu->gs_base`
rather than executed. Guest `%gs` use is rare in practice; guest `%fs` use is ubiquitous and stays
untouched.

**Built, off by default: `docs/amd64-host-translit.md`.** The register model above is what
`src/translator/guest/x86_64/translit/` implements, with two corrections this section got wrong. Guest `%fs`
cannot "stay untouched" — leaving it live means putting the guest FS base in the host's, and every host
signal handler then reads its TLS through it — so `%fs` is declined to the interpreter, not passed through.
And the non-PIE bias fold is not something verbatim copying can carry at all, so a biased image is declined
wholesale rather than transliterated.

Scratch registers need no steal either. x86-64 addressing modes fold most of what the ARM64 side needed
scratch for, and the sequences that genuinely need a temporary are the block-exit and chaining stubs, which
run at block boundaries where all guest state is being spilled to `struct cpu` anyway. The one invariant to
carry over from `stubs.c` is that a spill must never touch the guest red zone at `[sp-128, sp)` — on x86-64
that zone is 128 bytes rather than AArch64's 16 and is used by far more compiled code, so it matters more
here, not less.

## 4. Things that were wrong independently of this work

Naming the host-CPU axis surfaced two defects that were latent on the existing hosts:

- **Persistent-cache identity ignored the host CPU.** `hl_identity_configuration(build, guest_isa,
  host_isa, modes)` takes it as a parameter and DOCS.md documents it as part of the key, but both call
  sites passed the literal `1`. Two hosts sharing a cache directory would have accepted each other's host
  code and executed it. Now passes `HL_HOST_CPU_ISA`, with a static assertion tying the preprocessor
  constants to `hl_host_isa`.
- **`package/linux-aarch64` was a path literal** in three `.cmake` files and two shell scripts. An x86-64
  host would have overwritten the aarch64 artifact under its name, and `tools/refresh_crate_archives.sh`
  would have installed it as the *aarch64* crate asset — where the only thing that would have caught it is
  a link test with the aarch64 compiler one line later. Derived from `HL_HOST_ARCH` now.

Also corrected: the `__APPLE__` arm of `native_context.h` carried no CPU test and defined the AArch64
accessors unconditionally, so an Intel Mac compiled `__ss.__pc` against a register file with no such
member. Intel macOS is not a supported host and this does not make it one; the arm exists so the matrix is
total and the failure is a diagnostic instead of a miscompile.

## 5. The three axes, named

DOCS.md section 1 lists guest OS, guest ISA, and host platform. "Host platform" is two axes, and only the
first had a name. Both now do, once per language:

| axis | C | CMake | nix |
|---|---|---|---|
| host OS | `src/host/<os>/`, `__APPLE__`/`__linux__` | `CMAKE_SYSTEM_NAME` | `hostBackends` |
| host CPU | `HL_HOST_CPU_*` (`src/host/host_cpu.h`) | `HL_HOST_ARCH` | `hostCPUs` |
| guest ISA | `HL_GUEST_ISA_*` | per-lane `hl_linux_production()` | `guestISAs` |

Use `HL_HOST_CPU_*` rather than the compiler predefines. They are spelled differently per compiler
(`__aarch64__` vs `_M_ARM64`), and a bare `defined(__x86_64__)` says nothing about which OS's context
layout and calling convention apply — which is exactly how an Apple-shaped `uc_mcontext->__ss.__rip` came
to sit under a plain `#elif defined(__x86_64__)` in `linux_abi/signal.c`, unable to compile against a
Linux `ucontext_t`.

A guest ISA equal to the host CPU permits same-ISA transliteration. It is never a reason to conflate the
two: `HL_GUEST_ISA_X86_64` and `HL_HOST_CPU_X86_64` are independent facts, and `src/core/target/dual.c`'s
comment calling `hl_aarch64_run_linux_guest` the *"native AArch64 default"* is the kind of elision that
made this change bigger than it needed to be.

## 6. Status

Both guest ISAs execute on an x86-64 Linux host. The compat corpus scores **2993/3013 = 99.34%** across
1580 cases and both guest ISAs, measured per case with pinned binaries and nothing sampled — 20 of the 24
matrix suites fully green on both. `unit` is green, `package` passes all seven steps, and the aarch64 cross
build is at zero errors, which is the check that keeps the shipped host honest: CI's aarch64 runners never
compile the x86_64 arms, so this is the inverse of that gap.

Engine-in-engine is a gate rather than a habit (`nested.*`, five cells). The acceptance criterion — an
aarch64 engine hosting an x86-64 engine hosting a guest — passes, and so does a three-engine chain that
puts an aarch64 host through engine-in-engine on a machine with no aarch64 hardware.

`qemu-aarch64` is available here, so the AArch64 host arm is no longer unexecutable on this box; see
`docs/emulated-aarch64.md` for exactly what emulation vouches for and what it does not. That lane is
registry-only until the defects it found are fixed and qemu is in the devShell.

What remains is in `docs/amd64-host-findings.md` §3.12 onward: a short list of named defects, each with a
reproducer, plus the Stage 2 transliterator of §3.1 above. Performance is measured, not guessed —
`docs/amd64-host-performance.md` — and the single largest cost is a host syscall per guest basic block,
not the interpretation itself.
