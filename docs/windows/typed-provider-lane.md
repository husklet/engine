# The typed-provider guest path is dead on Windows

Working note. `epoll`, `eventfd`, `pipe`, `timerfd` and `pidfd` are finished, unit-tested typed
providers, their host groups now exist and validate on Windows, and **nothing on Windows can reach
any of them.** Wiring them does not change that, and this is the measurement that proves it, so
nobody spends another lane discovering it a fourth time.

**86 of the 605 failing corpus cases are named for these five providers.**

## The measurement

Same probe in `bound_route`, both hosts, same syscall numbers:

```
Windows: [PROBE nr=19 pid=2240  box=0000000000000000]   eventfd2
         [PROBE nr=59 pid=23852 box=0000000000000000]   pipe2
         [PROBE nr=26 pid=22036 box=0000000000000000]   inotify_init1 -> "Operation not supported"
Linux  : [PROBE nr=19 pid=498844 box=0x609f2e26cd70]
```

`g_linux_box` is **NULL in the guest process on Windows** and non-NULL on Linux. Every typed arm in
`bound_route` is guarded by `g_linux_box != NULL`, so the entire typed-provider guest path is dead
there. The `inotify` arm that every scoping document recommends as the wiring template is itself
inert on Windows — which is exactly why `inotify_init1` reports ENOTSUP.

## The cause, and why it is not a bug

`src/core/lifecycle.c`'s `hl_production_spawn` has a `#if defined(_WIN32)` branch that does
`(void)box;`, and `hl_production_cold_entry` calls `hl_run_linux_guest(&services, NULL, ...)`.
Linux and macOS pass `entry.box = box`.

That was deliberate, and the comment says why: a `CreateProcess` child shares no address space with
its parent, so a box built in the parent's heap cannot cross. It is the same constraint that made
`spawn_cloned` refuse `lifecycle.c`'s stack-local entry context until the launch record was
extended to carry a serialised payload.

So the box is not missing by oversight. It has never been *constructed child-side*.

## Why wiring is unlandable in either form

Measured by A/B — two frozen tree snapshots differing in exactly one file, 86 added lines wiring
`eventfd2` precisely as scoped, engines built identically and driven against the same staged corpus:

| | passed | active | rate |
| --- | --- | --- | --- |
| A, baseline | 860 | 1471 | 58.5% |
| B, `eventfd2` wired | 859 | 1471 | 58.4% |

Zero fixed. Un-gated, the wiring changes the guest path only where the box is live — Linux and
macOS — which is pure regression risk for zero Windows benefit. Gated on `_WIN32`, it is dead code.

## The corpus noise floor is ±1

The single "newly failing" case above is `dbt_longjmp_reenter`, which passes 5/5 standalone on
**both** engines. 17 of 18 comparable suites were identical case-for-case, and all 9 non-fork
eventfd guests produced byte-identical output on A and B.

Treat a ±1 delta on a full run as noise, not signal. Anything smaller than ±2 needs a standalone
re-run before it is believed.

## Three prerequisites, each found by probing the previous

1. **`bound_shadow_activate` opens its sentinel with the bare CRT `open()`**, which files no
   handle-table binding — `host_fd.h` defines `openat`, not `open`.
2. **So `fcntl(F_DUPFD_CLOEXEC)` refuses** at its unbound-descriptor guard: measured errno 40,
   sentinel −1. `host_fd.h` also refuses `F_DUPFD` *by design*, naming `engine_fd_reloc`'s `1<<20`
   floor. The UCRT descriptor table measures **8191**, so a floor at or below 8191 is synthesizable
   and `1<<20` is not.
3. **Even with both fixed, the child still has no box.** A throwaway child-local box prototype in
   `lifecycle.c` returned status 70 with no output for every guest, before and after synthesizing
   `F_DUPFD`.

(3) is the real blocker and it lives in `src/core/lifecycle.c`. (1) and (2) belong with the
ambient-fd work.

## A note on measuring in a shared tree

The published baseline was 820/1471; the real one was 860, because another lane's uncommitted work
had moved `ipc` from 6/124 to 34/124 mid-session. That lane also edited `binding.c` and
`src/host/linux/host.c` *between* the two snapshots. The isolation held only because `diff -rq`
caught it and B was rebuilt as A-plus-the-change.

Do not trust a published corpus number while lanes are active. Freeze both trees, diff them, and
build both engines yourself.
