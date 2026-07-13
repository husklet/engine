# macOS host backend

Move Mach VM, `MAP_JIT`, kqueue, process/thread, BSD filesystem/network and IOSurface ownership here one
service group at a time. No macOS header may be included outside this directory, the runner packaging target,
or the not-yet-migrated production implementation.
