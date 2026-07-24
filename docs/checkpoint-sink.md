# The checkpoint sink

The checkpoint writer no longer calls the filesystem. Every byte of a checkpoint image is emitted through a
narrow internal interface, `struct ckpt_sink` (`src/linux_abi/ckpt_sink.h`), with exactly one implementation
today: the directory sink (`src/linux_abi/ckpt_sink_dir.h`), which reproduces the historical on-disk format
byte for byte and reaches the host only through `hl_host_services->file`.

This is the prerequisite for letting an embedder decide *where* and *how* checkpoint bytes are stored. It is
not itself that feature; see "What a callback sink would still need" below for the honest remaining work.

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

## What a future Rust callback implementation would have to provide

A caller-supplied sink must implement all of the above. Concretely:

- `begin`/`write`/`write_at`/`tell`/`finish`/`abort` per named object, with `write_at` able to patch an
  already-written range — either natively or by buffering the object in memory until `finish`.
- Group atomicity: buffer or stage the objects of `proc.<gpid>` and expose them only on `group_commit`.
- A `claim` primitive with test-and-set semantics that is **visible across host processes**, because the
  processes racing for it are separate host processes (see below).
- An explicit `commit(manifest_bytes)` call. It replaces the manifest-appeared signal end to end: the Rust
  `MachineSpec::checkpoint` currently polls for `<dir>/MANIFEST`, and that poll would become "the sink was
  committed".
- Partial failure: returning an error from any callback must leave the caller's store in a state it is willing
  to discard. The engine will not call back to undo anything.

### The blocker for a caller-supplied sink today

The Rust API does not run the engine in-process. `hl_activation_start_*` (`src/core/activation.c`)
`posix_spawn`s or `fork`+`execve`s a **separate engine executable**, and each guest process is a further
`fork()` of that child. At checkpoint time the bytes are produced by *N separate host processes*, none of
which shares an address space with the caller's Rust code. A Rust trait object therefore cannot simply be
handed to the writer; a callback sink needs, at minimum:

1. a transport established at activation and inherited across every guest `fork()` (a descriptor per engine
   process, or a multiplexed channel), plus a request/response wire protocol carrying begin/write/write_at/
   finish/group/claim/commit, tagged by process;
2. a Rust-side server that demultiplexes those requests onto the caller's trait implementation, since several
   engine processes dump concurrently;
3. cross-process `claim` arbitration on the Rust side (today the filesystem provides it via `O_EXCL`);
4. the coordinator's peer rendezvous re-plumbed. It currently uses the workspace itself as its synchronisation
   medium: `access("<dir>/proc.<gpid>")` to detect a finished peer and `opendir` to count published process
   images. With a non-filesystem sink those must become sink queries or a separate control channel;
5. the manifest digest re-plumbed. `ckpt_image_digest` walks and *re-reads* the written workspace to hash it
   into the manifest, and restore recomputes the same hash to authenticate the image. A streaming sink cannot
   be walked, so the digest must be accumulated as bytes are emitted, in an order both sides agree on;
6. a symmetric **source** interface for restore. Restore opens image files by name, reads them at arbitrary
   offsets, and enumerates the workspace (`opendir` over `proc.*`). Streaming a checkpoint out is only half
   the feature if it cannot be streamed back in.

Items 4, 5 and 6 are not plumbing — they are places where the current design uses the filesystem as a
*database* (listable, re-readable, randomly addressable), not as a byte sink. Each needs a design decision
before a callback sink can be honest rather than nominal.

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
