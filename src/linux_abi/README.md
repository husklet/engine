# Linux ABI

This directory implements the Linux behavior visible to guests. Host backends
provide primitives; this layer owns Linux flags, errors, structures, container
rules, descriptors, and checkpoint semantics.

## Checkpoint contract

Checkpoint bytes pass only through `ckpt_sink` and `ckpt_source`; the embedder
chooses their storage. An object becomes visible at `finish`, a process image
becomes visible at `group_commit`, and the manifest `commit` is the final
operation. Without that manifest, partial groups are not a checkpoint and
restore refuses them.

Restore validates the complete image before mapping memory or forking. External
files and devices reconnect to current host state; the engine never rolls back
mounted data. The recovery policy may refuse the image or stop a nonviable
process subtree, but container init is always required. Every restore records
its decisions in `RECOVERY.jsonl`.

`ctest --test-dir <build> -L checkpoint --no-tests=error` runs the process,
descriptor, corruption, and recovery gates.
