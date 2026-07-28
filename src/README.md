# Runtime

The runtime has one dependency direction:

| Layer | Ownership |
| --- | --- |
| `include/hl`, `core` | Public contracts, validation, lifecycle, and backend selection |
| `core/target` | Guest-ISA registration and execution entry points |
| `linux_abi` | Linux syscall, container, descriptor, and checkpoint semantics |
| `translator` | Guest instruction lowering and host code generation |
| `host` | Operating-system implementations of memory, process, file, and synchronization services |

Core code depends on host services through `hl_host_services`; Linux semantics
must not leak into a host backend. A new host implements that service boundary
and native activation, while a new guest ISA implements the target and
translator boundaries.

The typed library path creates host services, validates an
`hl_engine_config`, selects a guest backend, starts the process, waits through
the host process service, and publishes one `hl_engine_exit`. The activation
path decodes its descriptor request and then enters that same lifecycle.
