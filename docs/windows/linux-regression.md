# The Linux host is not regressed

The Windows port edits shared code. This is the evidence that none of it changed the Linux host,
re-runnable by `tools/windows/linux_regression_check.sh`.

## Why this document exists

Adding a host OS is supposed to be additive (DOCS.md §11), but the M0/M1 work did not only add
files. It edited the root `CMakeLists.txt`, eight `cmake/*.cmake` files, and one shared header —
and none of that could be checked from a Windows box. Three of the edits are the kind that look
inert and are not:

- `find_program(HL_BASH_EXECUTABLE NAMES bash REQUIRED)` became optional, at three sites. That is a
  real behaviour change on **every** host: a bash-less Linux box now configures with four tests
  skipped instead of failing.
- `-fvisibility=hidden` moved out of the inline `hl_engine_cflags` list into a trailing guarded
  `target_compile_options`, with a claim that flag **order** on Linux and Darwin was preserved
  byte-for-byte. That is a claim about generated build rules, and only a diff of the generated
  command line settles it.
- `src/host/native_context.h` gained a Windows arm before the `#else #error`. It should be
  unreachable on Linux; "should be" is not evidence.

## Method

Build the branch point and the branch side by side in one checkout, identical configure line, and
compare. Ran on WSL Ubuntu 26.04, x86-64, gcc 15.2.0, cmake 4.2.3, ninja 1.13.2 — a plain native
Linux configure, no nix, so the guest cross-compilers are absent and the fixture-backed suites are
correctly disabled on both sides.

Baseline is `6d8514c3` (*docs: squash seven amd64-host documents into one*), the commit
`feat/windows-amd64` was branched from.

## Result

| | baseline `6d8514c3` | branch `feat/windows-amd64` |
| --- | --- | --- |
| configure | 0 | 0 |
| full build | 0 | 0 |
| warnings | 74 | 74 |
| new warning kinds | — | **none** |
| `ctest -L unit` | **110/111** | **110/111** |

Compiler command line for `src/core/engine.c`, taken from the generated `build.ninja`:

- flag **set**: identical, 16 flags
- flag **order**: identical
- `-fvisibility=hidden` present: baseline 1, branch 1

So the `-fvisibility` reordering claim is verified rather than assumed, and it is still applied on
Linux.

### The one failure is pre-existing and environmental

`unit.native-capacity` fails on **both** trees, identically:

```
tests/unit/test_native_capacity.c:65: check failed: opened.status == HL_STATUS_OK
```

It fails on the baseline, so it is not caused by this branch. It is a descriptor-capacity test and
this was a stock WSL environment; the two columns matching is the result that matters. It is
recorded here rather than subtracted away, so that a genuinely red baseline stays visible instead
of being hidden by a "no new failures" summary.

## Standing rule

Any change touching shared build files or `src/**` gets this run **before** it is merged, not
after. The script prints both columns rather than a single verdict for exactly that reason.
