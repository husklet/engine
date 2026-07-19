# HL engine protocol

`hl-engine-protocol` owns the frozen, backend-independent wire representation shared by engine
runtimes and provider processes. It contains only copied-value frame, service request/reply, and
namespace-install codecs. Live channels, provider authority, dispatch, descriptor state, threads,
and Unix I/O remain in the runtime crate.
