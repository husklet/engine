# Engine

This component owns the execution libraries: core lifecycle and providers,
Linux ABI emulation, guest translation, and host backends. The repository root
orchestrates products, tests, packaging, and cross-target builds.

Preserve Linux behavior rather than adding application-specific paths. Chrome
is the primary stress workload and depends especially on:

- exact errno, partial I/O, nonblocking, poll/epoll, futex, timer, pipe, Unix
  socket, and `SCM_RIGHTS` behavior;
- complete fork, clone, exec, signal, wait, descriptor inheritance, `/proc`,
  namespace, sandbox, TLS, and dynamic-loader behavior;
- coherent shared mappings, `memfd`, protection changes, executable mappings,
  descriptor offsets, and backing-storage lifetimes;
- generic projection of devices, sockets, descriptors, shared memory, Wayland,
  and graphics services with stable identity and bounded resource use; and
- bounded diagnostics, synchronization, retries, and GPU work. Hot paths must
  not busy-spin, log per operation, or serialize unrelated processes.

Acceptance requires an unmodified sandboxed `google-chrome` launch, responsive
navigation and input, accelerated rendering, stable long-running resource use,
and focused regressions for every engine defect found by that workload.
