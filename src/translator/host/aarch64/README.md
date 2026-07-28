# AArch64 host support

`asm.{c,h}` is the ARM64 instruction assembler (`hl_a64_*`) the production JIT emits through. It encodes
instructions; it makes no host-operating-system calls and holds no translation policy. Covered by `unit.a64_asm`.

This directory once also held `codegen.c` + `aarch64_codegen.h`, a lowerer for the IR in `include/hl/ir.h`. That IR
and both of its per-host-CPU lowerers were deleted — nothing in `src/` ever called them, and the layout they created
(`host/<cpu>/codegen.c`, symmetric across two host CPUs) misread as the production lowering pipeline. There is no
host-neutral IR: the guest frontends under `src/translator/guest/` emit ARM64 machine code directly. See DOCS.md §3.3.
