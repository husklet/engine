# Chrome compatibility

Chrome is a demanding Linux compatibility workload. It combines a sandboxed multi-process runtime,
shared memory, descriptor passing, asynchronous I/O, threads, signals, Wayland, and accelerated graphics.
Fix the underlying Linux or engine contract; do not add Chrome-specific execution behavior.

## Regression-sensitive contracts

These contracts have caused or can plausibly cause Chrome startup failures, stalls, extreme latency, or
host-wide freezes. Preserve them during engine refactors:

- [ ] Errno values remain guest Linux values. `EAGAIN`/`EWOULDBLOCK` (11) from nonblocking I/O is returned
      to the guest and never promoted to an engine fatal error.
- [ ] A forked child inherits every epoll interest, including one descriptor registered in multiple epoll
      instances with different masks and user data.
- [ ] Fork resets child thread identity and synchronization ownership; no child retains a vanished host
      worker, lock owner, stop-the-world owner, or parent-only thread ID.
- [ ] Guest buffers crossing translated pages or imported mappings are copied and validated as ranges, not
      assumed to be one contiguous host pointer.
- [ ] Writes performed by translated bulk operations and syscalls invalidate translated code with the same
      precision and ordering as ordinary guest stores.
- [ ] Mapping ownership survives split, protect, unmap, fork, and rollback without double release, stale
      aliases, or lost backing storage.
- [ ] Signal delivery and return preserve the guest ABI frame, register state, mask, alternate stack, and
      restart semantics for both ARM64 and AMD64 guests.
- [ ] Diagnostics never log per translated block, syscall retry, frame, or polling iteration unless a
      bounded diagnostic session explicitly enables them.

## Launch

- [ ] An unmodified supported installation starts with `google-chrome`, without compatibility flags.
- [ ] The browser, renderer, GPU, network, storage, and utility processes start and remain isolated.
- [ ] Dynamic linking, `dlopen`, TLS, executable mappings, and process startup work for every supported
      guest architecture.
- [ ] The Chromium sandbox works. Debugging with `--no-sandbox` is not acceptance evidence.
- [ ] Process IDs, thread IDs, `/proc`, namespaces, credentials, limits, process groups, and wait status are
      Linux-compatible.
- [ ] `fork`, `clone`, `exec`, descriptor inheritance, `CLOEXEC`, signals, and child cleanup remain correct
      under concurrent process creation.
- [ ] Terminating an exec updates its observable state and reaps its descendants; a killed Chrome process
      never remains `Running` or leaves an unowned translated process tree.

## Scheduling and I/O

- [ ] Nonblocking operations return the correct result and `errno`. In particular, `EAGAIN`/`EWOULDBLOCK`
      is expected control flow, not a fatal error.
- [ ] Partial I/O and `EINTR` preserve progress and retry semantics.
- [ ] `poll`/`epoll`, futexes, pipes, Unix sockets, `eventfd`, timers, and wakeups neither lose events nor
      busy-spin.
- [ ] Monotonic clocks and timer deadlines are accurate enough for animation, input, IPC, and frame pacing.
- [ ] Stop-the-world operations, process inventory, and checkpointing cannot deadlock forked or exiting
      threads.
- [ ] Translation and syscall diagnostics are bounded and disabled on hot paths by default.

## Memory and files

- [ ] `mmap`, `munmap`, `mprotect`, shared mappings, copy-on-write, and executable mappings preserve Linux
      visibility and lifetime rules.
- [ ] `memfd`, seals, shared file offsets, duplicated descriptors, and descriptor ownership work across
      processes.
- [ ] Atomic rename, locks, symlinks, permissions, directory enumeration, and filesystem notifications
      match Linux behavior.
- [ ] Guest pointers spanning translated or discontinuous mappings are validated and copied without
      truncation or false `EFAULT`.
- [ ] Resource limits reject abusive allocations before host work begins and do not destabilize the host.

## IPC and Wayland

- [ ] Unix socket ancillary data preserves `SCM_RIGHTS` descriptors until the receiver owns or rejects them.
- [ ] Socket readiness, peer shutdown, cancellation, and backpressure follow Linux semantics.
- [ ] Projected sockets name existing host endpoints and remain valid for the launch lifetime.
- [ ] Wayland requests, callbacks, object destruction, and descriptor transfers preserve protocol order.
- [ ] Input focus, keyboard state, pointer coordinates, popups, output scale, and presentation feedback are
      delivered without artificial polling or debounce.

## Accelerated graphics

- [ ] Generic engine APIs project devices, sockets, descriptors, memory, and provider services; they contain
      no ANGLE, Chrome, GL, Vulkan, or product policy.
- [ ] Render-node and graphics-library projections retain stable identity and permissions.
- [ ] Shared GPU buffers and synchronization descriptors remain valid while any process or host provider
      owns them.
- [ ] External allocation identity and generation come from an authoritative provider record, not
      guest-supplied metadata. Recycled, stale, forged, and generation-zero handles are rejected.
- [ ] Buffer release means every GPU consumer has finished reading it. Cross-process or cross-queue
      presentation uses an explicit fence or a bounded buffer ring; producer reuse cannot race scanout.
- [ ] Graphics capabilities are advertised only when allocation, import, synchronization, presentation,
      resize, release, and failure recovery are all available end to end.
- [ ] Provider memory validation, rollback, retention, and release are atomic across failed launches and
      rejected submissions.
- [ ] GPU submissions have bounded size and work, deterministic backpressure, and no partial mutation before
      a late validation failure.
- [ ] Browser GPU-process failure cannot poison later processes or leak unbounded host GPU resources.

## Performance and host safety

- [ ] Ordinary browsing, animation, scrolling, typing, video, Canvas, WebGL, and WebGPU do not fall back to
      software because of an engine limitation.
- [ ] Hot syscall, futex, memory, socket, and translation paths are measured under real Chrome workloads.
- [ ] No global lock serializes unrelated Chrome processes or threads.
- [ ] No per-operation device-wide GPU wait, full-frame readback, synchronous disk capture, or unbounded
      retry loop exists on a presentation path.
- [ ] CPU, memory, descriptor, thread, log, command, and provider-resource growth remain bounded during a
      long browsing session.
- [ ] A busy or malformed guest cannot stall the host UI, GPU, filesystem, or networking stack.

## Checkpoint and restore

- [ ] Checkpoint either preserves or explicitly rejects every live thread, descriptor, mapping, socket,
      process relationship, and projected provider resource.
- [ ] Restore retains terminal process groups, controlling-terminal state, signals, and pending I/O.
- [ ] External GPU and Wayland state is re-established through an explicit provider contract; opaque host
      resources are never assumed serializable.
- [ ] Unsupported live state fails before destructive mutation and reports the exact unsupported capability.

## Acceptance evidence

- [ ] Launch plain `google-chrome` in a clean workspace on every supported host and guest architecture.
- [ ] Load several real sites, navigate, type, scroll, open popups and additional windows, download a file,
      and run sustained animation.
- [ ] Confirm Chrome reports GPU compositing, rasterization, Canvas, WebGL, and supported WebGPU paths as
      hardware accelerated.
- [ ] Run representative WebGL conformance and stress content without context loss, GPU-process restart, or
      rendering corruption.
- [ ] Record input-to-present latency, frame cadence, CPU use, memory growth, GPU submission latency, and
      host responsiveness against a native baseline.
- [ ] Run for an extended period with no engine crash, guest crash, leaked process, resource growth, or host
      freeze.
- [ ] Test failures identify the violated generic engine contract and retain a focused regression.
