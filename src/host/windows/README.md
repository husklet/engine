# Windows host backend

Future Win32/NT implementation of `hl_host_services`. Guest fork, epoll and signals stay in the Linux ABI
model and are implemented using spawn/state restoration, IOCP/waits and engine-controlled interruption.
