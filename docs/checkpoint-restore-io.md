# Checkpoint/restore IO contract

Checkpoint images contain engine-owned process state. They do not snapshot mounted volumes or other external
host resources. Restore validates the complete image before mapping guest memory or forking the process tree,
then reconnects external resources to their current host state.

## Recovery policies

- `refuse`: any nonviable process or required resource refuses the whole restore.
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

Established or in-progress connections that cannot be transferred are refused under `refuse`. Under permissive
capture they restore as disconnected sockets with pending `ECONNRESET`, allowing application retry logic.

## Image durability

- Each process image is published by temporary-directory rename after all files are synchronized.
- `MANIFEST` is synchronized and published last.
- The manifest authenticates the path, size, and content of every engine-owned image file.
- Modified, truncated, missing, and unexpected image files are rejected before runtime mutation.
- Every process image is semantically checked for metadata identity, CPU layout and leader, memory-region and
  sparse-page bounds, descriptor count/range/kind, external-resource viability, and queued rights.
- A completed image is reusable; repeated restores do not modify its authenticated contents.

## Release gate

Run:

```sh
make e2e-checkpoint-io-full
```

The target is fail-fast and runs the IO/recovery matrix on AArch64 and x86_64 plus the existing process-tree,
thread, signal, anonymous-object, epoll, socket, network fallback, strict-refusal, and corruption suites.

No finite test suite guarantees correctness under every host failure. The current gate does not simulate host
kernel failure, physical device removal during an individual `open`, network-filesystem server failure during
restore, disk failure after successful `fsync`, or an external actor changing a path in the interval between
preflight and reopen. Those races fail the affected restore operation; they do not cause the engine to restore
old external contents.
