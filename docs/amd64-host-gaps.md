# x86-64 Linux host: the completion checklist

Everything still missing before `README.md`'s host table can say **Supported** for Linux x86-64.

The bar is `README.md`'s own: *"Supported means the exact-golden compatibility, lifecycle and production
matrices pass on that host for both guest ISAs."* Anything an aarch64 Linux host provides that this host
does not is an item here.

Two neighbouring documents; this one does not repeat either.

- `docs/amd64-host.md` is the **design** — the host-CPU seam, the (host CPU x guest ISA) matrix, the
  interpreter-then-transliterator staging, and section 3.1's settled Stage-2 register model.
- `docs/amd64-host-findings.md` is the **debris list** — 16 defects fixed, 7 found and left, harness
  defects, misleading docs. It looks backwards. This file looks forwards and cross-references it by
  section number (`F-3.3` means that file's section 3.3).

## How the measurements here were taken

Everything below marked *measured* was run on this machine today against `build-amd64` (a native
`nix develop` configure, `HL_MATRIX_TIMEOUT_SCALE=30`, `HL_PERF_ENFORCE=OFF`). Anything not measured
says so. Two properties of this machine produce failures that are **not** engine defects, and both are
called out where they occur:

- the shell runs at **nice 12**, so `setpriority(2)` cannot lower the niceness and any fixture asserting
  a specific nice value fails — *natively as well as through the engine*;
- the repo is on **ext4**, not tmpfs, so the guest `/tmp` the matrix runner synthesises is not tmpfs
  unless `HL_MATRIX_SCRATCH_DIR=/dev/shm` is exported. Every compat run below exported it.

Three kinds of problem are kept apart throughout: **engine defects** (the engine is wrong),
**golden-side problems** (the expected output is wrong), and **environmental artefacts** (this machine
is not the machine the case describes).

---

## Ranked top 10

Ordered by (blocks-Supported x cases-unblocked) / effort. Every entry names the file, lane or encoding.

| # | Do this | Unblocks | Effort | Decision? |
|---|---|---|---|---|
| 1 | **Split the host-neutral runtime halves out of `src/translator/guest/x86_64/lower/*.c`** (F-3.3). `libhl-engine-activation.a` currently carries **92 undefined ARM64-emitter symbols** on this host. | `package.consumer-link` (labels `package`, `package-activation`, `package-embedded`) which fails at step 4 today, and `dual-backend.link` (label `embedding`) whose binary is **never linked at all**. Also deletes the 21 aborting stubs in `interp.c` and lets the interpreter use the bulk `rep movs`/`rep stos` helpers. | 1-2 days | no |
| 2 | **Split `HL_CI_SHARDED_LINUX` per host CPU, teach I19/I20 the split, declare `Linux-x86_64` in `HL_CI_COMPAT_HOSTS`, then shard `compat-ipc`, `compat-syscall-edges` and `compat-time`.** In that order — declaring the token first switches I20 off. | The first CI gating this host has ever had: **430** (case, guest-ISA) runs. All three suites **measured green on both guest ISAs today** — ipc 124/124 per ISA, edges 52/52 per ISA (5.4 s), time 39/39 per ISA (183 s). `docs/amd64-host-findings.md` §3.10 still records ipc as green on the x86-64 guest only; that is now stale. | 1 day | no |
| 3 | **Implement the `mov r,imm` V8-blob rebase in the x86-64 interpreter** — `interp.c` declares `g_nonpie_blob_code` and uses it only in `interp_call_return_pc`; the JIT's `lower/mov.c` rebases the `mov`-imm materialisation and the interpreter does not. | `core/regress/nonpie-v8blob`, the **only** failing case in `compat-core-regress` (aarch64 11/11, x86_64 9/10). Makes a third suite gateable. | hours | no |
| 4 | **Make the aarch64 interpreter SIGILL on an EL1 ID-register `MRS`.** `interp.c`'s EL1 ID-register arm writes 0 into Rt; the JIT's matching arm in `translate.c` does `emit32(0)`, and `0x00000000` is `udf #0` — it **traps**. Both arms use the same mask, so the comment claiming they "deny the same set" is half right and entirely misleading. | `completeness/cpu_discovery`. More importantly it closes a silent cross-backend divergence in guest CPU feature discovery: an ifunc resolver probing ID registers takes a different path on the two backends. | hours | no |
| 5 | **Five missing x86-64 encodings, all reachable by a real guest.** `XLATB` (`D7`); `MOV r/m16,Sreg` (`8C`)/`MOV Sreg,r/m16` (`8E`); `MASKMOVDQU` (`66 0F F7`); the MMX register file (`0F 6F`/`0F 7F`/`0F FC`/`0F 70`, plus `CVTPI2P*`/`CVTP*2PI`); `IRETQ` (`48 CF`). The JIT implements every one. | `abi-corpus/x_xlat`, `abi/iretq-context`, `completeness/{movseg,ntload,mmx-width}` — 5 cases across 3 suites. | XLATB/Sreg/MASKMOVDQU hours each; IRETQ ~1 day; MMX 1-2 days | no |
| 6 | **Diagnose `signals/sigurg-go-preempt`** (aarch64 guest only). It is the sole failing case in `compat-signals` — measured today at aarch64 66/67, x86_64 64/64 — and it is also *slow*: run alone it had not finished after 120 s, where the whole 131-run suite otherwise completes. | `compat-signals`, 131 runs, becomes the fourth gateable lane. | hours-1 day | no |
| 7 | **Regenerate `tests/compat/isa/x86_64/expected/isa-regress.out` on real x86-64 hardware** (F-3.9). 107 golden lines encode ARM NaN semantics; the interpreter is byte-identical to running the fixture natively. | `compat-isa-x86-64`, which is **unreachable by construction** on any x86-64 host until this happens. | hours to regenerate; unknown to fix the AArch64 JIT gap it exposes | **yes** — needs the hardware and the decision to move a committed golden |
| 8 | **Diagnose `completeness/sse4x` and `completeness/xflags-sig`.** `sse4x` diverges at output byte 241 (`9` vs `8`) — an arithmetic result, not an unimplemented encoding. `xflags-sig` exits 139 with **zero** stdout. Both x86-64 guest only. | 2 cases in `compat-completeness`; both are the "silently wrong result" class F-3.6 is about. | hours-1 day each | no |
| 9 | **Diagnose `completeness/pf-maps-relro`** — `has_ro=0`, expected 1, on **both** guest ISAs, so it is host-neutral and should be reproducible on an aarch64 host. `/proc/<pid>/maps` is not reporting the RELRO read-only region. | 1 case x 2 ISAs, and a `/proc` fidelity defect that is probably not amd64-specific at all. | hours-1 day | no |
| 10 | **Give `isa-fuzz.x86_64-regress` a native oracle on an x86-64 host.** `tests/fuzz/isa/x86_64/run.sh` hardcodes `qemu-x86_64`, which is in neither `flake.nix` nor this machine, so the lane is red here for a missing tool. On this host CPU the same static binary run **natively** is a strictly better oracle — and is exactly what the aarch64 half already does. | `isa-fuzz` goes from red to green *and* gains a real oracle for the first time. | hours | no |

Two items are deliberately **not** in the top 10 because neither blocks the word "Supported" as this repo
defines it: the Stage-2 transliterator (G1) and the crate archive (D3). Both are below.

**One item left the list because it is already done.** `checkpoint.x86_64.threads` — the "one remaining
checkpoint failure (77/78)" of `README.md` and F-6 — **passes on this tree**: `ctest -L '^checkpoint$'`
is **78/78**, and the case alone passes **15/15** consecutive runs at ~2.4 s. Something between the
branch's checkpoint work and HEAD closed it. Two follow-ups, both small: correct `README.md` and F-6,
and decide whether the reserved-VA-window fix is still wanted as *robustness*. The original diagnosis —
x86-64 anonymous guest mmaps get kernel-chosen addresses, and a re-forked child restores after engine
init when those VAs may no longer be free — describes a condition that is inherently probabilistic, so
15 green runs on one machine is evidence, not proof. Recorded as **D5**.

---

## A. Guest ISA coverage

**Source.** A scan over all 3012 (fixture, guest-ISA) runs collecting the interpreters' own
`interp_undefined` diagnostics found **9 hits** — 3 aarch64, 6 x86-64 — of which one is a scan artefact
(see the end of A.2). That is the complete
reachable-unimplemented set; everything else that fails does so with a wrong answer, not a missing
encoding. Each site below was disassembled out of the actual fixture.

### A.1 x86-64 guest — five reachable gaps

| Item | Encoding | Fixture / suite | Advertised? | Blocks Supported | Effort |
|---|---|---|---|---|---|
| A1 `XLATB` | `D7` | `abi/corpus/x_xlat` (`compat-abi-corpus`) | baseline — always reachable | yes | hours |
| A2 `MOV r/m16,Sreg` | `66 8C d0` etc. | `completeness/movseg`, and `abi/iretq_context` reaches it first at `4028aa` | baseline | yes | hours |
| A3 MMX | `0F 6F`, `0F 7F`, `0F FC`, `0F 70`, `0F 2A/2C/2D` | `completeness/mmx-width` | **yes** — `cpuid.c` leaf 1 EDX sets EDX bit 23 (MMX) | yes | 1-2 days (needs an `mm[8]` file in `struct cpu`, see caveat) |
| A4 `MASKMOVDQU` | `66 0F F7` | `completeness/ntload` | **yes** — EDX bit 26 (SSE2) | yes | hours |
| A5 `IRETQ` | `48 CF` | `abi/iretq-context` | baseline; the manifest calls it a focused production regression | yes | ~1 day |

All five are implemented by the aarch64-host JIT (XLATB and IRETQ in `translate.c`, `MOV Sreg` in `lower/mov.c`), so this is **backend parity**, not new semantics. Note that a guest cannot avoid
any of them: three are baseline long-mode instructions and two are covered by feature bits the engine
itself advertises, which is the same argument `interp.c` makes for implementing AES/SHA (`interp.c`, above `interp_aes_sbox`).

**Caveat on A3.** `struct cpu` is a three-way ABI including the checkpoint format
(`sizeof(struct cpu)` is written into the image and validated on restore — F-5.4). Adding `mm[8]`
either changes that size, invalidating existing checkpoint images, or must alias the x87 stack the way
real hardware does. The second is correct and is also the only version that passes `mmx-width`, which
exists to check exactly that aliasing. Budget the extra half-day.

Two more x86-64 classes report themselves unimplemented but **no fixture reaches them**:

- **`XSAVE`/`XRSTOR`** (`0F AE /4,/5`, `interp.c`). `cpuid.c` leaf 1 ECX does **not** set bit 26
  (XSAVE), so a well-behaved guest never issues it. Optional polish.
- **`INT imm8` / far returns** (`CD`/`CA`/`CB`, `interp.c`). `int $0x80` is *not* a fault — it is
  Linux's 32-bit syscall gate and a real guest can use it. Nothing in the corpus does. Optional, but it
  is the one member of that group that is genuinely reachable, so it should not be closed by folding the
  group into a `#GP`.

### A.2 aarch64 guest — three gaps, all unadvertised, and a contradiction

`dotprod`, `i8mm` and `bf16` in `compat-completeness` execute SDOT/UDOT, SMMLA/USMMLA and BFCVT. The
interpreter reports all three as unimplemented, and the suite scores **aarch64 110/116**.

They are **not reachable by a well-behaved guest**:

- `g_aarch64_cpu_model.hwcap` is `0x1fb` (`src/translator/guest/aarch64/cpu.h`) — FP, ASIMD, AES,
  PMULL, SHA1, SHA2, CRC32, ATOMICS. Bit 20 (`HWCAP_ASIMDDP`, DotProd) is **clear**.
- `src/linux_abi/elf.c` emits `AT_HWCAP` (key 16) and **no `AT_HWCAP2` at all**, so
  `getauxval(AT_HWCAP2)` returns 0 and I8MM (bit 13) and BF16 (bit 14) are invisible.
- `HWCAP_CPUID` is deliberately absent, so a guest cannot read `ID_AA64ISAR*` to find them either.

So the three fixtures execute instructions the engine says do not exist, and they pass on an aarch64
host **only because the same-ISA transliterator copies unknown instruction words verbatim into the code
cache** (`docs/amd64-host.md` §1) and the host silicon happens to have those extensions. They would fail
on an aarch64 host whose CPU lacked them. That is the HWCAP-versus-completeness-manifest contradiction,
and it is a real one — see **D2**. Until it is settled, implementing the three encodings is work whose
requirement nobody has agreed.

`HL_ISA_FUZZ_ARM_ARGS` is `"+i8mm +bf16 +dczva +fpcr"` (`cmake/Phase3Gates.cmake`), i.e. the aarch64 fuzz
generator emits the same two extensions, for the same reason: on an aarch64 host the oracle is the host
CPU. That lane is host-conditional and does not run here at all.

**One scan hit is an artefact.** `compat/memory/x86_64/dbt_codecache_straightline` reports an
unimplemented one-byte opcode at an address far outside the image. Its manifest row
(`tests/compat/memory/manifest.tsv:68`) is `aarch64`-only, so the x86-64 binary is built but never
selected. Ignore it.

---

## B. Correctness defects measured here (not missing encodings)

These are the "wrong answer" class. All measured today.

| Item | Case(s) | Symptom | Class | Effort |
|---|---|---|---|---|
| B1 | `completeness/cpu_discovery` (aarch64) | `id_sigill=0`, expected 1. `interp.c`'s EL1 ID-register arm returns 0 in Rt for every EL1 ID-register `MRS`; `translate.c`'s matching arm emits `0x00000000` = `udf #0`, which **traps**. | engine defect, backend divergence | hours |
| B2 | `core/regress/nonpie-v8blob` (x86-64) | `same_half=0`. The baked `mov r,imm` materialisation of `v8_Default_embedded_blob_code_` is not rebased to the high mapping. `lower/mov.c` does it for the JIT; the interpreter declares `g_nonpie_blob_code` (`interp.c`'s `g_nonpie_blob_code`) and only uses it in `interp_call_return_pc`. | engine defect, backend divergence | hours |
| B3 | `completeness/sse4x` (x86-64) | first difference at output byte 241, `9` vs `8`, in a 14 KB output that begins with the MOVBE block. An arithmetic divergence. | engine defect | hours-1 day |
| B4 | `completeness/xflags-sig` (x86-64) | wait status `0x8b00`, **zero** bytes of stdout. | engine defect | hours-1 day |
| B5 | `completeness/pf-maps-relro` (**both** ISAs) | `has_ro=0`, expected 1 — `/proc/<pid>/maps` does not show the RELRO read-only region. Failing on both guest ISAs means it is host-neutral; **verify it on an aarch64 host before treating it as an amd64 item**. | engine defect, probably not amd64-specific | hours-1 day |
| B6 | `signals/sigurg-go-preempt` (aarch64 guest; the row is aarch64-only) | fails in the suite run, and run alone had not completed after 120 s. The row covers SIGURG async-preempt suppression for aarch64 Go images. Sole blocker for `compat-signals`. | engine defect | hours-1 day |
| B7 | `posix/tty_suspend` (aarch64 guest) | **A live hang on this machine right now**: two processes at 1 h 41 m elapsed with `00:00:00` CPU, in `do_wait` and `poll_schedule_timeout`. That is exactly the zero-CPU signature `docs/ci-green.md` defines, and it is not one of the four futex-across-fork cases that document names (those were fixed). | engine defect | days, diagnosis-led |

B1 and B2 are the same shape as F-3.11: an obligation the JIT discharges and a second backend silently
does not, with nothing structural forcing agreement. They are cheap and they are the two highest-value
correctness items on the list.

**Not defects.** Three corpus cases fail on this machine for environmental reasons and must not be
counted against the engine:

- `completeness/priority` — prints `nice=12`, golden `nice=5`. The shell is niced to 12 and
  `setpriority(PRIO_PROCESS, 0, 5)` needs `CAP_SYS_NICE` to lower it.
- `process/sched-attr` — `ok=0`, golden `ok=1`, for the same `setpriority` step. **Verified**: running
  `build-amd64/compat/process/x86_64/sched_attr` *natively* on this machine also prints `ok=0`. The
  engine is byte-identical to native here; the golden describes a nice-0 environment.
- `syscall/memfd-seals` — fails on an ext4 build tree, **passes** with `HL_MATRIX_SCRATCH_DIR=/dev/shm`.
  The runner already prints a note saying so. (`filesystem/memfd-seals` passes either way.)

---

## C. Test lanes

`ctest --print-labels` on this host lists 50 labels over **396 registered cases**. Classification below;
"measured" means run today unless noted.

### C.1 Green here (measured)

| Lane | Result |
|---|---|
| `unit` | 115/115 |
| `gate` (`gate.ci-lane-parity`) | pass — every lane `cmake/CiLanes.cmake` declares selects >= 1 test here |
| `rust` (`rust.fmt`, `rust.clippy`) | 2/2 |
| `lifecycle` | 10/10 |
| `production-config` | 3/3 |
| `e2e-oracle` | **68/68**, 14 s total |
| `checkpoint` | **78/78** — including `checkpoint.x86_64.threads`, which the docs still call the one failure |
| `checkpoint-io` | **34/34** |
| `integration` (`remote-supervisor`) | 1/1 |
| `compat-native` | 1/1 |
| `dynamic-e2e` | 2/2 |
| `compat-ipc` | pass — **124/124 per ISA**, 122 cross-ISA identical |
| `compat-syscall-edges` | pass — 52/52 per ISA, 52 cross-ISA identical, 5.4 s |
| `compat-time` | pass — 39/39 per ISA, 183 s |

`e2e-oracle`, `dynamic-e2e`, `production-config` and `checkpoint-io` are listed in
`.github/workflows/linux-x86_64.yml` as absent because their runners "still carry unscaled per-case
budgets (30 s / 15 s), so an interpreted guest trips them as if it had hung." **On an idle machine they
do not trip.** The budgets are still unscaled and still need scaling (F3 — C.6 shows the 15 s one
flipping twelve cases red under mere CPU contention), but the workflow comment overstates the
blockage: the engine is not what is stopping these lanes.

### C.2 Red here (measured)

| Lane | Result | Cause | Class |
|---|---|---|---|
| `package`, `package-activation`, `package-embedded` (all three select `package.consumer-link`) | **fail at step 4** | linking the activation consumer against the staged `libhl-engine-activation.a` dies on `e_cset`, `e_bfi`, `e_fmov_from_d`, ... out of `lower/x87.c` | engine/build defect — item **E1** |
| `embedding` (`dual-backend.link`) | **Not Run** — `package/linux-x86_64/dual-backend-link-test` was never produced | same cause: the target link-tests the archive with `--whole-archive` | same |
| `isa-fuzz` (`isa-fuzz.x86_64-regress`) | `missing reference oracle: qemu-x86_64` | `run.sh` hardcodes qemu; qemu is not in `flake.nix` and not on this machine | tooling + design — item **F4** |
| `compat-completeness` | aarch64 **110/116**, x86_64 **135/142** | A1-A5, A.2, B1, B3, B4, B5, plus `priority` (environmental) | mixed |
| `compat-core-regress` | aarch64 11/11, x86_64 **9/10** | B2 alone | engine defect |
| `compat-signals` | aarch64 **66/67**, x86_64 64/64 | `sigurg-go-preempt` (aarch64-only row) alone | engine defect — top-10 item 6 |
| `compat-isa-x86-64` | unreachable | 107 golden lines encode ARM NaN semantics (F-3.9) | **golden-side** — item **D1** |

### C.3 Not measured here

`perf-linux` (28 cases), `perf-native` (11), `production` (51, of which 45 are `production-full-*`),
`compat-extended` (2, hour-scale x `--repeat 10`), and 18 of the 24 compat suites. `README.md`
records the corpus-wide figure — 2632/3013 = 87.4%. **There is no
committed per-suite, per-ISA scoreboard for this host at all** — and as of the in-flight comment
rewrite there is less than there was: `cmake/CiLanes.cmake` used to carry per-ISA numbers for
`compat-syscall-edges`, `compat-time` and `compat-core-regress` beside `HL_CI_COMPAT_HOSTS`, and now
defers to `docs/ci-green.md`, which does not contain them. `docs/amd64-host-findings.md` §3.10 names
`compat-ipc` and `compat-signals` as x86-64-guest-only, which is stale in the pessimistic direction.
Nothing left in the tree says which suites are gateable, and without that nobody can tell whether the
remaining failing runs are one defect or three hundred — as `compat-ipc` and
`checkpoint.x86_64.threads` both just demonstrated in the other direction. That is itself a gap: see
**F5**.

`compat-extended` deserves a separate note. It is `core-workload` and `soak` at `--repeat 10`, already
hour-scale on an aarch64 host, and this host interprets. Whether it is *practical* here at all, as
opposed to merely correct, is unmeasured and may become an argument for Stage 2 (**G1**) on grounds
other than perf thresholds.

### C.4 Registered here but structurally weaker than on an aarch64 host

- **`isa-fuzz`**: one case here, three on an aarch64 host (`isa-fuzz.aarch64-regress{,-pie}` are
  registered only under `CMAKE_HOST_SYSTEM_PROCESSOR MATCHES aarch64`). Half the differential ISA fuzz
  coverage is host-conditional. Already documented at `DOCS.md` §7.5.1 and at the declaration.
- **`perf-native`**: 11 cases here, 11 on an aarch64 host, but they are *different* fixtures — the
  host's own ISA. Not a gap; a fact.
- **`perf-linux`**: 28 cases, of which **26 are renamed `.record-only`** and only
  `perf.linux-resource-{aarch64,x86_64}` still enforce. See **G2**.

### C.5 Correctly absent

`e2e-mac`, `macos`, `compat-direct` and `perf-macos` are Darwin-only by construction. Nothing to do.

Worth noting that `package-activation` and `package-embedded` — declared only in
`HL_CI_REGISTRY_DARWIN` — *are* non-empty here, because `package.consumer-link` carries all three
labels. `gate.ci-lane-parity` counts tests, so it cannot see that two of those labels are aliases of a
third rather than distinct coverage. Not a gap, but do not read three green labels as three gates.

### C.7 Structural areas checked, and what they turned out to be

Asked directly, in case their absence above reads as an oversight.

- **Host services / providers / device paths.** `src/host/<os>/` is host-CPU-neutral by construction
  (`docs/amd64-host.md` §1) and its provider tests live in `unit`, which is 115/115 here. No gap.
- **The fork server.** F-3.11's `fork(2)` deadlock — every `fork` from a non-PIE x86-64 guest that had
  started a thread — was found and fixed on this branch, and the `checkpoint` lane (78/78 here, and it
  re-forks) exercises the path. `README.md` reports `production.matrix` green; I did not re-run it. No
  gap found, but note that four futex-across-fork cases were the motivating example for the stall
  detector and B7 shows the class is not extinct.
- **The activation entry point.** `src/core/activation.c` and `src/core/target/dual.c` build and install
  here; the archive is complete (E2) and does not link (E1). The one live landmine is F-2.4:
  `hl_run_linux_guest` still hardcodes `hl_aarch64_run_linux_guest` as its default guest, commented
  "native AArch64 default". Nothing tests that path, so nothing here proves it either way.
- **macOS-remote lanes.** `cmake/toolchains/macos-remote.cmake` and `tools/run_remote_macos_ctest.sh`
  drive a mac *from* a Linux host through OrbStack's `mac` bridge. Nothing in them is host-CPU-specific
  that I can see, but **there is no mac reachable from this machine, so I could not verify it** — an
  x86-64 Linux host driving the macOS lane is untested. It is not part of this host's bar.
- **Checkpoint across hosts.** The checkpoint image records `sizeof(struct cpu)` and validates it on
  restore, and `struct cpu` is deliberately shared between the JIT and interpreter backends so images
  stay compatible (`docs/amd64-host.md` §2, F-5.4). **Nothing tests an image written on one host CPU and
  restored on another**, and I cannot test it from here. If cross-host checkpoint is meant to work, that
  is an unwritten test, not a known defect. Note that F-1.1 deliberately made the *persistent code
  cache* host-CPU-specific; the checkpoint image is a different artefact and is not obviously covered by
  the same reasoning.
- **The IR / per-host-CPU codegen backends.** F-3.1 establishes these are on no execution path. Running
  `unit.codegen` here executes the x86-64 lowerer's emitted bytes for the first time on any machine, and
  it passes. Not a gap; a coverage gain.

### C.6 A measurement artefact worth knowing about

The first `checkpoint` run here reported all twelve `checkpoint.x86_64.io-*` cases failing. The same 78
tests, run again with nothing else on the machine, are **78/78**, each io case in ~2.2 s. The only
difference was a concurrent `matrix-runner`. So: on a machine this loaded, at nice 12, the checkpoint
runner's **unscaled 15 s** per-case budget (`tests/integration/checkpoint_tree_runner.c`) is close
enough to the edge that CPU contention alone flips a whole lane red. That is the concrete case for
**F3**, and it is also a warning about reading any single sweep on a shared machine.

---

## D. Decisions a human has to make

None of these can be worked around from this machine. Each is listed plainly because pretending
otherwise would hide the real shape of the remaining work.

**D1. The `isa-x86-64` golden.** `tests/compat/isa/x86_64/expected/isa-regress.out` disagrees with real
x86-64 hardware on 107 lines, all two-NaN selection or NaN sign (F-3.9). `compat-isa-x86-64` is
therefore unreachable on **any** x86-64 host, and it is in both `HL_CI_SHARDED_LINUX` and
`HL_CI_SHARDED_DARWIN`. Regenerating it on real hardware is the fix; doing so exposes a genuine
two-NaN-selection gap in the AArch64 JIT, so the immediate effect is to move a red lane from one host to
the other. **Blocks Supported. Needs the decision and the hardware.**

**D2. HWCAP versus the completeness manifest.** Three `active` rows
(`tests/compat/completeness/manifest.tsv:8-10`) execute DotProd/I8MM/BF16 instructions that
`g_aarch64_cpu_model.hwcap = 0x1fb` and the absent `AT_HWCAP2` both say do not exist. Either:

- *advertise them* — set the HWCAP bits, emit `AT_HWCAP2`, and implement the encodings in the aarch64
  interpreter (1-2 days) so the model and the corpus agree; or
- *do not* — deactivate or retag the three rows, and accept that the aarch64-host transliterator will
  keep letting unadvertised host instructions through, which is a host-dependent pass.

Doing neither leaves three permanently red cases on this host and three host-dependent green ones on the
other. **Blocks Supported** (it is three cases in a matrix that must pass).

**D3. The crate archive.** `pkgs/rust/build.rs` accepts `x86_64-unknown-linux-gnu` and
`refresh-crate-archives-linux` builds it, but each archive is ~24 MB against the 10 MB crates.io budget
documented in `pkgs/rust/Cargo.toml` (F-2.7), and `flake.nix` gates the Rust outputs on
`hasCrateArchive = backend.supported && hostCpu == "aarch64"`. This is a **publication** decision, not
an engineering one. It does **not** block "Supported" as `README.md` defines it — that bar is the test
matrices — but it does block "first class" in the shipping sense, and `checks.x86_64-linux.rust` will
not exist until it is solved.

**D4. What `perf-linux` should enforce.** See G2.

**D5. Whether the reserved VA window is still wanted.** `checkpoint.x86_64.threads` now passes (78/78
lane, 15/15 alone), so the *symptom* is gone. The condition F-6 describes has not been shown to be gone:
the x86-64 guest pins only its main stack, so anonymous guest mmaps — glibc pthread stacks — take
kernel-chosen host addresses, and a re-forked child restores after engine init. The aarch64 guest is
immune because its mmaps live in a biased window the host never allocates from. Either accept the case
as fixed and delete the item, or add the reserved window as robustness against a layout this machine
happened not to produce. That is a judgement about how much probabilistic evidence is enough, not a
piece of work anybody can just do. Days if the answer is "add it".

---

## E. Artifacts and packaging

**E1. `lower/*.c` is a link-time landmine, and it is no longer only a landmine.** F-3.3 records this as
"worked around rather than fixed". Measured today, it is an outright red:

- `nm` on `build-amd64/package/linux-x86_64/libhl-engine.a` (the installed
  `libhl-engine-activation.a`) shows **92 undefined ARM64-emitter symbols** — `e_bfi`, `e_cset`,
  `e_fmov_from_d`, `emit_exit_const`, `hl_x86_emit_*`, ... The 21 aborting stubs in `interp.c` cover
  only the `repstr.c` subset that on-demand archive selection actually pulls.
- `package.consumer-link` therefore fails at step 4 (`cmake/PackageTest.cmake:134`).
- `dual-backend-link-test` is in `all` (`cmake/Phase2Production.cmake`) and its binary **does not
  exist** in the build tree, so `dual-backend.link` reports *Not Run*.

Preference order is unchanged from F-3.3: split the host-neutral runtime halves (`hl_x86_rep_movs`,
`hl_x86_rep_stos`, and the two setters `engine_global_init` calls) out of the lowering objects; or gate
`lower/*.c` out of `IR_SOURCES` on a non-AArch64 host. Effort 1-2 days. **Blocks Supported.**

**E2. `hl-engine-activation` installs correctly.** F-1.10's inconsistency is fixed: `cmake --install`
here produces the **same 23 files** an aarch64 Linux host produces, including `include/hl/activation.h`,
`lib/libhl-engine-activation.a` and `lib/pkgconfig/hl-engine-activation.pc`. The archive is complete and
correctly named; it simply does not link (E1). Nothing further to do here once E1 lands.

**E3. Crate archive** — see D3. The positive CI coverage exists already:
`.github/workflows/linux-x86_64.yml` builds the x86-64 archive with `refresh-crate-archives-linux`,
installs it, and links the crate's `--lib` tests against it. What is missing is only the committed
bytes, and that is the budget decision.

---

## F. CI gating

Today `.github/workflows/linux-x86_64.yml` runs `ctest -L unit` alone. `HL_CI_COMPAT_HOSTS` omits
`Linux-x86_64`, so I20 requires that workflow to name **no** lane at all. None of the ~3000 corpus runs
on this host is gated.

**F1. The exact sequence.** `tools/check_ci_workflows.sh`'s `sole_host_for` fails if
`HL_CI_COMPAT_HOSTS` names more than one `Linux-*` token, because there is one `HL_CI_SHARDED_LINUX`
list per host **OS**. So:

1. Split `HL_CI_SHARDED_LINUX` into `HL_CI_SHARDED_LINUX_AARCH64` and `HL_CI_SHARDED_LINUX_X86_64` (the
   Darwin list is untouched). Keep the aarch64 list byte-identical to today's so the aarch64 lane cannot
   move.
2. Teach `lanes_in`, I13/I14 (`check_shards`) and I19/I20 to key on the host **token** rather than the
   OS. I19's `sole_or_fail` disappears; the parity comparison becomes per-token.
3. *Then* add `Linux-x86_64` to `HL_CI_COMPAT_HOSTS`. Doing this before step 2 turns I20 off — it only
   applies to a host absent from the list — and leaves that workflow with **no** structural guard, which
   is strictly worse than today.
4. Shard the green subset.

**Step 2 is bigger than it looks, and this is the part nothing in the tree currently says.** I19's
invariant is *cross-host parity*: every sharded lane must run on every host in `HL_CI_COMPAT_HOSTS`
unless `HL_CI_SHARDED_HOST_ONLY` declares the asymmetry. A new compat host that starts with three of the
24 lanes therefore needs **21 `Linux-aarch64:<lane>` exemptions** — and `cmake/CiLanes.cmake` already
argues, about a different case, that exempting everything "asserts nothing". So parity is the wrong
invariant for a host that is mid-adoption, and step 2 has to decide what replaces it. The smallest
honest answer: keep parity between *mature* compat hosts, and give an adopting host its own list whose
invariant is monotonic growth (a lane may be added, never removed) rather than parity. Budget the extra
half-day there, and expect it to be the part that needs review.

Effort ~1-1.5 days, no decision required beyond the invariant above. **Blocks Supported** (item 6 of
`DOCS.md` §11 "Add a host CPU" is explicitly this).

**F2. What to gate first, in order of evidence.**

| Lane | Evidence | Gate when |
|---|---|---|
| `compat-ipc` | **124/124 per ISA**, 122 identical — measured today | immediately, in step 4 above. It is the single largest green lane available and the only record of it in the tree calls it half-green |
| `compat-syscall-edges` | 52/52 per ISA, 52 identical, **5.4 s** — measured today | immediately, in step 4 above |
| `compat-time` | 39/39 per ISA, **183 s** — measured today | second. `cmake/CiLanes.cmake` argues for holding it back because timerfd/itimer assertions are the classic hosted-runner flake; that argument is about the *runner*, not the engine, so gate it once it has been seen green there |
| `compat-core-regress` | 11/11 + 9/10 — one case, B2 | as soon as item 3 lands |
| `compat-signals` | 66/67 + 64/64 — one case, top-10 item 6 | as soon as that lands |
| `e2e-oracle`, `dynamic-e2e`, `lifecycle`, `production-config`, `checkpoint`, `checkpoint-io` | all green today (C.1) | after F3, on runner-class hardware. These are *direct* lanes, not sharded ones, so they need no `HL_CI_SHARDED_*` change at all — only `HL_CI_DIRECT_LINUX` (or a per-host split of it) and a workflow step |
| everything else | unmeasured or known red on at least one guest ISA | after F5 says which |

The unit of gating is the **label**, because `cmake/Phase3Compat.cmake` gives each compat label one
CTest case covering both guest ISAs, so a suite green on one guest ISA and red on the other is a red
lane and not half a green one.

**F3. Scale the four remaining runners' per-case budgets.** `tools/e2e_runner.c`,
`tools/config_e2e_runner.c`, `tools/rootfs_e2e_runner.c` (30 s) and
`tests/integration/checkpoint_tree_runner.c` (15 s) do not read `HL_MATRIX_TIMEOUT_SCALE`. On an idle
machine the lanes they drive all pass here — but C.6 shows the 15 s one flipping twelve cases red under
nothing worse than CPU contention. So this is not merely insurance for a slower runner; it is the
difference between a lane that is gateable and one that is *flaky*, which is worse than red. Hours. Do
it before F2's last row.

**F4. `isa-fuzz` oracle.** `tests/fuzz/isa/x86_64/run.sh` defaults `qemu="qemu-x86_64"` and hard-fails when it is absent. qemu appears nowhere in `flake.nix`, so this lane depends on an ambient
tool on **every** host, and it is registry-only everywhere, so nothing has noticed. On an x86-64 host,
running the generated static binary natively is both available and a better oracle — it is precisely the
symmetry `tests/fuzz/isa/aarch64/run.sh` already relies on. Hours. Optional polish that happens to turn
a red lane green.

**F5. Publish a per-suite, per-ISA scoreboard for this host.** The 87.4% figure is a single number over
24 manifests, and it is now the *only* number: the three suites' worth of per-ISA detail that used to sit
beside `HL_CI_COMPAT_HOSTS` was condensed into a pointer to `docs/ci-green.md`, which does not carry it
(C.3). Nobody
can plan the remaining failing runs without the breakdown, and producing it is cheap now that the runner
reports per-ISA counts (F-3.10's first defect is fixed) and does not skip the second ISA leg. Commit it
next to `cmake/CiLanes.cmake`, where the gating argument already lives, so it cannot drift out of sight.
A day, mostly waiting. Not a blocker; it is what makes the rest of this list finite.

---

## G. Performance

**G1. Stage 2, the same-ISA x86-64 transliterator.** `docs/amd64-host.md` §3 states plainly that it *"is
not a prerequisite for the host being supported"*, and nothing measured here contradicts that: every
lane that fails, fails for correctness or packaging reasons, not for speed. §3.1 has already settled the
register model (steal no GPR; reach `struct cpu` through `%gs`, mirroring `hl_a64_load_cpu`'s
`TPIDRRO_EL0` trick; never touch the 128-byte red zone). **Weeks.** Independent of everything above
except that it plugs into the same `G_OWN_TRAMPOLINES` seam.

**G2. Making `perf-linux` enforcing.** Today 26 of its 28 cases are renamed `.record-only` and only
`perf.linux-resource-{aarch64,x86_64}` enforce — correctly, because their assertions are leak bounds
(RSS growth, descriptor and thread baselines) and a leak is not a function of backend speed. Enforcing
the other 26 needs one of:

- Stage 2 plus a re-measurement against the existing `PERF_LIMIT_*` — the honest path, and the one
  `docs/ci-green.md` argues for; **or**
- a second, host-conditional threshold set, which `docs/ci-green.md` rejects because it blesses whatever
  a half-written interpreter happens to do.

So G2 **depends on G1**, and G1 is optional for Supported, therefore G2 is too — provided
`README.md`'s host table and `docs/ci-green.md` keep saying so. That is **D4**: someone has to confirm
that "Supported" does not include "perf thresholds enforced". `DOCS.md` §12's "Performance and release"
checkbox is ticked and says *"on macOS and Linux for both guest ISAs"*, which on a literal reading it no
longer is. Decide which reading applies.

---

## H. Documentation that is now wrong

Small, but each will cost the next reader time. F-4 has the pre-existing list; these are new or newly
false.

**A caution on citations.** While this document was being written, a separate comment-rewrite pass was
in flight across `src/`, `cmake/`, `tools/`, `.github/`, `flake.nix` and `pkgs/` — 57 files. Line
numbers into those files are therefore not given anywhere above; identifiers are. Rows below that name a
comment were re-checked against the tree at the end, but a comment can be fixed in the next minute, so
verify before acting on one.

| Where | Claim | Reality |
|---|---|---|
| `DOCS.md:434` | *"a Linux `nix develop` configure registers 396 cases"* | 396 is **this** host's count. An aarch64 Linux host registers the same set plus `isa-fuzz.aarch64-regress` and `-pie` = 398. |
| `DOCS.md:536-540` (§7.4) | production lanes *"cannot pass until the x86-64 translator backends land"* | They landed. `production.smoke-x86_64`, `production.matrix`, `lifecycle` (10/10) and `production-config` (3/3) pass here. |
| `README.md:19`, `docs/amd64-host-findings.md` §6 | *"`checkpoint` is 77/78"*, `checkpoint.x86_64.threads` *"still fails, consistently"* | `ctest -L '^checkpoint$'` is **78/78**; the case alone passes 15/15. See D5. |
| `docs/amd64-host-findings.md` §3.10 | `compat-ipc` and `compat-signals` are *"green on the x86-64 guest only"* | `compat-ipc` is **124/124 on both**, 122 cross-ISA identical. `compat-signals` is 66/67 + 64/64 — one case away. The largest gateable lane in the tree is being described as ungateable. |
| `README.md:22` | of the failing runs, *"most of the remainder is unimplemented named CPU extensions"* | The interpreters report **9** unimplemented sites across all 3012 runs, 8 of them real (A). Whatever the remainder is, it is not mostly missing extensions — and nothing in the tree currently says what it is (F5). |
| `src/translator/guest/aarch64/interp.c`, the EL1 ID-register comment | *"these read 0 rather than trap ... the mask is spelled as `translate.c`'s so both backends deny the same set"* | The **mask** matches; the **answer** does not. `translate.c` emits `0x00000000`, which is `udf #0` and traps; the interpreter writes 0 into Rt. Same gate, opposite behaviour — which is precisely the divergence the comment claims to prevent, and it is why B1 exists. |

---

## I. Dependencies

```
E1 (lower/*.c split) ──> package / package-activation / package-embedded / embedding green
                    └──> interpreter can use the bulk rep helpers (feeds G1's baseline)

F1 (split sharded lists, teach I19/I20, declare token)
   ├── F2 gate compat-ipc, compat-syscall-edges, compat-time   [all measured green, 430 runs]
   ├── item 3 / B2 ──> compat-core-regress gateable
   ├── item 6 / B6 ──> compat-signals gateable
   └── F3 (scale 4 runners) ──> e2e-oracle / dynamic-e2e / lifecycle /
                                checkpoint / checkpoint-io gateable as DIRECT lanes
                                on runner-class hardware

D2 (HWCAP decision) ──> A.2 (implement SDOT/SMMLA/BFCVT)  ──┐
A1..A5 (x86-64 encodings)                                   ├──> compat-completeness green
B1, B3, B4, B5                                              ┘

D1 (regenerate isa golden) ──> compat-isa-x86-64 reachable
                          └──> exposes a two-NaN gap in the AArch64 JIT (new work on the other host)

D5 (VA window) ──> robustness only; the case it was for now passes

F5 (scoreboard) ──> tells you what the remaining failing runs actually are
                    (everything after the items above depends on it being done first)

G1 (Stage 2) ──> G2 (perf-linux enforcing)         [both optional for Supported]
D3 (crate budget) ──> published x86-64 archive     [optional for Supported; blocks "shipping"]
```

## J. What "Supported" honestly requires

Working the list top to bottom gets there, with three caveats stated plainly:

1. **Two items cannot be closed from this machine.** D1 needs real x86-64 hardware and the decision to
   move a committed golden; D2 needs somebody to say whether the engine advertises DotProd/I8MM/BF16.
   Both are inside the bar — they are cases in matrices that must pass — so "Supported" is *gated on a
   human*, not only on work.
2. **One item is outside the bar and should stay outside it.** The crate archive (D3) is a publication
   decision. `README.md`'s definition of Supported is about test matrices; nothing in it mentions
   crates.io. Do not let the two merge.
3. **The remaining failing corpus runs are not enumerated.** Sections A and B account for about 25 of
   them across six suites; the rest live in the 18 compat suites nobody has measured per-case on this
   host. Today's measurements moved the count in *both* directions — `compat-ipc` turned out fully
   green and `checkpoint.x86_64.threads` turned out fixed, while `compat-completeness` turned out to
   have five failures nobody had listed. F5 is therefore not optional bookkeeping: without it, no
   estimate of "how far from Supported" is worth anything, including this one.

A useful sanity check on the shape of the remainder: of everything measured today, **not one cross-ISA
stdout divergence appeared** among cases passing on both guest ISAs (0 mismatched out of 122 + 89 + 62 +
52 + 39 + 7 compared). The two backends agree with each other. What is left is agreement with the
goldens.
