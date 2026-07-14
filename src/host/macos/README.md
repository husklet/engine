# macOS host backend

Mach VM, `MAP_JIT`, clocks, process/thread, and BSD filesystem/network operations belong to this backend.
No macOS header may be included by portable core, translator, or Linux ABI code.
