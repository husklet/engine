# Checkpoint/restore IO contract

Checkpoint images contain engine-owned process state. They do not snapshot mounted volumes or other external
host resources. Restore validates the complete image before mapping guest memory or forking the process tree,
then reconnects external resources to their current host state.

## Recovery policies

Selected by `HL_CHECKPOINT_POLICY` (`ckpt_recovery_policy`, `src/linux_abi/checkpoint.c`). An unset policy
restores as `discard-optional`: a restore deliberately restores everything it can and stops what it cannot; a
stopped subtree is the designed outcome, not a failure or a gap to be closed later. Capture is unaffected by
the default — capture only relaxes when a permissive policy is asked for explicitly.

- `refuse` (explicit only): any nonviable process or required resource refuses the whole restore.
- `reconnect`: reconnect path-backed resources where possible and stop nonviable process subtrees.
- `discard-optional`: reconnect reconstructible resources and stop nonviable process subtrees.
- Container init is mandatory under every policy. If init is nonviable, restore is refused.
- Stopping a process also stops all of its descendants. Unaffected ancestors and sibling subtrees may resume.

Every restore writes `RECOVERY.jsonl` atomically. It records process outcomes, top-level descriptors, and
descriptors queued in Unix-socket `SCM_RIGHTS` messages.

## External files and volumes

- Regular files and directories are reopened by path. Current host contents are authoritative.
- Same-inode modification, atomic path replacement, and delete/recreate are accepted when the resulting path
  has the expected type and can be opened with the saved access mode.
- Saved offsets, `O_APPEND`, descriptor flags, and shared open-file descriptions created by `dup` are restored.
- A shortened file keeps the saved offset; reads beyond its current end return EOF.
- A missing path, file/directory type change, or access-mode failure makes the owning process nonviable.
- Deleted or pathless regular files captured as image blobs are reconstructed from the image.
- Directory descriptors reconnect to the current directory and observe files created after checkpoint.

The engine never rewrites, rolls back, or replaces external volume contents.

## Devices and terminals

- Path-backed character and block devices are reopened only when the path still has a device type and the saved
  access mode succeeds.
- Device descriptors queued through `SCM_RIGHTS` use the same validation and reconnect behavior.
- Controlling terminals are inherited from the restore launcher and process-group ownership is reconstructed.
- Named FIFOs are explicitly refused. Anonymous engine-managed pipes are reconstructed.

## Reconstructed IO objects

The release gate covers:

- anonymous pipes, unread bytes, EOF, aliases, and cross-process endpoints;
- memfd contents, mappings, offsets, aliases, and seals;
- eventfd counters, semaphore mode, nonblocking mode, aliases, and cross-process sharing;
- timerfd deadline, interval, pending expirations, aliases, and clock identity;
- signalfd masks, queued signals, flags, and descriptors queued through `SCM_RIGHTS`;
- inotify watches, queued events, aliases, and descriptors queued through `SCM_RIGHTS`;
- epoll aliases, level/edge/oneshot state, watched objects, and queued epoll descriptors;
- Unix stream and seqpacket socket pairs, unread frames, EOF, and queued descriptor graphs;
- standalone UDP, Unix listeners, socket options, connected internal sockets, and connection fallback.

Established or in-progress connections that cannot be transferred are refused at capture time unless a
permissive policy was requested explicitly. Under permissive capture they restore as disconnected sockets with
pending `ECONNRESET`, allowing application retry logic.

## Image durability

Ordering is expressed through explicit group and commit calls — see docs/checkpoint-sink.md.

- Each process image is published all-or-nothing by `group_commit`.
- `commit` carries the `MANIFEST` and happens last.
- The manifest authenticates the name, size, and content of every engine-owned image object.
- Modified, truncated, missing, and unexpected image objects are rejected before runtime mutation.
- Every process image is semantically checked for metadata identity, CPU layout and leader, memory-region and
  sparse-page bounds, descriptor count/range/kind, external-resource viability, and queued rights.
- A completed image is reusable; repeated restores do not modify its authenticated contents.

## Guest virtual addresses are pinned by the image, and restore takes them unconditionally

A guest `mmap` result is an ordinary host `mmap` result: guest anonymous and file mappings are not confined to
a reserved band the way the image, brk heap and initial stack are (`g_force_base` / the checkpoint heap base).
So the saved VA of a guest mapping is whatever the CAPTURING process's host allocator happened to return, and
restore reproduces it with `MAP_FIXED` — which silently replaces anything the RESTORING process has there.

Two consequences, one fixed and one open.

- **Fixed.** A re-forked restore child ran the shared after-fork engine reset *after* its memory restore, and
  that reset rebuilds the translated-code arena at a fresh VA and unmaps the ~64 MiB pair inherited from the
  restoring parent. The child's `MAP_FIXED` regions routinely land inside that inherited arena, so the release
  punched the just-restored guest pages back out. `checkpoint.x86_64.threads` died with a host `SIGSEGV` on
  the resumed peer's own stack (`si_addr == sp`, guest pc at glibc's `__syscall_cancel_arch_end`), 10/10
  reproducible. The hook now runs *before* the memory restore, so every mapping it is going to drop is dropped
  while the guest's VA is still free.
- **Open.** Nothing prevents the residual case. Restore now probes each region with `MAP_FIXED_NOREPLACE`
  first and, on `EEXIST`, prints `[restore] guest region <a>+<len> overlaps a live host mapping; reclaiming
  it` plus the `/proc/self/maps` rows in the way, then proceeds with `MAP_FIXED` — the guest's own pointers
  are unrelocatable, so keeping its VA is the only option that can still work, but the collision is now named
  instead of corrupting engine state silently. It fires in no case of the `checkpoint` or `checkpoint-io`
  lanes; it does fire under `ckpt-cross` restoring an x86_64 threads image into the emulated AArch64
  host, where a `MAP_SHARED` guest region names a range `qemu-user` reserves and does not expose. The real fix
  is to give guest `mmap` a reserved band under `HL_CHECKPOINT`, exactly as the image/heap/stack already get,
  so a saved guest VA can never name host or engine memory on any host.

## Cross-backend restore

`struct cpu` **is** the format — `sizeof(struct cpu)` is written into each image and validated on restore —
and the reason is that the interpreter backend and the JIT backend must be able to read each other's guest
state. The `ckpt-cross` lane (`tools/checkpoint_cross_gate.sh`, `cmake/Phase3Gates.cmake` section 9c)
tests that directly: capture with one host backend, restore with the other, both directions, both guest ISAs.
It is skip-gated on `qemu-aarch64` plus the cross tree, and shares `docs/emulated-aarch64.md`'s boundary.

Measured: 11/11 green, including the `cycle` double round-trip in both directions. The x86_64
`interp-to-jit` threads cell is deliberately not registered — it fails on the address-space collision above,
not on CPU-state interchange, and whether a real AArch64 host collides there is unknown.

## Release gate

Run:

```sh
ctest --test-dir <build-dir> -L checkpoint      # or -L checkpoint-io for the 17 IO/recovery scenarios
```

The target is fail-fast and runs the IO/recovery matrix on AArch64 and x86_64 plus the existing process-tree,
thread, signal, anonymous-object, epoll, socket, network fallback, strict-refusal, and corruption suites, and
two cases that a single restore cannot cover:

- `cycle` — capture, restore, **capture the restored tree again**, restore that second image. A single restore
  cannot see a lossy capture, because whatever the image failed to carry the restored process usually still
  runs; the second image is written from state only the first restore reconstructed, so a drop propagates and
  the third launch notices. The fixture forks, so the re-forked-child restore path is covered too, and prints
  one `BOOT` line per genuinely fresh process plus a per-stage counter it only advances while running: two
  `BOOT` lines for one role means a restore relaunched instead of resumed, and a counter that did not grow
  means the restored process was not the one that had been running.
- `handler` — captured with a **signal handler frame live** on an alternate stack. `struct cpu` carries
  `alt_sp`/`alt_size`/`alt_flags`, `sig_depth`, `sig_defer`, `sig_defer_stack[]` and `sig_frame_sp[]`, and
  nothing exercised any of them across a restore. The restored handler must still be on the alternate stack,
  still hold its own locals, still run under the mask delivery installed, and its return must hand control
  back to the interrupted main flow with that frame intact.

No finite test suite guarantees correctness under every host failure. The current gate does not simulate host
kernel failure, physical device removal during an individual `open`, network-filesystem server failure during
restore, disk failure after successful `fsync`, or an external actor changing a path in the interval between
preflight and reopen. Those races fail the affected restore operation; they do not cause the engine to restore
old external contents.
