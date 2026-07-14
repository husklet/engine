# Windows host backend

This directory reserves the Win32/NT implementation boundary for `hl_host_services`; no Windows backend is
currently built. Guest fork, epoll, and signal semantics remain owned by the Linux ABI rather than a host passthrough.
