# The checkpoint sink

The checkpoint writer no longer calls the filesystem. Every byte of a checkpoint image is emitted through a
narrow internal interface, `struct ckpt_sink` (`src/linux_abi/ckpt_sink.h`), and read back through its
symmetric counterpart, `struct ckpt_source` (`src/linux_abi/ckpt_source.h`). There are two implementations of
each: the directory sink/source (`ckpt_sink_dir.h`), which reproduces the historical on-disk format byte for
byte and reaches the host only through `hl_host_services->file`, and the streaming sink/source
(`ckpt_sink_stream.h`), which reaches an embedder-supplied store over a socket and touches no filesystem.

An embedder therefore decides *where* and *how* checkpoint bytes are stored: implement `CheckpointStore` in
the Rust crate and launch with `Engine::spawn_with_store`. See "What a caller can and cannot do" below for
the boundaries this has today.

## The interface

```c
int  begin(sink, group, name, flags, &stream);   // start a named object
int  write(stream, data, size);                  // append
int  write_at(stream, offset, data, size);       // patch bytes already emitted
int64_t tell(stream);                            // current logical end
int  finish(stream);                             // the object is now complete and durable
void abort(stream);                              // discard the object

int  group_begin(sink, group);                   // start an all-or-nothing set of objects
int  group_commit(sink, group);                  // publish the whole set
void group_abort(sink, group);                   // discard the whole set

int  claim(sink, name);                          // 0 acquired, 1 already held, -1 error
void unclaim(sink, name);

int  commit(sink, manifest, size);               // COMPLETION: the image is finished
```

Four concepts, and each exists because the writer genuinely needs it:

- **Object** — a named byte stream. `pages`, `cpu`, `fds`, `meta`, `inotify`, `signals`, per-process epoll and
  inotify sidecars, plus workspace-level shared records (`pipe.<id>`, `signalfd.<id>`, `socket.<id>`,
  `socket-state.<id>`, `file.<pid>.<fd>.<seq>` blobs).
- **Group** — one guest process's image. Today `proc.<gpid>`. Its members must appear together or not at all,
  because the coordinator treats the appearance of `proc.<gpid>` as "that peer finished". The directory sink
  implements a group as a `proc.<gpid>.tmp.<pid>` staging directory renamed into place.
- **Claim** — several engine processes can see the same shared kernel object (both ends of a pipe, both ends
  of a socketpair). Exactly one of them must write the record. `claim` elects that writer; "already held" is a
  normal, non-error outcome that makes the loser skip the object.
- **Commit** — see below.

`write_at` + `tell` exist for exactly two writers that emit a header before they know its final contents: the
sparse page dump (a `ckpt_region` header patched with `npages` once the region's non-zero pages are known) and
the socket-queue capture (a header patched with `peer_closed` once the drain loop ends). They replace the old
`fseeko`/`ftello` back-patching. A sink that cannot seek must buffer the object until `finish`.

## Commit semantics

Historically completion was a filesystem *side effect*: `MANIFEST` was written last, and its mere presence in
the workspace meant "this checkpoint is complete and restorable". Both the restore path
(`ckpt_read_manifest`) and the Rust `MachineSpec::checkpoint` poll for that file.

A callback sink has no equivalent of "a file atomically appeared", so completion is now an explicit call:

> `commit(sink, manifest_bytes, size)` is the single, final operation of a capture. It is called by the
> container init (guest pid 1) after every peer group and its own group have been committed. Nothing may be
> emitted afterwards.

The directory sink implements `commit` as exactly what happened before — write `MANIFEST` last, then fsync the
workspace directory — so the on-disk format and its ordering are unchanged.

Ordering guarantees the writer relies on, which every implementation must honour:

1. an object is complete only after `finish`;
2. a group's objects are invisible until `group_commit`;
3. `commit` happens last, after every object and group of the image.

## Failure semantics

- **Object failure.** Any failed `write`/`write_at` poisons the stream; the writer calls `abort` and fails its
  caller. The directory sink unlinks the staging file. No partially written object is ever published.
- **Group failure.** A failed object inside a process image aborts the whole group (`group_abort`), and
  `ckpt_dump_self` returns failure. The process exits non-zero without publishing `proc.<gpid>`.
- **Peer failure.** The coordinator waits for every peer's group to appear; if one does not,
  it refuses to publish the manifest and exits 70. The workspace is left holding whatever complete groups did
  appear, but **without** a manifest — which by rule (3) means "not a checkpoint". A partially written
  workspace is therefore inert, not dangerous: restore refuses anything without a manifest.
- **Commit failure.** Same outcome: no manifest, no checkpoint.
- **Nothing is rolled back.** Cleaning up the debris of a refused capture is the caller's job, exactly as it
  was when the debris was a directory.

## The streaming sink: an embedder-supplied store

The sink now has a second implementation, `src/linux_abi/ckpt_sink_stream.h`, which touches no filesystem at
all: every operation becomes a request on a socket, and a server in the Rust crate replays it onto a
caller-supplied `CheckpointStore` (`pkgs/rust/src/checkpoint_stream.rs`). It is selected by passing the
sentinel `@hl-checkpoint-stream` (`HL_CKPT_STREAM_SENTINEL`) where a workspace directory would go, which is
what `Engine::spawn_with_store` does. The sentinel is never opened; it is a discriminator, so no launch-config
ABI field was added for it.

### Transport

`hl_activation_start_*` re-executes a separate engine executable and every guest process is a further
`fork()` of it, so the bytes are produced by N host processes that share no address space with the caller.
The transport (`include/hl/checkpoint_stream.h`, `src/core/checkpoint_channel.c`) is therefore two-level:

- a **broker**, one `SOCK_DGRAM` descriptor handed to the engine at activation with `SCM_RIGHTS` and
  inherited by every `fork()`. It carries one message kind, `hl_ckpt_hello`, with one attached descriptor.
  Datagram framing makes concurrent announcements from arbitrarily many engine processes atomic;
- a **channel** per engine process, created by that process and passed to the server over the broker. It is
  strictly request/response and strictly serial — a dumping process is at a safepoint with one thread — so
  concurrency between processes is demultiplexed by *having one channel per process*. There are no request
  tags, because a channel never has more than one request outstanding.

Both the protocol and the activation request are versioned (`HL_CKPT_STREAM_ABI`, activation ABI 2, which
tags its inherited descriptors by role so a launch can carry a provider transport, a checkpoint broker and a
trigger page independently). A mismatch fails the capture rather than producing an unreadable image.

The **trigger** — the shared generation counter `ckpt_poll` reads at every safepoint — was a file next to the
workspace. With no workspace it is an anonymous shared mapping (`memfd_create`, or an immediately unlinked
POSIX segment on macOS) whose descriptor is inherited exactly like the broker. It has to stay a plain memory
load: it is read on the dispatcher's hot path, so it cannot become a message.

## Decisions that were not plumbing

### Rendezvous

The coordinator used the workspace as its synchronisation medium: `access("<dir>/proc.<gpid>")` to detect a
finished peer, `opendir` to count published images.

**Decision: rendezvous is a sink query, not a store observation.** `group_present` and `group_count` joined the
vtable. The *definition* is unchanged — a peer is finished when its group is committed — but it is now asked
of the participant that actually observes every `group_commit`, instead of inferred from a directory entry
appearing. The directory sink answers with the same `access`/`opendir` it always used, so its behaviour and
its timing are identical; the streaming server answers from its own set of committed groups, which is
authoritative rather than eventually-consistent. The alternative — a separate control channel — would have
introduced a second ordering to reason about between "committed" and "announced"; there is only one.

### Digest

`ckpt_image_digest` walked and *re-read* the finished workspace to hash it into the manifest, and restore
recomputed it. A stream cannot be walked, and asking an embedder's store to be re-read defeats the point.

**Decision: the digest is two-level, and both sinks compute the same value.**

```
per object : h = FNV1a(name '\0' || u64 size || contents)
image      : H = FNV1a over (name '\0' || u64 h) for every object, in ascending name order
```

The per-object hash is accumulable by a writer that sees the object exactly once; the image hash needs only
the `(name, h)` pairs, which the server holds as objects are finished. Ascending name order makes the fold
independent of the order in which concurrent peers happen to emit — which the old single running hash was
not, once "walk the directory" stopped being available to impose an order.

Consequences, taken deliberately:

- the manifest digest changes value, so `CKPT_VERSION` is **2**. Images written by earlier builds are refused
  rather than mis-authenticated. This codebase has already shipped one incident from a tolerated format skew;
- the directory sink adopts the new algorithm too, so there is exactly one digest and no format flag. It
  still computes it by walking, because it can, and that keeps the on-disk format self-describing;
- the digest is requested through `sink->digest` / `source->digest`, so neither side re-reads the store.

## Restore: the source

`src/linux_abi/ckpt_source.h` is the read half, with the same two implementations. It is deliberately *not* a
mirrored byte stream: restore opens objects by name, seeks inside them, and enumerates the image to discover
the process tree, so the source is `size` / `read(offset)` / `list(prefix)` / `digest`.

The restore driver reaches it through a `FILE*`: `ckpt_source_fopen` is a plain `fopen` on the directory
source and a memory stream over materialised bytes on the streaming source. That was a deliberate trade —
converting ~40 `fread`/`fseek` call sites to an explicit cursor API is a large change whose correctness could
only be argued by inspection, while the `FILE*` seam leaves the directory path byte-for-byte what it was. The
cost is real and is stated where it is paid: with a streaming source, one object at a time is resident in the
restoring process, bounded by the largest single object (a process's `pages` image).

`ckpt_scan_procs` (which was `opendir` over `proc.*`) and the manifest digest now go through the source too.

## What a caller can and cannot do

Can:

- implement `CheckpointStore` (`put` / `get` / `list` / `commit`) and capture a real multi-process guest into
  it, then restore that guest from it, with no checkpoint directory anywhere. `MemoryStore` is provided;
- rely on only ever seeing complete objects, and on a process image appearing all at once — the server does
  the staging, so an embedder never has to implement group atomicity;
- rely on `commit` being the single completion signal. `Machine::checkpoint_into_store` returns when the
  store has been committed, not when a file appeared;
- fail. An error from any method fails the capture, and nothing is committed. Whatever the store already
  accepted is debris the caller discards; the engine never calls back to undo it.

Cannot, as it stands:

- stream an object incrementally into the store. The server buffers each object until the engine finishes it,
  because `write_at` back-patching has to land somewhere and the engine may not seek in the embedder's store.
  A large `pages` image is therefore resident in the server while it is being written;
- use a caller-supplied store together with a controlling terminal or a provider transport.
  `Engine::spawn_with_store` uses the stdio form of activation; the descriptor roles support the combination,
  the Rust entry point does not expose it yet;
- avoid the host filesystem entirely for *guest* state. Restoring an unlinked regular file still stages its
  contents through a host temporary file, because the guest needs a real descriptor to a real file. That is
  guest state being reconstructed, not the checkpoint image being stored, but it is a host write.

## Raw filesystem calls left in the writer

Zero image bytes are written raw: every byte now goes through the sink and therefore through
`hl_host_services->file`. The calls that remain in the writer region of `src/linux_abi/checkpoint.c`, and why:

| Site | Calls | Why it is still raw |
|---|---|---|
| `ckpt_map_trigger` | `open`, `ftruncate`, `mmap` | The `<dir>.trigger` shared generation counter is a control channel, not image data, and is mapped `MAP_SHARED` by every engine process *and* written by the Rust caller. The host contract has shared memory by anonymous identity, not a named, externally-writable shared page. |
| `ckpt_image_digest` / `ckpt_hash_tree` | `opendir`, `readdir`, `closedir`, `lstat`, `open` | Re-reads the finished workspace to hash it into the manifest. This is image *enumeration and readback*, which the sink deliberately does not express. |
| `ckpt_rmrf` | `opendir`, `readdir`, `closedir`, `rmdir`, `unlink` | Recursive removal of a stale staging directory. The host contract has `remove_directory` and `unlink_relative` but no recursive removal, and `read_directory` needs an open handle plus its own cursor management; converting it is a separate change with no correctness benefit here. Used only by the directory sink. |
| `ckpt_coordinate_and_exit` | `mkdir`, `access`, `opendir`, `readdir`, `closedir` | The coordinator's peer rendezvous: it detects that a peer finished by the appearance of `proc.<gpid>` and counts published images by listing the workspace. This is synchronisation through the store, not image I/O. |
| fd scan (`ckpt_normalize_reopen_path` and the descriptor viability probes) | `access` | Probes **guest** paths, not the checkpoint image. |

Counting textually over the writer region, raw filesystem calls went from **75 to 27**, and all 27 are in the
five rows above. `ckpt_close_sync`/`ckpt_sync_dir` also survive in that text range but are now used only by the
restore-side `RECOVERY.jsonl` writer, which this change did not touch.
