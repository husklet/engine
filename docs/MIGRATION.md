# Engine capability migration ledger

This ledger records observable behavior before an old launch option or domain special case is
removed. A checked item means the behavior has a typed owner and compatibility coverage; it does
not mean every backend implements the capability.

| Legacy input or special case | Observable consumers | Classification | Typed destination | Status |
| --- | --- | --- | --- | --- |
| `HL_CWD`, `HL_GUEST_ENV`, `HL_UID`, `HL_GID`, `HL_HOSTNAME` | ELF stack, exec, procfs, credentials and UTS syscalls | core process/identity policy | `ProcessSpec`, `IdentitySpec` | [x] typed launch; legacy instance store remains |
| `HL_MEM_MAX`, `HL_PIDS_MAX`, `HL_CPUS`, `HL_ULIMITS` | allocation, fork/clone, affinity, procfs and rlimit syscalls | core resource policy | `ResourceSpec`, `CpuSpec` | [x] typed validation/lowering for implemented limits; [ ] remove option reads |
| `HL_VOLUMES`, `HL_LOWER`, `HL_ROOTFS_RO`, `HL_FILE_OWNERS` | all VFS path operations, mountinfo, stat and access checks | core filesystem policy | `FilesystemSpec` | [x] typed host binds/root/ownership; [ ] typed overlays |
| extension immutable files/directories/symlinks | open/stat/readlink/readdir/canonicalization through the normal VFS | generic namespace capability | `ExtensionSpec.namespace` | [x] `engine.namespace` v1 runtime projection and real guest test |
| extension read-only host binds | open/stat/readlink/readdir and live host-file coherence through the normal VFS | generic namespace capability | `NamespaceEntry::HostBind` | [x] regular files/directories; writable and special nodes rejected |
| extension mutable byte files | read/write, positioned I/O, truncate, stat and shared mmap across opens/fork | generic namespace capability | `FileSource::Mutable` | [x] bounded launch-private backing with lifecycle cleanup |
| `HL_FSGEN_FILE` | overlay cache invalidation after external writes | filesystem coherence policy | `CoherenceHandle` | [x] opaque typed launch handle; [ ] replace backend generation-file option |
| `HL_NET_ISOLATE`, `HL_NETNS`, `HL_NETBR`, `HL_IP`, `HL_NETIFS`, `HL_PUBLISH*` | sockets, DNS, interface/ioctl views, IPC identity, port forwarding | core networking policy | `NetworkSpec` | [x] typed launch; [ ] typed live route/egress update |
| `HL_EGRESS_SOCKS` | external TCP connect path | provider-backed networking transport | `NetworkSpec.egress` plus transport provider | [ ] |
| `HL_PCACHE`, `HL_PCACHE_DIR` | translation lookup/store and fork behavior | core translation-cache policy | `TranslationCacheSpec` | [x] typed launch; [ ] capability-backed store |
| `HL_CHECKPOINT_DIR`, `HL_RESTORE_DIR` | process/memory/fd snapshot and restore dispatch | checkpoint protocol | `CheckpointSpec` and source/sink controls | [ ] backend is intentionally undiscovered |
| `HL_LOG` | translator and runtime debug output | bounded observability/debug policy | `ObservabilitySpec` | [ ] debug-build ambient compatibility only |
| `HL_ACTIVATION_FD` | bootstrap control descriptor | internal activation transport | private activation argument/handle | [ ] only non-debug ambient host read |
| hardcoded `/proc`, `/sys`, `/dev`, `/tmp`, `/run` personalities | ordinary Linux applications and container probes | core Linux VFS mounts | typed standard mount configuration | [ ] names remain in VFS implementation |

## Ambient configuration boundary

`src/core/options.c` is the authoritative compatibility registry. Launch records are copied into an
engine-instance-owned option store; Linux and translator code still read that store using legacy
`HL_*` keys. Direct process-environment reads are confined to `src/core/environment.c`: debug-only
`HL_LOG` and internal activation transport `HL_ACTIVATION_FD`. New public behavior must enter through
typed launch or control APIs and must not add another ambient variable.

## Removal sequence

For each unchecked row: capture a real consumer test, classify ownership, add the narrow typed port,
lower it without bypassing the coherent subsystem, run compatibility tests, then delete the legacy
key/path/branch. Discovery reports runtime support only after the compatibility test passes.
