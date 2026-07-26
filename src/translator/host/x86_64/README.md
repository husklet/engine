# `src/translator/host/x86_64/` — intentionally empty

There is no x86-64 host backend. This directory holds no code, and its emptiness is the accurate statement.

It used to hold `codegen.c` + `x86_64_codegen.h`, a lowerer for the IR in `include/hl/ir.h`. That IR, its lowerers,
and the public `hl/ir.h` + `hl/codegen.h` headers were deleted: `hl_codegen_*` had no caller anywhere in `src/`, and
the 17-opcode IR could express neither production frontend (no flags, no vectors, no atomics, no syscalls).

The reason for deleting rather than keeping it is this directory. A symmetric `host/<cpu>/codegen.c` per host CPU
reads exactly like the production lowering pipeline, and it is not one. The engine's guest frontends under
`src/translator/guest/` emit host machine code **directly** — ARM64 today — so "add an x86-64 host" is new code
generation there, not a matter of wiring up a backend that already exists here. That misreading cost real time; see
`docs/amd64-host-findings.md` §3 for the history and `docs/amd64-host.md` for what the work actually is.

The live sibling is `../aarch64/asm.{c,h}`, the ARM64 instruction assembler the production JIT uses.
