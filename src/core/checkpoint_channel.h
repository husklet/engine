/* hl/core -- the engine-process side of the checkpoint stream transport (include/hl/checkpoint_stream.h).
 *
 * Lives in core rather than in linux_abi because two separately linked translation units need it: the
 * activation child (src/core/activation.c), which receives the broker descriptor over SCM_RIGHTS and
 * publishes it here, and the checkpoint writer (src/linux_abi/checkpoint.c, compiled into the per-target
 * unity TU), which consumes it. It is deliberately free of engine state: a descriptor and a per-process
 * lazily created channel, nothing else. */

#ifndef HL_CORE_CHECKPOINT_CHANNEL_H
#define HL_CORE_CHECKPOINT_CHANNEL_H

#include <stddef.h>
#include <stdint.h>

#include "hl/checkpoint_stream.h"

/* Activation publishes the inherited broker descriptor exactly once, before the guest starts. It is
 * inherited by every fork() of the engine, which is what lets a peer process reach the embedder's store. */
void hl_ckpt_channel_publish(int broker);
int hl_ckpt_channel_broker(void);

/* This process's private request/response channel, created on first use and re-created after a fork so a
 * child never shares a channel (and therefore never interleaves a request) with its parent.
 * Returns -1 when no broker was published (the directory sink is in use) or the connect failed. */
int hl_ckpt_channel_acquire(void);

/* One round trip. `name` (or NULL) is sent NUL-terminated; `payload` is `request->length` bytes. Reply
 * payload is copied into `out` (up to `capacity`); a longer reply is a protocol error. Returns 0 when a
 * well-formed reply arrived -- the operation's own status is in `reply->status` -- and -1 when the transport
 * or the framing failed. */
int hl_ckpt_channel_call(hl_ckpt_request *request, const char *name, const void *payload,
                         hl_ckpt_reply *reply, void *out, size_t capacity);

/* Embedder side, called from the Rust FFI boundary. `hl_ckpt_broker_pair` creates the datagram socketpair
 * whose child end is handed to activation; `hl_ckpt_broker_accept` waits up to `timeout_ms` for one engine
 * process to announce itself and returns its channel descriptor (-1 on timeout or error). */
/* The checkpoint TRIGGER is a 4-byte generation counter shared by every engine process and bumped by the
 * embedder to request a capture. ckpt_poll reads it at every safepoint, so it has to be a plain memory load;
 * it cannot be a message. With a workspace directory it is a mapped file next to the workspace. With a
 * streaming sink there is no directory, so it is an anonymous shared mapping whose descriptor activation
 * hands to the engine exactly like the broker. */
void hl_ckpt_trigger_publish(int descriptor);
int hl_ckpt_trigger_descriptor(void);

int hl_ckpt_broker_pair(int *out_parent, int *out_child);
int hl_ckpt_broker_accept(int broker, int timeout_ms, uint64_t *out_host_pid);

/* Embedder side of the trigger: create the shared counter, read and bump it, release it. */
int hl_ckpt_trigger_create(int *out_descriptor, void **out_mapping);
uint32_t hl_ckpt_trigger_bump(void *mapping);
void hl_ckpt_trigger_destroy(void *mapping, int descriptor);

#endif
