# x86-64 host backend

Lower validated IR to x86-64 host code. Lowering has no host-operating-system dependencies and preserves the
translator's documented guest-state contract. Guest ISA and host CPU are independent build axes.
`codegen.c` is at full IR-opcode parity with the AArch64 backend beside it; nothing here is reserved.

It is a lowerer for the IR in `include/hl/ir.h`, **not** the backend that runs guests on an x86-64 host.
`hl_codegen_*` has no caller in `src/`, and the frontends under `src/translator/guest/` emit host machine
code directly -- ARM64 today. See DOCS.md section 3.3 and `docs/amd64-host.md`.

This file previously read "Reserved for the x86-64 host-code backend", which was accurate when the directory
was created and stale by the end of the same day (`docs/amd64-host-findings.md` section 3). Read together
with the symmetric directory listing, that was the most misleading thing in the tree for anyone adding a host
CPU: it suggests running guests on an x86-64 host is a matter of finishing this file, when this file is
finished and the work is elsewhere.

`unit.codegen` covers this file on every host but only *executes* the emitted bytes on a matching host CPU
(`#if defined(__aarch64__)` / `#elif defined(__x86_64__)`). Every CI runner was AArch64 until an x86-64 host
lane existed, so these bytes had never been executed on any machine.
