# Differential compat findings

Every case below was written as a small self-contained C program, run **natively on the host Linux
kernel** to produce its golden output, and only then run through both production engines. Where the
engine disagreed with the kernel, the kernel is the specification.

All four divergences found in this round were fixed; the cases are `active` in their manifests.

---

## 1. `dup3` flag validation aliased to the x86 legacy-`dup2` marker

Case: `tests/compat/syscall_edges/dup3_edges.c` (`sc-dup3-edges`)

```
native  a=1 b=-1 eb=22 c=-1 ec=22 d=201 f0=0 f1=1 e=202 f2=0 g=-1 eg=9
engine  a=1 b=-1 eb=22 c=200 ec=0  d=201 f0=0 f1=1 e=202 f2=0 g=-1 eg=9
                          ^^^^^^^^^^^^^
```

`dup3(fd, 200, 0x40000000)` must be `EINVAL` — `O_CLOEXEC` is the only legal `dup3` flag. The engine
returned the new descriptor instead.

Mechanism: `translator/guest/x86_64/legacy.c` rewrote the x86-only `dup2(2)` into the canonical `dup3`
form and signalled "this was really a dup2" by setting **bit 30 of the flags argument**, which the
shared handlers (`linux_abi/syscall/io.c` case 24, `linux_abi/syscall/binding.c`) then stripped before
validating. A guest that passes `0x40000000` itself is indistinguishable from the rewrite, so it got
`dup2` semantics — including the `oldfd == newfd` no-op that `dup3` must reject.

Fix: the signal moved out of band, mirroring the existing `g_x86_forksave` idiom — legacy.c keeps a
per-thread `g_x86_dup2_compat`, refreshed on every normalized syscall, exposed as
`hl_x86_legacy_is_dup2()` and consumed through the `G_IS_DUP2_COMPAT()` seam (`0` for aarch64 guests,
which have no legacy `dup2`). The flags argument now reaches the validator exactly as the guest wrote it.

## 2. `signalfd` reported one siginfo record too many, with the wrong `si_code`

Case: `tests/compat/signals/signalfd_state.c` (`signalfd-state`)

```
native  r0=-1 e0=11 r1=128 signo=10 code=-6 r2=-1  e2=11 ...
engine  r0=-1 e0=11 r1=128 signo=10 code=0  r2=128 e2=0  ...
                                    ^^^^^^  ^^^^^^^^^^^
```

Two independent bugs in one line.

* **Duplicate wake byte.** `sigq_push` correctly coalesces a second pending instance of a *standard*
  (non-realtime) signal, but `raise_guest_signal_si` wrote the signalfd self-pipe wake byte
  unconditionally. The dropped instance still left a byte in the pipe, so the second `read(2)` returned
  a fabricated 128-byte record where Linux returns `EAGAIN`. `sigq_push` now reports whether it actually
  enqueued, and `sfd_deliver` is gated on that.
* **`si_code` for self-directed `tkill`/`tgkill`.** Linux stamps `SI_TKILL` (-6) for both syscalls
  regardless of target, and glibc's `raise()` lowers to `tgkill`. `thread_kill` only stamped `SI_TKILL`
  when routing to *another* thread; the self-signal fell through to `raise_guest_signal`, which stamps
  `SI_USER` (0). It now calls `raise_guest_signal_si(..., HL_SI_TKILL, ...)` directly.

## 3. `read(2)` into a buffer straddling a guest `PROT_NONE` page was all-or-nothing

Case: `tests/compat/memory/guard_page_efault.c` (`guard-page-efault`)

```
native  rd=8  erd=0  wr=-1 ewr=14 fit=8 wfit=8 v=-1 ev=14 g=-1 eg=14 ecwd=34 ecwd2=14
engine  rd=-1 erd=14 wr=-1 ewr=14 fit=8 wfit=8 v=-1 ev=14 g=-1 eg=14 ecwd=34 ecwd2=14
        ^^^^^^^^^^^^
```

hl force-maps guest anonymous pages host-writable and models guest `PROT_NONE` in the `g_gna` registry,
so `linux_abi/syscall/io.c` rejected `read/write/pread/pwrite` with `EFAULT` whenever `gna_hit()` found
*any* overlap. Linux's `copy_to_user` is byte-granular: a `read(2)` copies the good prefix and returns
that **short count**, reporting `EFAULT` only when nothing at all could be copied.

Fix: a new `gna_prefix()` helper (`linux_abi/thread.c`) returns the leading non-`PROT_NONE` byte count;
the read family clamps its count to it and only `EFAULT`s on an empty prefix. The write family keeps the
all-or-nothing check deliberately — the native oracle shows Linux's pipe/`writev` paths report `EFAULT`
for the whole call rather than a short write, and the same case pins both behaviours.

## 4. `/proc/self/comm` was not writable

Case: `tests/compat/procfs/comm_status.c` (`pf-comm-status`)

```
native  ... w=12 after=written-name
engine  ... w=12 after=abcdefghijklmno
                 ^^^^^^^^^^^^^^^^^^^^^
```

Writing `/proc/self/comm` renames the task on Linux exactly as `prctl(PR_SET_NAME)` does, and the new
name is immediately visible through `PR_GET_NAME` and `/proc/self/{comm,status:Name,stat}`. The engine
accepted the write into the synthetic backing file and dropped it.

Fix: a write intercept on the `self:comm` tagged descriptor in `linux_abi/syscall/io.c`, mirroring the
existing `self:oom_score_adj` handler — it truncates to `TASK_COMM_LEN-1`, drops one trailing newline,
updates `g_procname` + `set_guest_comm_name()`, re-renders the backing file, and returns the full count.
`synth_stat_raw` now reports mode 0644 for `/proc/self/comm`.
