# Structural proposals from the amd64-host work

What to **move, rename, split or delete** so this tree is easier to navigate and harder to get wrong —
derived entirely from what adding an x86-64 Linux host actually hit. `docs/amd64-host.md` is the design,
`docs/amd64-host-findings.md` is the debris list, and this file is the "so what should change" that follows
from them. Every claim below was re-checked against the tree and, where a number is quoted, against built
artifacts in `build-amd64/` and `build-arm-check/`. Line numbers are as of the branch tip on 2026-07-26 and
will drift — the file and symbol names are the durable part.

Two rules shape every proposal:

1. **A compile error beats a link error, a link error beats a runtime abort, a runtime abort beats silence.**
   Where a choice existed between documenting an invariant and making its violation fail, the proposal makes
   it fail.
2. **The aarch64 hosts are the shipped, corpus-green ones.** No proposal may make their path harder to
   verify. Every item states its verification, and the one-minute cross-compile is the floor:

```sh
nix develop --command bash -c '
  cmake -G Ninja -B build-arm-check -DHL_BUILD_TESTS=OFF \
        -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/aarch64-linux.cmake &&
  ninja -C build-arm-check -k 0'
```

**Checkpoint-format warning, stated once and repeated per item where it applies.** Anything that changes
`struct cpu`'s layout, the numeric value of an `OFF_*` constant, or `G_CKPT_ARCH` changes the checkpoint
image format: `sizeof(struct cpu)` is written into the manifest and validated on restore
(`src/linux_abi/checkpoint.c:1980, 2250, 3580`), and `G_CKPT_ARCH` is compared at three sites. **No proposal
in this document changes any of them.** Several proposals *read* those constants in new places; that is
deliberate and is the point.

---

## 1. Shortlist — the five best clarity-per-risk changes

| # | Change | Risk | When | Effort | Detail |
|---|---|---|---|---|---|
| **S1** | Close the `lower/*.c` link landmine for real: split the host-neutral `rep` runtime out of `lower/repstr.c`, gate `lower/*.c` out of `IR_SOURCES` on a non-AArch64 host, put an `#error` in `lower/primitives.h`, delete the 21 aborting stubs in `guest/x86_64/interp.c`. | Low–med | **In PR** | ~half a day | §3 P1 |
| **S2** | Rewrite `docs/arch.md` §4 "Seams that gate a new host platform". All four listed seams are the wrong ones. | None | **In PR** | 1 h | §3 P5 |
| **S3** | Make the guest-ISA and host-CPU parameters of `hl_identity_configuration` non-interchangeable types, so defect 1.1 (High: one host executing another host's machine code) cannot recur silently. | Low | **In PR** | 1–2 h | §3 P3 |
| **S4** | Derive the four copies of the `run_block`/`block_return` assembly's `struct cpu` offsets from the `OFF_*` macros by preprocessor stringification, instead of hand-written decimal literals. | Low, byte-verifiable | **In PR** | 2–3 h | §3 P2 |
| **S5** | One shared header for the non-PIE address-materialisation rule, `static inline`, included by both the JIT and the interpreter, replacing two byte-identical private copies of `call_return_pc`. | Low | **In PR** | 2 h | §3 P4 |

(The `P` numbers in §3 are stable identifiers for handing an item to an implementer; they are not a priority
order. The priority order is this table, then §4.)

## 1.1 What I would explicitly **not** do

A proposal to leave something alone is worth as much as a change. Each of these looks like an obvious
cleanup and is not.

**N1 — Do not delete, move or rename `src/translator/{ir,codegen.c,host/}` or `include/hl/{ir,codegen}.h`.**
The temptation is strong: `hl_codegen_block`/`hl_codegen_function` have no caller in `src/` (verified: the
only callers in the tree are eight sites in `tests/unit/test_codegen.c`), and the symmetric
`host/{aarch64,x86_64}/codegen.c` pair is the single most misleading thing in the layout for someone adding
a host CPU. But:

- `include/hl/codegen.h` and `include/hl/ir.h` are `HL_API` and **are installed** — verified:
  `cmake/Phase4Install.cmake:138` is `file(GLOB HL_PUBLIC_HEADERS ${CMAKE_SOURCE_DIR}/include/hl/*.h)`, which
  removes only `activation.h`, and `hl-engine.pc` publishes `-lhl-translator`. Removal is an ABI decision
  taken by a human, not a structural cleanup.
- The neighbouring `src/translator/reloc.c` **is** live — `hl_reloc_slide` serves the persistent cache from
  `guest/*/cache.c` — and it hardcodes AArch64 `MOVZ`/`MOVK` while sitting in the same layer. Moving the
  dead half would strand the live half in a directory whose name no longer describes it.
- `unit.codegen` is now the only thing in the tree that *executes* x86-64 host bytes, and it did so for the
  first time on the amd64 lane (findings §3.1). That is real, newly-realised value.

The fix that was needed here was **documentation, and it has already landed**: DOCS.md §3.3 now states which
of the two paths executes guests, and `src/translator/host/x86_64/README.md` was rewritten from "Reserved
for the x86-64 host-code backend" (accurate for about six hours in July 2026) to a plain statement that it
is a lowerer for `ir.h` and not the backend a new host needs. Both are current. Leave the code where it is.

**N2 — Do not split `src/core/target/{aarch64,x86_64}.c`.** They are 1332 and 1216 lines each and
`#include` roughly thirty `.c` files in a load-bearing order. They are unity translation units *by design*:
`src/core/target/namespace.h` exists precisely because two of them are linked into one binary for the dual
activation archive, and their file-static globals (`g_nonpie_lo`, `g_target_services`, the container state,
the JIT cache statics) are TU-local exactly so the two backends can coexist. Splitting them converts a set
of file statics into a cross-TU ABI, on the shipped, corpus-green path, with no test that would catch a
subtle change in initialisation order. It is the single riskiest change available in this tree. See P11 for
what *is* worth doing about navigability.

**N3 — Do not renumber `G_CKPT_ARCH` to agree with `HL_GUEST_ISA_*`.** They disagree today and it looks like
a bug: `HL_GUEST_ISA_AARCH64 = 1, HL_GUEST_ISA_X86_64 = 2` (`include/hl/engine.h:21`) but
`G_CKPT_ARCH` is `1` in `src/core/target/x86_64.c:683` and `2` in `src/core/target/aarch64.c:340` — inverted.
It is not a bug, it is a wire format: the value is written into every checkpoint manifest and CPU header and
compared on restore. Changing it invalidates every existing image. Make it *loud* instead (P6), never
"consistent".

**N4 — Do not try to merge `HL_HOST_ISA_*` away.** It is genuinely redundant with `HL_HOST_CPU_*` in the
sense findings §5.3 describes, but it is a published enum in `include/hl/codegen.h`, and
`src/translator/identity.c:17-18` already pins the two numberings together with `_Static_assert`. The
remaining hazard is not the duplication, it is that a guest-ISA value and a host-ISA value are both bare
`uint32_t` with identical domains — which is what produced defect 1.1. **P3 fixes the hazard without
touching the published enum.** That is the better trade.

**N5 — Do not regenerate `tests/compat/isa/x86_64/expected/isa-regress.out` in this PR.** The golden
disagrees with real x86-64 hardware on 107 lines, all two-NaN selection or NaN sign (findings §3.9). It is
the golden that is wrong. But regenerating it exposes a genuine two-NaN-selection gap in the shipped AArch64
JIT and turns `compat.isa-x86-64` red on the corpus-green hosts. That is a deliberate, separately-scoped
piece of work with its own fix attached, not a side effect of a host port.

**N6 — Do not fix findings 2.1, 2.2, 2.3 or 2.6 here.** All four are pre-existing, host-independent, and
change behaviour on a page size or a security posture this branch cannot test. They were left out on purpose
and should stay out.

**N7 — Do not specialise `struct cpu` per backend.** The interpreter carries `host_save[12]` and
`host_v[16]`, AArch64 callee-saved slots it never uses, and deleting them is the obvious win. It is the
checkpoint format (§0 warning). One layout is what lets a JIT-captured image restore into an interpreter and
back. The waste is the price and it is the right trade (`docs/amd64-host.md` §2 argues this; agreed).

**N8 — Do not rename `src/host/<os>/` to encode the CPU.** That layer really is host-CPU-neutral —
`src/host/linux/` compiled and passed its provider tests on x86-64 unchanged. The OS/CPU split introduced by
this branch (`HL_HOST_CPU_*` / `HL_HOST_ARCH` / `hostCPUs`) is correct and the directory tree should keep
naming only the OS.

---

## 2. What the evidence actually says

Three things surfaced that are stronger than the individual defects, and they drive most of §3.

### 2.1 `libhl-translator.a` is not a library

`add_library(hl-translator STATIC ${IR_SOURCES})` (`CMakeLists.txt:181`) and it is **installed and
published**: `HL_INSTALL_LIBS` includes it and `hl-engine.pc` emits `-lhl-translator`
(`cmake/Phase4Install.cmake:74,115`). Running `nm` over the built archive on **both** hosts:

| host | archive members with engine-internal unresolved references |
|---|---|
| `build-arm-check` (aarch64) | 9 |
| `build-amd64` (x86-64) | 9 |

**89 engine-internal symbols are undefined in the shipped archive on the aarch64 host too.** Breakdown:

- **84** are ARM64 host emitters (`e_ldr`, `e_rrr`, `emit_exit_const`, `hl_x86_emit_spill`, `emit32`, …)
  declared in `src/translator/guest/x86_64/lower/primitives.h` and **defined only inside the production
  unity TU** via `#include ".../guest/x86_64/emit.c"`. Carried by
  `lower/{alu,crypto,mov,repstr,shift,sse4x,trace,x87}.c` — 8 of the 9 `lower/*.c` files.
- **4** are `hl_logical_vma_*`, i.e. **`libhl-linux-abi` symbols referenced from `libhl-translator`**:
  `hl_logical_vma_resolve_exec` from `src/translator/guest_fetch.c:30,51` and
  `hl_logical_vma_{global_active,pin_data,unpin}` from `src/translator/guest/x86_64/lower/repstr.c:71-175`.
- **1** is `hl_x86_guest_pointer`, defined in `src/core/target/x86_64.c:134`.

Two consequences the findings did not state:

- This is **not** an amd64 problem. It has always been true. What amd64 changed is only *which* members the
  linker was made to demand — `engine_global_init` calls `hl_x86_rep_set_store_commit()` and
  `hl_x86_rep_set_access_validators()`, two host-neutral runtime setters that happen to share
  `lower/repstr.c` with an ARM64 emitter, which pulls the object in and fails the link on 21 undefined
  symbols (findings §3.3). Archive-member-on-demand had been hiding an 89-symbol hole on every host.
- The 4 `hl_logical_vma_*` references contradict **DOCS.md §3**'s dependency diagram, which shows translator
  and Linux ABI as siblings with no arrow between them and says "Only arrows shown above are allowed". They
  resolve at link time only because `hl-engine.pc` happens to list `-lhl-translator -lhl-linux-abi` in that
  order.

### 2.2 The `struct cpu` three-way ABI has two enforced legs and one unenforced one

`src/translator/guest/aarch64/cpu.h:178-180` states the contract itself:

> the baked numeric `OFF_*` above are duplicated into emitted machine code **AND the
> `run_block`/`block_return` asm**. A struct edit that shifts any of them must fail the BUILD, not corrupt a
> guest at runtime — so assert each baked offset against the real field.

Eleven `_Static_assert`s follow. They check leg 1 (struct ↔ `OFF_*`). Leg 3 (the checkpoint image) is
checked at restore. **Leg 2 — the assembly — is not checked at all.** The four trampoline copies
(`src/core/dispatch.c:130-175`, GCC-asm and clang-naked arms; `src/translator/guest/x86_64/translate.c:6279-6323`,
same two arms) contain 20 hand-written decimal offsets each. A struct edit that shifts a field *and*
correctly updates `OFF_HSAVE` passes every assertion and silently corrupts the host register image. That is
the "compile error beats silence" gap in the highest-consequence file in the tree.

### 2.3 The seam a newcomer follows points at the wrong place

`docs/arch.md` §4 lists four "seams that gate a new host platform": host selection (`target/native.h`,
`activation.c`), compat shims (`native_context.h`, `native_compat.h`), legacy ISA routing (`dual.c`), and
the build. Measured against what the work actually took:

| arch.md §4 seam | what happened on x86-64 |
|---|---|
| Host selection | Passed unchanged. It is an **OS**-only test. |
| Compat shims | `native_compat.h` is entirely CPU-neutral. `native_context.h` did need work — but as one of the smallest items, and it is listed for the wrong reason. |
| Legacy ISA routing (`dual.c`) | Real but marginal — a legacy untyped entry point with no test coverage. |
| Build | Real, and understated: it is three languages' worth of a whole new axis. |

None of the four things that actually gated the host appears: the `run_block`/`block_return` trampolines,
the register model, `ibtc_publish`'s 16-byte atomic store, and the fact that **neither guest frontend has a
non-ARM64 back end**. A document that misdirects on its one navigational section is worse than an absent
one.

---

## 3. Proposals

Each is: **what** (file and symbol level) · **why** (with the finding it comes from) · **risk and
verification** · **when** · **effort**.

---

### P1 — Close the `lower/*.c` link landmine (shortlist S1)

**What.** Four steps, in this order:

1. **Split `src/translator/guest/x86_64/lower/repstr.c`.** Move to a new
   `src/translator/guest/x86_64/rep_runtime.c` (note: **out of `lower/`** — the directory should mean "ARM64
   lowering") the host-neutral runtime half, which is a clean cut with no shared state to untangle:
   - the five file statics `g_rep_store_commit`, `g_rep_store_observation_active`, `g_rep_readable`,
     `g_rep_writable`, `g_rep_access_special` (lines 11-15);
   - `hl_x86_rep_set_store_commit`, `hl_x86_rep_set_access_validators` (17-27);
   - `static rep_fault` (29-38);
   - `hl_x86_rep_movs` (58-152) and `hl_x86_rep_stos` (153-222).

   *Verified clean:* the emit half (`emit_rep_string`, `hl_x86_lower_repstr`, lines 223-416) touches none of
   those five statics, and the `#include "../../../../linux_abi/logical_vma.h"` dependency belongs entirely
   to the runtime half — so `lower/repstr.c` loses its Linux-ABI include as a side effect.

2. **Split `lower/repstr.h` correspondingly** into `rep_runtime.h` (the three typedef groups, the two
   setters, the two bulk helpers) and a slimmed `lower/repstr.h` (`enum hl_x86_direction`,
   `hl_x86_repstr_state`, `hl_x86_lower_repstr`). `rep_runtime.c` needs one declaration currently living in
   `lower/primitives.h`: `uint64_t hl_x86_guest_pointer(uint64_t)` — put it in `rep_runtime.h` or in
   `guest/x86_64/cpu.h`. It is defined in `src/core/target/x86_64.c:134` and is already host-neutral, so
   nothing else moves.

3. **Gate the lowering objects out of the archive on a non-AArch64 host.** In `CMakeLists.txt`, wrap the
   nine `src/translator/guest/x86_64/lower/*.c` entries of `IR_SOURCES` (lines 129-137) in
   `if(HL_HOST_ARCH STREQUAL aarch64)`, and add `src/translator/guest/x86_64/rep_runtime.c` unconditionally.

4. **Make the assumption a compile error.** At the top of `src/translator/guest/x86_64/lower/primitives.h`,
   after `#include "../decoder.h()"`:

   ```c
   #include "../../../../host/host_cpu.h"
   #if !defined(HL_HOST_CPU_AARCH64)
   #error "lower/primitives.h declares the ARM64 host emitters defined by guest/x86_64/emit.c inside the \
   production unity TU. Including it means this TU emits ARM64. It must not be compiled on a non-AArch64 \
   host -- see docs/amd64-host-findings.md section 3.3."
   #endif
   ```

5. **Delete the emitter-stub section** at the bottom of `src/translator/guest/x86_64/interp.c` — roughly
   lines 4725-4815: the explanatory banner, `interp_no_emitter`, the `INTERP_EMITTER_STUB` macro and its 19
   expansions, plus the two hand-written trailers `emit_soft_memory_active` (returns 0) and
   `hl_x86_emit_cursor` (aborts). Also drop the forward references to the section in the file header comment
   (~lines 78-90).

**Why.** Findings §3.3, the highest-priority item in that document still worked around rather than fixed.
The concrete confusion: a **host-neutral runtime configuration hook** and an **ARM64 emitter** share an
object file, so calling the hook drags the emitter in. The fix in the tree today is 21 abort-only function
bodies that are dead by construction. Step 4 is what stops it recurring: today a mistake of this shape
manifests as N undefined symbols at final link, several layers from the file that caused it; after step 4 it
is one `#error` naming the exact file and the exact reason. Bonus: with the bulk helpers in their own TU the
interpreter can finally call `hl_x86_rep_movs`/`hl_x86_rep_stos` for `rep movs`/`rep stos` — a real speedup
it currently has to decline (findings §3.3, last line).

**Risk and verification.** Medium-low. The aarch64 build must be unaffected:

- one-minute cross-compile (above) must build 235/235;
- `nm build-arm-check/libhl-translator.a` — the set of *defined* symbols must be unchanged except that
  `hl_x86_rep_*` move from `repstr.c.o` to `rep_runtime.c.o`;
- `objdump -d` on the production aarch64 engine's `hl_x86_lower_repstr` and `emit_rep_string` must be
  byte-identical before and after (the code moved is data-flow-disjoint, so this should hold exactly);
- `ctest -L compat-syscall` and `production.full-x86_64.*` on an aarch64 host — `rep movs`/`rep stos` is the
  memcpy/memset of every musl and glibc x86-64 guest, so essentially the whole corpus exercises it.

On the amd64 host the win is directly observable: the build links without the stub section.

**When.** **Safe inside this PR.** It removes debris the PR itself added and touches no aarch64 logic.
Coordinate with whoever is editing `src/translator/guest/x86_64/` — this moves ~200 lines between files.

**Effort.** Half a day including verification.

**Checkpoint format.** Unaffected.

---

### P2 — Derive the trampoline offsets from `OFF_*` (shortlist S4)

**What.** In all four copies of the host-entry trampolines:

- `src/core/dispatch.c:130-144` (GCC file-scope `__asm__`) and `:145-175` (clang `__attribute__((naked))`),
  which serve the aarch64 guest; offsets `#280` (`OFF_HSP`), `#288..#376` (`OFF_HSAVE + 8k`), `#896..#1008`
  (`OFF_HOSTV + 16k`);
- `src/translator/guest/x86_64/translate.c:6279-6298` and `:6300-6323`, which serve the x86-64 guest;
  offsets `#168` (`OFF_HSP`), `#176..#264` (`OFF_HSAVE + 8k`), `#272..#384` (`OFF_HOSTV + 16k`), plus
  `#248` (`OFF_HSAVE + 72`, the host `x28` slot).

replace every decimal literal with a stringified macro. Add to each `cpu.h` beside the `OFF_*` block:

```c
#define HL_STR_(x) #x
#define HL_STR(x) HL_STR_(x)
```

and write the asm as, e.g., `"str x19,[x0,#" HL_STR(OFF_HSAVE) "]\n"` and
`"str x20,[x0,#" HL_STR(OFF_HSAVE + 8) "]\n"`. The `OFF_*` involved are all plain integer-literal macros
(`aarch64/cpu.h:166-177`, `x86_64/cpu.h:165-176`), so the double-expansion yields `288` and `288+8`
respectively, and GNU `as` accepts an expression in an AArch64 immediate field.

**Why.** §2.2 above. `cpu.h` already *claims* this leg is enforced — "the baked numeric `OFF_*` above are
duplicated into emitted machine code AND the `run_block`/`block_return` asm. A struct edit … must fail the
BUILD" — and it is the one leg of the three-way ABI that is not. It is also the file findings §6 names as a
merge hot spot and the one findings §4 identifies as the seam a new host actually hits. Secondary gain: the
GCC and clang arms of each pair currently have to agree with each other character-for-character
(`dispatch.c:118-121` says so explicitly); after this they agree by construction.

**Risk and verification.** Low, and it is one of the few changes with an *exact* verification:

```sh
objdump -d --disassemble=run_block   <engine>   # before vs after
objdump -d --disassemble=block_return <engine>
```

must be byte-identical for both production engines on aarch64. Anything else is a bug in the change. Follow
with `ctest -L production` on an aarch64 host — every guest instruction executed goes through these two
symbols, so any error is immediate and total rather than subtle.

Note the clang arm cannot be verified by cross-compile (the toolchain is GCC); it must be checked by
building with clang or, at minimum, by an assembler-only syntax check.

**When.** **Safe inside this PR**, but land it *after* P1 so a byte-comparison failure has one cause.
Alternatively defer to a follow-up if the reviewer prefers the PR to touch no trampoline byte at all — the
argument for doing it now is that this PR already restructured the guard around these blocks (defect 1.6),
so a reviewer is already reading them.

**Effort.** 2-3 hours, mostly verification.

**Checkpoint format.** Unaffected — the offsets' *values* do not change, only where the text comes from.
This is precisely the change that makes a *future* offset change fail loudly instead of corrupting an image.

---

### P3 — Make guest-ISA and host-CPU identity non-interchangeable (shortlist S3)

**What.** In `src/translator/identity.h`, replace

```c
uint64_t hl_identity_configuration(uint64_t build, uint32_t guest_isa, uint32_t host_isa, uint64_t modes);
```

with wrapper structs that cannot be passed for one another:

```c
typedef struct { uint32_t v; } hl_identity_guest_isa;
typedef struct { uint32_t v; } hl_identity_host_cpu;
#define HL_IDENTITY_GUEST(x) ((hl_identity_guest_isa){(uint32_t)(x)})
#define HL_IDENTITY_HOST(x)  ((hl_identity_host_cpu){(uint32_t)(x)})
uint64_t hl_identity_configuration(uint64_t build, hl_identity_guest_isa, hl_identity_host_cpu, uint64_t modes);
```

Five call sites to update, all internal: `src/translator/guest/x86_64/cache.c:163`,
`src/translator/guest/aarch64/cache.c:268`, `src/translator/guest/aarch64/interp.c:5910`,
`src/translator/guest/x86_64/interp.c:4694`, and `tests/unit/test_identity.c:68-72`. While there, replace
the bare literals `1` / `2` at the guest-ISA argument with `HL_GUEST_ISA_AARCH64` / `HL_GUEST_ISA_X86_64` —
`guest/aarch64/interp.c:5910` currently passes `HL_HOST_CPU_ISA_AARCH64` in the **guest** slot, which is
numerically right and semantically wrong.

**Why.** Defect 1.1 was the highest-severity finding in the entire document — two hosts sharing a cache
directory would each have accepted and executed the other's host machine code — and its proximate cause was
that the function takes two adjacent `uint32_t` parameters with **identical value domains**
(`HL_GUEST_ISA_AARCH64 = 1, HL_GUEST_ISA_X86_64 = 2` in `include/hl/engine.h:21`;
`HL_HOST_ISA_AARCH64 = 1, HL_HOST_ISA_X86_64 = 2` in `include/hl/codegen.h:10`). Nothing in the language, the
compiler or the test suite distinguishes a swap, a copy-paste, or a hardcoded `1`. This is findings §5.3's
"three ISA vocabularies are easy to confuse" reduced to the one place where confusing them is catastrophic,
and it is the alternative to the ABI-breaking cure rejected in N4. `identity.h` is internal — it is not in
`include/hl/` and is not installed — so this costs no published surface.

**Risk and verification.** Low. It is a compile-error-or-nothing change: if it builds, every call site was
updated. `ctest -L unit` (`unit.identity` covers the function directly), plus a warm-cache check — build,
run a fixture twice, confirm the second run hits the persistent cache (`HL_PCACHE=1`). Cross-compile for
the aarch64 arm of `interp.c`.

**When.** **Safe inside this PR.** It is the durable half of a defect this PR already fixed.

**Effort.** 1-2 hours.

**Checkpoint format.** Unaffected. It *does* participate in persistent-cache identity, but the computed
value is unchanged — the arguments are the same numbers, just typed.

---

### P4 — One shared non-PIE address-materialisation rule (shortlist S5)

**What.** Create `src/translator/guest/x86_64/nonpie.h` containing the rule as `static inline`, and include
it from both backends:

```c
/* Non-PIE pointer identity. A biased ET_EXEC executes at link_pc + bias, but every address the image
 * MATERIALISES (a pushed CALL return address, a rip-relative LEA result) is guest-visible architectural
 * state and must carry the LINK value; rip-relative ACCESSES stay biased. Getting this backwards breaks
 * memory access instead of pointer comparison. Every backend owes both halves.
 * See docs/amd64-host-findings.md section 3.11. */
static inline uint64_t hl_x86_nonpie_return_pc(uint64_t pc) { ... }
```

`src/translator/guest/x86_64/translate.c:280-286` (`static uint64_t call_return_pc`) and
`src/translator/guest/x86_64/interp.c:379-385` (`static uint64_t interp_call_return_pc`) are **byte-for-byte
identical bodies** — same four-term guard on `g_nonpie_lo`, `g_nonpie_types_lo`, `g_nonpie_blob_code`, same
range test, same `pc - g_nonpie_bias`. Replace both with a call to the shared inline (keeping the existing
names as one-line wrappers if the call sites are noisy to touch). The interpreter's comment even says
"Byte-for-byte the JIT's `call_return_pc` (translate.c), because the two backends must push the same value
into guest memory" — make that structural instead of aspirational.

Do the same for the data half where it is already shared in spirit: `hl_x86_guest_pointer`
(`src/core/target/x86_64.c:134`) is the precise range check and the interpreter uses it; the JIT's
`emit_bias` in `address.c` uses a *different* predicate (findings §3.4, third bullet — "worth a decision").
At minimum, cross-reference the two from `nonpie.h` so the next backend author sees both in one place.

**Why.** Findings §3.11 — the sharpest bug on the branch. The x86-64 interpreter shipped without the
`LEA` rewrite and the symptom was glibc's `__malloc_fork_lock_parent` deadlocking on a mutex it held itself,
because `lea main_arena(%rip),%r12` produced HIGH and the baked `main_arena.next` was LOW. Zero CPU, no
output, three whole compat suites. Findings state it plainly: "A new backend must implement both, and there
is no test that fails loudly if it implements neither." A shared header does not make it a compile error —
nothing can, short of a per-backend conformance test — but it moves the obligation from "two private
functions in files you will not both read" to "one header with the rule written on it", and the same-ISA
x86-64 transliterator is the next thing to be written.

**Risk and verification.** Low. The two bodies are provably identical (`diff` the extracted functions).
Verification: `objdump` on the aarch64 production x86-64 engine — `call_return_pc` inlines into
`translate_block` today and should continue to; the emitted guest code must be unchanged. Then
`ctest -L compat` on an aarch64 host, with particular attention to any non-PIE fixture — and note that
**every fixture in the corpus is built `-static`, i.e. non-PIE**, so coverage here is total.

**When.** **Safe inside this PR.**

**Effort.** 2 hours.

**Checkpoint format.** Unaffected.

---

### P5 — Rewrite `docs/arch.md` §4 (shortlist S2)

**What.** Replace §4 "Seams that gate a new host platform" wholesale. The replacement should say, in this
order:

1. **The host-service layer is not a seam for a new host CPU.** `src/host/<os>/` is host-CPU-neutral and
   compiled on x86-64 unchanged. It *is* the seam for a new host OS. Say which question the reader is
   asking first — DOCS.md §11 already splits the two checklists correctly; arch.md should point at it rather
   than contradict it.
2. **The four seams that actually gate a new host CPU**, with file and symbol:
   - `run_block` / `block_return` — `src/core/dispatch.c:122-228` and
     `src/translator/guest/x86_64/translate.c:6270-6350`. Hand-written ARM64 in both, four copies, and the
     `#else` arm exists only to abort with a diagnostic.
   - **The register model.** `src/core/target/x86_64.c`'s header states it (guest `rax..r15` in host
     `x0..x15`, `cpu` pinned in `x28`); `guest/aarch64/translate.c` is a same-ISA transliterator that copies
     guest instruction words verbatim. Neither has a non-ARM64 back end.
   - `ibtc_publish` — `src/translator/cache.c:665`. A 16-byte single-copy-atomic pair store whose
     correctness is `stp` on AArch64 and `movdqa` on x86-64, with different alignment consequences.
   - **The two per-backend obligations with nothing enforcing them:** non-PIE address materialisation and
     `call_return_pc` (P4, findings §3.11).
3. **`native_context.h` as the OS × CPU matrix it now is**, with the three tiers (host-neutral,
   `HL_HOST_HAS_A64_CONTEXT`, `HL_HOST_HAS_X64_CONTEXT`) named. The current entry lumps it with
   `native_compat.h`, which is CPU-neutral.
4. Keep the build seam, but upgrade it: it is `HL_HOST_ARCH` in CMake, `HL_HOST_CPU_*` in C, `hostCPUs` in
   nix, and `HL_CI_HOSTS`/`HL_CI_COMPAT_HOSTS` in `cmake/CiLanes.cmake` — four tables, and DOCS.md §11 lists
   the order to touch them in.

Also correct §1's ownership list, which says `src/host/windows/` is README-only (true and honest — leave)
but does not mention that `src/translator/host/<cpu>/` is *not* the production lowering path. One sentence
with a pointer to DOCS.md §3.3 is enough.

**Why.** §2.3 above, and findings §4. This is the file a newcomer reads for navigation ("DOCS.md is
normative; this file is a navigation aid") and all four of its listed seams are wrong or marginal. It cost
real time on this task and will cost it again.

**Risk and verification.** None — documentation only. Verification is a human reading it against
`docs/amd64-host.md` §1.

**When.** **Safe inside this PR.** It is the cheapest item on the list and the one that most directly pays
back the next person.

**Effort.** 1 hour.

---

### P6 — Make the deliberate divergences assert themselves

**What.** Three small additions, each of which turns a comment into a check:

1. **`G_CKPT_ARCH`.** Add beside each definition (`src/core/target/x86_64.c:683`,
   `src/core/target/aarch64.c:340`) a static assertion plus one sentence:

   ```c
   #define G_CKPT_ARCH 1 /* NOT HL_GUEST_ISA_X86_64. This is the checkpoint wire format. */
   _Static_assert(G_CKPT_ARCH != HL_GUEST_ISA_X86_64,
                  "G_CKPT_ARCH is the checkpoint image format and is deliberately not the "
                  "HL_GUEST_ISA_* numbering; 'fixing' it invalidates every existing image");
   ```

   (The assertion is unusual in asserting *in*equality — that is the point. It fails if someone
   "harmonises" the numbering, which is exactly the mistake to catch. See N3.)

2. **`ibtc_ent` sizing.** The static assertions added by this branch at `src/translator/cache.c:665`
   already cover it; add a one-line back-reference to findings §1.14 so the *reason* survives.

3. **`namespace.h` completeness.** `src/core/target/namespace.h` is a hand-maintained allowlist of ~40
   symbols renamed so two unity TUs can share a binary, and nothing checks that it is complete. Today the
   failure is a "multiple definition" link error in the dual archive, which is loud but cascades — findings
   §3.7 records how one uncovered symbol pair (`run_block`/`block_return`) cascaded into ~300 undefined
   ARM64-emitter references. Add a `tools/check_namespace.sh` invoked from a `gate.target-namespace` ctest
   entry (the repo idiom: see `tools/check_ci_workflows.sh` → `gate.ci-lane-parity` in
   `cmake/LaneParity.cmake`) that runs `nm --defined-only --extern-only` over both
   `target/{aarch64,x86_64}.c` objects and asserts the intersection is empty, printing the colliding names.
   That converts the cascade into a list.

**Why.** Findings §3.7 (`visibility("hidden")` is not local linkage — a hidden-visibility C definition is
still `STB_GLOBAL`, and the JIT gets away with it only because its trampolines come from a file-scope
`__asm__` block with `.hidden`, which GCC emits as a genuinely local `t` symbol). And N3 above.

**Risk and verification.** Items 1 and 2 are compile-time-only and zero risk. Item 3 is a new test that must
pass on the current tree before it lands — run it against `build-arm-check` and `build-amd64` first; if it
finds a collision today, that is a bug report, not a reason to weaken the gate.

**When.** Items 1-2 **safe inside this PR**. Item 3 **follow-up** (it needs a new tools script and a ctest
entry, both in files another agent may be editing).

**Effort.** 1 h for 1-2; half a day for 3.

**Checkpoint format.** Item 1 explicitly protects it and changes nothing.

---

### P7 — Replace `G_GPC_HASH_SHIFT == 2` with a real guest-ISA predicate

**What.** `src/translator/guest/{aarch64,x86_64}/abi.h` already own a `G_*` vocabulary of honest guest-ISA
facts — `G_SECCOMP_ARCH`, `G_UNAME_MACHINE`, `G_NR`, `G_A0..G_A5`. Add one more to each:

```c
/* aarch64/abi.h */  #define G_GUEST_ISA HL_GUEST_ISA_AARCH64
/* x86_64/abi.h  */  #define G_GUEST_ISA HL_GUEST_ISA_X86_64
```

and change the four sites in `src/linux_abi/signal.c` (`:957`, `:964`, `:1006`, `:1032`) from
`#if G_GPC_HASH_SHIFT == 2` to `#if G_GUEST_ISA == HL_GUEST_ISA_AARCH64`.

*Better still, if the reviewer will take it:* move the register dump itself into `abi.h` as
`G_DIAG_REGS(emit, c)` so `linux_abi/signal.c` carries no per-ISA branch at all. The x86-64 arm currently
prints nothing where the aarch64 arm prints `lr`, `x0`, `x1`, `x20` and disassembles the faulting word —
that asymmetry is worth closing while the code is open, and `abi.h` is where the knowledge belongs.

**Why.** Finding 2.5. `G_GPC_HASH_SHIFT` is a **hash-tuning constant** (`aarch64/abi.h:35`, used by
`src/translator/cache.c:521,523,546` to hash the guest PC) that happens to be 2 for aarch64 and 0 for x86.
Using it as a guest-ISA discriminator means a future decision to retune the block-map hash silently changes
crash diagnostics — including whether `G_PC(c)` gets dereferenced as a `uint32_t *`. It also reads, to a
newcomer, as if the hash shift *were* the ISA, which is exactly the class of elision findings §5.1 is about.

**Risk and verification.** Very low — diagnostics-only, four sites, and the emitted text on aarch64 must be
character-identical. Verify by triggering an `[HLFATAL]` on an aarch64 host (any deliberate guest SIGSEGV
fixture) and diffing the line before and after. Cross-compile covers compilation of both arms.

Note `src/translator/cache.c:1578` and `:1753` also branch on `G_GPC_HASH_SHIFT` — those are **legitimate**
(they gate SMC machinery on whether PCs are fixed-width) and should be left alone; the point is that after
this change every remaining use of the macro is about hashing or instruction width, which is what it means.

**When.** **Safe inside this PR** in its minimal form.

**Effort.** 1 hour minimal; half a day for the `G_DIAG_REGS` version.

---

### P8 — Split `src/linux_abi/x86.c`; rename it and its aarch64 twin

**What.** `src/linux_abi/x86.c` is 1352 lines and is three things: an x86-64 guest ELF loader, guest stack
construction, and a chain of **host-ARM64** fault fixups. Split and rename:

- `src/linux_abi/x86.c` → `src/linux_abi/elf_x86_64.c` (loader + stack), and for symmetry
  `src/linux_abi/elf.c` → `src/linux_abi/elf_aarch64.c`. Today `elf.c` is the aarch64-guest loader and
  `x86.c` the x86-64 one, and only one of them says so.
- Move the host-CPU-gated fixup chain — `nonpie_fixup` (`:810-925`), `lse_align_fixup` (`:970-1035`),
  `ldapr_align_fixup` (`:1068-1100`) and their `HL_HOST_UC_PC(uc) += 4` advances — into
  `src/translator/guest/x86_64/host_aarch64_fixup.c`, a unity-TU fragment `#include`d from the
  `HL_HOST_CPU_AARCH64` arm. Keep the *dispatch* chain (the `if (…fixup(sig, si, uc)) return;` sequence at
  `:1120-1160`, `:1273`, `:1304`) in the fault file, calling into it.

**Why.** Findings §5.2. These functions decode the faulting **host** AArch64 instruction word
(`*(uint32_t *)HL_HOST_UC_PC(uc)`) to emulate the load the JIT emitted, and then advance the host PC by a
fixed 4 bytes. That is (guest ISA = x86-64) × (host CPU = aarch64) code sitting in a file named for the
guest ISA alone, in the layer that DOCS.md §3.4 defines as "the syscall and environment surface a Linux
guest image expects". The `+= 4` is unportable by construction. It is guarded now; findings say plainly "it
should eventually move". Its natural home is beside the emitters whose output it is repairing.

**Risk and verification.** Medium — this is a pure code move on the shipped aarch64 path, and fault handling
is the most delicate machinery in the tree. The move must be byte-preserving:
`objdump -d` on the three functions must be identical before and after, and the *order* of the fixup chain
must be preserved exactly (`ldapr` before `lse` before `nonpie` at `:1120-1157`, and `nonpie_fixup` consulted
FIRST at `:1273` and `:1304` — the comments there explain why, and getting it wrong produces an
mprotect/retry loop rather than a crash). Then `ctest -L compat` and `ctest -L checkpoint` on an aarch64
host in full.

**When.** **Follow-up, after this PR merges.** It touches the two files findings §6 names as the top merge
hot spots (`src/core/target/{aarch64,x86_64}.c` and the fault path), and it has no functional urgency —
the host-CPU guard added by this branch already makes it correct, just misfiled.

**Effort.** 1-2 days with verification.

**Checkpoint format.** Unaffected.

---

### P9 — Name the dispatch seam by backend, not by host CPU

**What.** The seam is currently selected by an `#if` duplicated in both unity TUs
(`src/core/target/x86_64.c:102-107`, and the equivalent in `aarch64.c`):

```c
#if defined(HL_HOST_CPU_AARCH64)
#include ".../guest/x86_64/dispatch.h"
#else
#include ".../guest/x86_64/interp_dispatch.h"
#endif
```

Proposed:

- rename `src/translator/guest/{aarch64,x86_64}/dispatch.h` → `jit_dispatch.h` (it *is* the JIT seam — it
  patches ARM64 branch encodings into the W^X arena and assumes guest registers live in matching host
  registers);
- add `src/translator/guest/<isa>/dispatch_select.h` containing only the (guest ISA, host CPU) → backend
  table for that ISA, so both target TUs include one unconditional header;
- the target TUs then read `#include ".../guest/<isa>/dispatch_select.h"` with no `#if`.

**Why.** Item 7 of the brief, and `docs/amd64-host.md` §2's own observation: "the seam is per
(guest ISA, host CPU), not per guest ISA alone, and that only became visible once there was a second host
CPU." The current *names* encode per (guest ISA, backend kind) and the current *selection* encodes per host
CPU. Those coincide today only because "aarch64 host ⇒ JIT" happens to hold. They stop coinciding the moment
the same-ISA x86-64 transliterator lands (`docs/amd64-host.md` §3): on an x86-64 host the x86-64 guest would
have a JIT and the aarch64 guest an interpreter, and the `#else` arm would be wrong for one of them —
duplicated in two files. Fixing the shape *before* that third arm is written is much cheaper than after.

**Risk and verification.** Low for the mechanical part (rename + one new header), but it touches file names
referenced from ~15 comments across the translator. Verification: cross-compile plus `ctest -L unit`; the
preprocessed output of both target TUs should be identical before and after (`gcc -E` diff), which is an
exact check.

**When.** **Follow-up.** It is a rename across files two other agents are editing right now; the conflict
cost inside this PR outweighs the benefit, and there is no correctness pressure until Stage 2.

**Effort.** Half a day.

---

### P10 — Name the three things inside `IR_SOURCES`

**What.** `IR_SOURCES` (`CMakeLists.txt:120-147`) is one flat list of 32 files that is actually three
unrelated sets, and the variable name describes only the smallest of them. Split into three variables
concatenated at `add_library` — **no file moves, no build change, byte-identical output**:

```cmake
set(TRANSLATOR_CORE_SOURCES   # host-CPU-neutral translator services, all live
  src/translator/arena.c src/translator/digest.c src/translator/guest_fetch.c
  src/translator/identity.c src/translator/persist.c src/translator/reloc.c
  src/translator/window.c)

set(IR_PIPELINE_SOURCES       # ir.h -> host-CPU lowerers. Published API, unit-tested,
                              # and NOT on the production execution path -- DOCS.md 3.3.
  src/translator/codegen.c src/translator/ir/ir.c src/translator/ir/interpreter.c
  src/translator/host/aarch64/asm.c src/translator/host/aarch64/codegen.c
  src/translator/host/x86_64/codegen.c)

set(GUEST_X86_64_SOURCES ...) # the separately-compiled half of the x86-64 frontend
```

with a comment above `IR_PIPELINE_SOURCES` saying in one line what DOCS.md §3.3 says in a paragraph, and a
comment above `GUEST_X86_64_SOURCES` noting that the *other* half of the same frontend
(`emit.c`, `translate.c`, `interp.c`, `cache.c`, `stubs.c`) is `#include`d into the unity TU instead, and
why (§2.1, P12).

**Why.** The comment at `CMakeLists.txt:11` says `libhl-translator.a <- IR_SOURCES (incl. aarch64 host
codegen)` — which is true and describes about a fifth of it. A reader looking for "where is the x86-64
frontend built" finds half of it in a variable called `IR_SOURCES` and the other half `#include`d from
`src/core/target/x86_64.c`, with no statement of which half is which or why. That arbitrary boundary is
precisely what made §3.3 a landmine.

**Risk and verification.** None. Byte-identical archive: `cmp` the `.a` before and after (modulo member
order — compare `nm` output sorted).

**When.** **Safe inside this PR**, though it touches `CMakeLists.txt`, which is being edited concurrently —
coordinate or defer by a day.

**Effort.** 1 hour.

---

### P11 — Give the unity TUs a spine without splitting them

**What.** Two mechanical additions, given N2 (do not split):

1. **An assembly-order banner.** At the top of each of `src/core/target/{aarch64,x86_64}.c`, a numbered list
   of every `#include`d `.c` in order, each with one clause saying what it contributes and — for the ones
   where it matters — what must precede it. The order *is* the contract: `container/state.c` defines the
   container globals before `cache.c` needs them; `g_nonpie_lo` is declared at `:124` before `avx.h` at
   `:125` because `g_avx_state` takes its address; `emit.c` must precede `translate.c` and both must precede
   `../dispatch.c` because `G_OWN_TRAMPOLINES` suppression depends on it. Today a reader recovers this by
   bisecting compile errors.

2. **A "not standalone" guard.** Define `HL_TARGET_UNITY 1` in each target TU before its first fragment
   include, and open every `.c` file that is `#include`d rather than compiled — `guest/*/{emit,translate,
   interp,cache,stubs}.c`, `linux_abi/{thread,signal,fork,elf,x86,checkpoint,sentry}.c`,
   `linux_abi/container/{state,vfs,netns}.c`, `linux_abi/syscall/dispatch.c`, `translator/cache.c`,
   `core/dispatch.c` — with:

   ```c
   #ifndef HL_TARGET_UNITY
   #error "this file is a unity-TU fragment #included by src/core/target/<isa>.c; it is not compiled standalone"
   #endif
   ```

**Why.** Item 10 of the brief. These files are navigable *if* you already know they are unity TUs and know
the order; the guard makes the first fact a compile error rather than folklore, and the banner makes the
second readable in one screen. It also directly prevents the §3.3 class of mistake in the other direction: a
future file added to `IR_SOURCES` that was written as a fragment fails to compile immediately, naming
itself.

**Risk and verification.** Low but broad — ~20 files touched, purely additive. Cross-compile plus
`ctest -L unit`. Guard against the trap that `core/dispatch.c` is included by *both* target TUs and
`translator/cache.c` likewise: the guard is per-inclusion, not per-file, so it is fine.

**When.** **Follow-up.** It touches nearly every file the other two agents are in. Zero urgency; high
conflict cost today, near-zero next week.

**Effort.** Half a day for the guards, half a day for the two banners (the banners require actually reading
the order, which is the work).

---

### P12 — Make `libhl-translator.a` self-contained, then gate it

**What.** Two parts, in order:

1. **Move `lower/*.c` into the unity TU.** After P1, `lower/*.c` are compiled only on an AArch64 host and
   are still resolved against `emit.c` inside the production executable. Complete the job: `#include` all
   nine from `src/core/target/x86_64.c`'s `HL_HOST_CPU_AARCH64` arm, immediately after `emit.c`, and remove
   them from `IR_SOURCES` entirely. This makes P1's CMake gate and `#error` redundant belt-and-braces rather
   than the mechanism, and eliminates 84 of the 89 unresolved references from the shipped archive.
2. **Route the remaining Linux-ABI references through callbacks.** `src/translator/guest_fetch.c` calls
   `hl_logical_vma_resolve_exec` and (post-P1) `rep_runtime.c` calls `hl_logical_vma_{global_active,pin_data,
   unpin}` — translator → Linux ABI, an arrow DOCS.md §3 does not permit. `rep_runtime.c` already has the
   right idiom for this: `hl_x86_rep_set_access_validators` is a setter the target TU calls at
   `src/core/target/x86_64.c:849`. Add the VMA operations to that same callback block (or a sibling
   `hl_x86_rep_set_vma_ops`) and give `guest_fetch.c` an equivalent.
3. **Then add `gate.archive-closure`.** A `tools/check_archive_closure.sh` (repo idiom, cf.
   `tools/check_ci_workflows.sh`) that runs `nm` over each installed static library and asserts every
   undefined symbol is either defined in a sibling installed library, or in an allowlist of libc / libm /
   pthread / compiler-runtime names (`memcpy`, `sqrt`, `__aarch64_swp4_acq`, `__stack_chk_fail`, …).

**Why.** §2.1. `hl-engine.pc` tells an external consumer to link `-lhl-translator`, and on **both** hosts
that archive has 89 engine-internal unresolved references that resolve only because nothing has yet demanded
the members carrying them. A consumer that does demand them gets a link failure against a published,
installed, versioned artifact. The gate is what stops it silently coming back — and it is the reason the
gate must come *after* parts 1 and 2 rather than with them: run against the tree today it fails on aarch64,
which is a true report and not a usable gate.

**Risk and verification.** Part 1 is medium: it grows `src/core/target/x86_64.c`'s preprocessed size by
~2900 lines and could surface file-static name collisions with `emit.c` (`emit_ea`, `rm_load`, `do_alu` are
declared in `primitives.h` and defined in `emit.c` — they are already one TU's worth of names, so this
should be clean, but it must be checked). Verification: the aarch64 production engine's `.text` should be
close to unchanged; `objdump -d` diff on `hl_x86_lower_*` and `translate_block`, then full
`ctest -L production` and `ctest -L compat` on an aarch64 host. Part 2 is a small mechanical indirection with
a measurable cost (an indirect call in the `rep` bulk path) — benchmark `perf-linux` before and after.
Part 3 is a new test, zero runtime risk.

**When.** **Follow-up, and part 1 needs a human decision** — it makes the largest file in the tree larger,
which is in tension with P11's goal. The alternative end state (keep them separately compiled but stop
publishing `libhl-translator`, i.e. fold it into `libhl-engine` for install purposes) is arguably better and
is a packaging decision, not an engineering one. Put the choice in front of a human with both options.

**Effort.** 1-2 days for part 1; half a day for part 2; half a day for part 3.

---

### P13 — `src/core/target/dual.c`

**What.** Three options, in increasing order of what they cost:

- **(a) Comment only, now.** The current comment says `hl_aarch64_run_linux_guest` is "the **native**
  AArch64 default used by the macOS command launcher". Replace "native" — the guest ISA is not native to
  anything, and this is the same elision as `guest/aarch64/abi.h:29`'s "the NATIVE audit arch" (findings §4).
  Say what it is: *the legacy untyped config-file launcher has no guest-ISA field, so it defaults to the
  aarch64 guest for backward compatibility. This is a compatibility default, not a statement about the host.*
- **(b) Name the constant.** `#define HL_LEGACY_DEFAULT_GUEST_ISA HL_GUEST_ISA_AARCH64` with the same
  comment, so the default is a value a reader can grep for rather than a hardcoded call.
- **(c) Fail loudly.** Make the untyped entry refuse rather than guess. This is a behaviour change on a
  legacy entry point with **no test coverage** — findings 2.4's stated reason for leaving it.

**Why.** Findings 2.4 and `docs/amd64-host.md` §5: "the kind of elision that made this change bigger than it
needed to be." The function itself is harmless today; the *comment* is the landmine, because it teaches the
reader that "AArch64 guest" and "native" are the same idea, which is the conflation that inverts on a new
host.

**Risk and verification.** (a) is zero. (b) is compile-time-only. (c) **needs a human decision** — it can
break an existing embedder and nothing in the suite would notice.

**When.** (a) **safe inside this PR**. (b) follow-up. (c) human decision, probably never.

**Effort.** 15 minutes for (a).

---

### P14 — Correct the remaining misleading documentation

**What.** Findings §4 lists seven items. Most have been fixed by this branch; two are worth re-checking as a
batch and one is new:

- `DOCS.md:169` "Lower IR to the selected host CPU", present tense, in the §3.3 responsibilities list. §3.3's
  body now states the truth at length, but the four-item list above it still reads as a description of the
  production path. Reword responsibility 4 to name both paths, or move the list below the "two code paths"
  paragraph so a skimmer cannot read it alone.
- `guest/aarch64/abi.h:29` "the NATIVE audit arch" / "aarch64's ABI numbers are already the native ones" —
  "native" here means "canonical". Say "canonical".
- **New:** `CMakeLists.txt:11`'s summary comment (see P10).

**Why.** Findings §4 opens "These cost real time on this task and will cost it again." The residue is small
and the fixes are minutes each.

**Risk and verification.** None.

**When.** **Safe inside this PR.**

**Effort.** 1 hour.

---

## 4. Sequencing

| | Proposal | When | Effort | Touches shipped aarch64 path? |
|---|---|---|---|---|
| P5 | `docs/arch.md` §4 rewrite | in PR | 1 h | no |
| P14 | Residual misleading docs | in PR | 1 h | no |
| P13(a) | `dual.c` comment | in PR | 15 min | no |
| P10 | Split `IR_SOURCES` into three named lists | in PR | 1 h | no (byte-identical) |
| P3 | Non-interchangeable ISA identity types | in PR | 1-2 h | compile-time only |
| P7 | `G_GUEST_ISA` replaces `G_GPC_HASH_SHIFT == 2` | in PR | 1 h | diagnostics only |
| P6(1,2) | `G_CKPT_ARCH` / `ibtc_ent` assertions | in PR | 1 h | compile-time only |
| P4 | Shared non-PIE rule header | in PR | 2 h | yes — inline, objdump-verified |
| P1 | Close the `lower/*.c` landmine | in PR | ½ day | yes — objdump-verified |
| P2 | Trampoline offsets from `OFF_*` | in PR (after P1) | 2-3 h | yes — objdump-verified, exact |
| P6(3) | `gate.target-namespace` | follow-up | ½ day | no |
| P9 | `jit_dispatch.h` + `dispatch_select.h` | follow-up | ½ day | rename only, `gcc -E` verified |
| P11 | Unity-TU banner + fragment guards | follow-up | 1 day | additive only |
| P8 | Split/rename `linux_abi/x86.c` | follow-up | 1-2 days | yes — code move, high care |
| P12 | Self-contained archive + closure gate | follow-up + **human decision** | 2-3 days | yes |

The in-PR block is roughly two days of work, and every item in it is verified by the same one-minute
cross-compile plus one targeted `objdump` or golden diff. The follow-up block is deliberately everything
that renames or moves a file another agent is currently editing.

## 5. Verification recipes referenced above

```sh
# 1. The one-minute aarch64 arm check. Run before every push. 235/235 targets.
nix develop --command bash -c '
  cmake -G Ninja -B build-arm-check -DHL_BUILD_TESTS=OFF \
        -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/aarch64-linux.cmake &&
  ninja -C build-arm-check -k 0'

# 2. Exact codegen non-regression for P2/P4/P8. Byte-identical is the pass condition.
objdump -d --disassemble=run_block    build-arm-check/<engine> > before.txt   # ... then after
objdump -d --disassemble=block_return build-arm-check/<engine>

# 3. Archive shape for P1/P12. Defined symbols must be unchanged; undefined must shrink.
nm build-arm-check/libhl-translator.a | \
  awk '/:$/{m=$0} /^[0-9a-f]* [A-TV-Z] /{d[$3]=1} /^ *U /{u[$2]=m}
       END{for(s in u) if(!(s in d)) print m, s}' | sort
```

For anything touching guest execution, `ctest -L compat-syscall` is the cheapest broad signal
(`syscall-edges` is 104/104 on both guest ISAs) and `production.smoke-{aarch64,x86_64}` the cheapest
end-to-end one. Note findings §3.10's caution: `tools/matrix_runner.c:1063` short-circuits the second ISA
when the first fails, so a single lane run cannot produce per-ISA numbers — run the two legs separately when
a number matters.
