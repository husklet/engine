# Provider transport version 1

This transport is foundation work, not an advertised engine capability. Discovery remains unchanged
until native VFS open-file descriptions dispatch through it.

Every frame starts with a frozen 32-byte little-endian envelope:

| Offset | Width | Field |
| --- | --- | --- |
| 0 | 4 | magic `HLPR` (`0x484c5052`) |
| 4 | 2 | protocol version (`1`) |
| 6 | 2 | message type |
| 8 | 4 | payload length |
| 12 | 8 | request id |
| 20 | 8 | feature bits |
| 28 | 4 | reserved zero bytes |

Types are hello, ready, request, reply, cancel, close, namespace install/ready, subscribe,
unsubscribe, and readiness event. Subscription ids use the request-id field; readiness events carry
the encoded poll reply and therefore remain correlated without exposing guest descriptor numbers.
Unknown types, nonzero reserved bytes,
unknown versions, and oversized payloads fail before allocation or provider dispatch. Request ids
are channel-allocated and replies must match. Reads and writes have explicit deadlines; timeout,
malformed input, quota exhaustion, and peer death are distinct errors.

Channels own a Unix stream created before process creation or supplied explicitly by activation. No
ambient descriptor number enters the public API. `ProviderRegistry` maps stable provider ids to
`Handles` authority for one launch and bounds registrations. Hello/ready completes before requests.
Activation can now attach one borrowed transport endpoint to the otherwise unchanged launch request
with `SCM_RIGHTS`. The reexecuted child validates the attachment count, adopts the endpoint into the
engine-private descriptor range, and revokes and closes it during teardown. Launches without a provider
send the same ABI-1 request bytes and no ancillary rights.

This plumbing remains intentionally undiscovered. Native VFS service-node dispatch, transport-backed
request encoding, cancellation completion, and end-to-end open-description tests are still required
before the engine may advertise handle services.

Service payloads inside request/reply frames use a separately frozen little-endian codec. Operation tags
cover open, read, write, seek, stat, poll, and close; variable byte fields carry an explicit bounded length.
Decoders reject truncation, trailing bytes, invalid flags, and oversized buffers before provider dispatch.
Linux failures preserve errno and bounded UTF-8 context. The parent dispatcher alone owns provider
`OpenHandle` objects and opaque handle ids, enforces a per-launch handle quota and request limit, and closes
every remaining handle on peer teardown. `OpenHandle` now has narrow seek and metadata ports; its default
ioctl and mapping behavior is explicitly `ENOTTY` and `ENODEV` rather than an ambiguous protocol failure.

The provider server uses independent read and write endpoints so a cancel frame can be consumed while a
provider operation is running. It bounds active request ids, rejects duplicate and zero ids, applies a
server-side deadline, suppresses replies after cancellation, and revokes every remaining provider handle
on close or peer death. Cancellation does not expose guest pointers and never transfers guest fd numbers.
