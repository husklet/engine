# The checkpoint sink

A checkpoint is not a directory. Every byte of an image is emitted through a narrow internal interface,
`struct ckpt_sink` (`src/linux_abi/ckpt_sink.h`), and read back through its symmetric counterpart,
`struct ckpt_source` (`src/linux_abi/ckpt_source.h`). There is exactly one implementation of each
(`ckpt_sink_stream.h`, and the stream half of `ckpt_source.h`): it reaches an embedder-supplied store over a
UNIX socket and touches no filesystem.

The embedder therefore decides *where* and *how* checkpoint bytes are stored: implement `CheckpointStore` in
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
  inotify sidecars, plus image-level shared records (`pipe.<id>`, `signalfd.<id>`, `socket.<id>`,
  `socket-state.<id>`, `file.<pid>.<fd>.<seq>` blobs).
- **Group** — one guest process's image. Today `proc.<gpid>`. Its members must appear together or not at all,
  because the coordinator treats the appearance of `proc.<gpid>` as "that peer finished".
- **Claim** — several engine processes can see the same shared kernel object (both ends of a pipe, both ends
  of a socketpair). Exactly one of them must write the record. `claim` elects that writer; "already held" is a
  normal, non-error outcome that makes the loser skip the object.
- **Commit** — see below.

`write_at` + `tell` exist for exactly two writers that emit a header before they know its final contents: the
sparse page dump (a `ckpt_region` header patched with `npages` once the region's non-zero pages are known) and
the socket-queue capture (a header patched with `peer_closed` once the drain loop ends). A sink that cannot
seek buffers the object until `finish`.

## Commit semantics

Completion is an explicit call, not a filesystem side effect:

> `commit(sink, manifest_bytes, size)` is the single, final operation of a capture. It is called by the
> container init (guest pid 1) after every peer group and its own group have been committed. Nothing may be
> emitted afterwards.

Ordering guarantees the writer relies on, which every implementation must honour:

1. an object is complete only after `finish`;
2. a group's objects are invisible until `group_commit`;
3. `commit` happens last, after every object and group of the image.

## Failure semantics

- **Object failure.** Any failed `write`/`write_at` poisons the stream; the writer calls `abort` and fails its
  caller. No partially written object is ever published.
- **Group failure.** A failed object inside a process image aborts the whole group (`group_abort`), and
  `ckpt_dump_self` returns failure. The process exits non-zero without publishing `proc.<gpid>`.
- **Peer failure.** The coordinator waits for every peer's group to be committed; if one is not, it refuses to
  publish the manifest and exits 70. The store is left holding whatever complete groups did arrive, but
  **without** a manifest — which by rule (3) means "not a checkpoint". A partial image is therefore inert, not
  dangerous: restore refuses anything without a manifest.
- **Commit failure.** Same outcome: no manifest, no checkpoint.
- **Nothing is rolled back.** Cleaning up the debris of a refused capture is the caller's job.

## Transport

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

The **trigger** — the shared generation counter `ckpt_poll` reads at every safepoint — is an anonymous shared
mapping (`memfd_create`, or an immediately unlinked POSIX segment on macOS) whose descriptor is inherited
exactly like the broker. It has to stay a plain memory load: it is read on the dispatcher's hot path, so it
cannot become a message.

The launch config carries only `checkpoint_mode` (`HL_CONFIG_CHECKPOINT_CAPTURE | _RESTORE`). There is no
path, and no magic string in a path field: the channel *is* the descriptor pair, so nothing has to be named.
The standalone engine takes the same two descriptors on its command line
(`--checkpoint-store <broker-fd> <trigger-fd>`), which is what the checkpoint gates use — `tests/integration/
checkpoint_tree_runner.c` is a store server in C, the twin of the Rust one.

## Decisions that were not plumbing

### Rendezvous

Rendezvous is a sink query, not a store observation. `group_present` and `group_count` are in the vtable: a
peer is finished when its group is committed, asked of the participant that actually observes every
`group_commit`. The alternative — a separate control channel — would have introduced a second ordering to
reason about between "committed" and "announced"; there is only one.

### Digest

The manifest carries a digest that restore recomputes. It is two-level so that it is accumulable by a writer
that sees each object exactly once and never re-reads the embedder's store:

```
per object : h = FNV1a(name '\0' || u64 size || contents)
image      : H = FNV1a over (name '\0' || u64 h) for every object, in ascending name order
```

Ascending name order makes the fold independent of the order in which concurrent peers happen to emit. The
digest is requested through `sink->digest` / `source->digest`; `MANIFEST` and the restore-side
`RECOVERY.jsonl` are excluded from the fold.

Version numbers (`src/linux_abi/checkpoint.c`): capture writes `CKPT_VERSION` = 1 and restore accepts
exactly that version.

## Restore: the source

`src/linux_abi/ckpt_source.h` is the read half. It is deliberately *not* a mirrored byte stream: restore opens
objects by name, seeks inside them, and enumerates the image to discover the process tree, so the source is
`size` / `read(offset)` / `list(prefix)` / `digest`.

The restore driver reaches it through a `FILE*`: `ckpt_source_fopen` is a memory stream over materialised
bytes. That was a deliberate trade — converting ~40 `fread`/`fseek` call sites to an explicit cursor API is a
large change whose correctness could only be argued by inspection. The cost is real and is stated where it is
paid: one object at a time is resident in the restoring process, bounded by the largest single object (a
process's `pages` image).

The restore-side `RECOVERY.jsonl` is written back through the sink, as an ordinary image object: restore binds
both halves of the same channel, so there is nowhere else for it to go.

## What a caller can and cannot do

Can:

- implement `CheckpointStore` (`put` / `get` / `list` / `commit`) and capture a real multi-process guest into
  it, then restore that guest from it. `MemoryStore` is provided;
- rely on only ever seeing complete objects, and on a process image appearing all at once — the server does
  the staging, so an embedder never has to implement group atomicity;
- rely on `commit` being the single completion signal. `Machine::checkpoint_into_store` returns when the
  store has been committed;
- fail. An error from any method fails the capture, and nothing is committed. Whatever the store already
  accepted is debris the caller discards; the engine never calls back to undo it.

Cannot, as it stands:

- stream an object incrementally into the store. The server buffers each object until the engine finishes it,
  because `write_at` back-patching has to land somewhere and the engine may not seek in the embedder's store.
  A large `pages` image is therefore resident in the server while it is being written;
- use a caller-supplied store together with a provider transport. `Engine::spawn_with_store` uses the stdio
  form of activation; the descriptor roles support the combination, the Rust entry point does not expose it
  yet (a controlling terminal does compose — see `a_store_captures_while_a_terminal_is_attached`);
- avoid the host filesystem entirely for *guest* state. Restoring an unlinked regular file still stages its
  contents through a host temporary file, because the guest needs a real descriptor to a real file. That is
  guest state being reconstructed, not the checkpoint image being stored, but it is a host write.

## Raw filesystem calls left in the writer

Zero image bytes are written or read raw: every byte goes through the sink or the source. The calls that
remain in `src/linux_abi/checkpoint.c`, and why:

| Site | Calls | Why it is still raw |
|---|---|---|
| `ckpt_map_trigger` | `mmap` | The shared generation counter is a control channel, not image data, and is mapped `MAP_SHARED` by every engine process *and* written by the caller. The descriptor is inherited; no path is opened. |
| `ckpt_source_copy_to_fd` / `ckpt_restore_file_blob` | `mkstemp`, `write`, `unlink` | Materializing a saved blob into a *guest* descriptor. The bytes come from the source; the temporary file exists because the guest needs a real descriptor to a real file. |
| fd scan (`ckpt_normalize_reopen_path` and the descriptor viability probes) | `access` | Probes **guest** paths, not the checkpoint image. |
