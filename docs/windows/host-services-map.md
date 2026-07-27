# `hl_host_services` → Win32/NT

The complete implementation map for a native Windows host backend, one row per callback across all 15 groups.

`DOCS.md` is normative. This file is a plan, not a record: nothing here has been compiled or measured. Every claim
about Windows behaviour is either cited to Microsoft documentation, cited to a named third-party implementation, or
explicitly marked as unverified. Where the honest answer is "this needs an experiment", it says so.

Line and file references were checked against the tree at `feat/windows-amd64`.

---

## 1. What this document is not

The task is "map `hl_host_services` to Win32". Doing that map honestly turns up three problems that are **outside**
`src/host/`, and they dominate the schedule. Stating them first is the only way the per-callback tables below are
readable as a plan rather than a wish.

### 1.1 `fork()` is the load-bearing primitive and Windows does not have it

`process.spawn_cloned` and `process.spawn_prepared` are `fork(2)` (`src/host/linux/host.c:3490`). Not
`posix_spawn`, not `CreateProcess` — a copy of the *current* address space with the guest already loaded, its JIT
code cache live, and its mappings at identical addresses. `sync.fork_prepare`/`fork_parent`/`fork_child` and
`memory.repair_code_after_fork` exist only to service it. `file.clone_for_fork` exists only to service it.
`private.c`'s entire shared-row design (`src/host/private.c`, 439 lines) exists to make the private-descriptor table
survive it.

Windows has `RtlCloneUserProcess`, which is what the retired SUA subsystem used. It is undocumented, leaves the new
process unknown to `csrss` (so most Win32 APIs in the child misbehave), and is not a supported path for a Win32
subsystem image. Cygwin's `fork` is the other approach — spawn a fresh process and copy the parent's address space
into it at identical addresses — and it is a multi-thousand-line subsystem with well-known failure modes when
ASLR or a differently-based DLL moves anything.

**Consequence:** `HL_HOST_CAP_PROCESS` should not be advertised in phase 1. That is legal —
`src/core/lifecycle.c:109` is the only caller that requires `PROCESS | MEMORY` and it converts failure to
`HL_STATUS_NOT_SUPPORTED`. Guest `fork(2)`/`vfork(2)` will not work. Guest *threads* are unaffected: they go through
`pthread_create` in `src/linux_abi/thread.c:2664,2745`, not the process group.

### 1.2 The Linux ABI layer calls POSIX directly, on a descriptor it gets from `posix_attachment`

This is the finding that should change the shape of the Windows plan.

`root_handle_bind` (`src/linux_abi/container/vfs.c:1455-1498`) borrows a native descriptor for the namespace root
through `posix_attachment.borrow_file_at_least` and stores it in `g_root_fd`. Every path resolution in the guest
personality then `openat()`s from that descriptor. There are **61 direct call sites** of the POSIX `*at` family
inside `src/linux_abi` and `src/core`, 62 of them in `src/linux_abi/syscall/fs.c` alone:

| File | `*at` call sites |
|---|---|
| `src/linux_abi/syscall/fs.c` | 62 |
| `src/linux_abi/syscall/nonpie_args.h` | 12 |
| `src/linux_abi/container/vfs/resolve.c` | 10 |
| `src/linux_abi/container/vfs.c` | 8 |
| `src/linux_abi/container/state.c` | 6 |
| `src/linux_abi/sentry.c` | 4 |
| (nine other files) | 1 each |

`root_handle_bind` failure is fatal at init — `src/core/target/x86_64.c:801` and `:812`,
`src/core/target/aarch64.c:822` and `:834` all do `return -1` — **for bare guests as well as containers**. There is
no fallback branch.

So: omitting `HL_HOST_CAP_POSIX_ATTACHMENT` is legal at the ABI layer (see §3.4), and it is the right call, but the
work it exposes is a Linux-ABI-layer port, not a host-backend one. A perfect Windows `hl_host_services` still will
not boot a guest until `g_root_fd` and the `openat`-from-`dirfd` resolver lane are routed through
`file.resolve_beneath` / `file.open_beneath` instead. That is a separate work item and it is larger than the entire
host backend.

### 1.3 Signals

`memory.repair_signal_page` is specified as async-signal-safe-equivalent; the guest fault path relies on
`sigsetjmp`/`siglongjmp` out of a POSIX handler (`docs/amd64-host.md` §4.1, §6.1). Windows has no POSIX signals.
The structural equivalent is a **vectored exception handler** (`AddVectoredExceptionHandler`) running on the
faulting thread, with `RtlRestoreContext` or a plain `longjmp` to leave it.

The safety analysis is *different*, and in one respect easier. A POSIX signal handler can interrupt the same thread
mid-`malloc`, so calling anything that takes the allocator lock deadlocks. A Win32 user-mode access violation cannot
be raised while the same thread is inside a kernel `NtAllocateVirtualMemory` call, so the re-entrancy hazard that
motivates the contract's "no userspace allocation, locks, logging, ownership registries" wording does not arise the
same way. `VirtualProtect`/`VirtualAlloc` are thin `kernel32`→`ntdll`→`syscall` stubs that touch no CRT state and
take no loader lock. **Unverified, and the single most important thing to prove with an experiment before
committing to the design.**

---

## 2. Cross-cutting policy decisions

These have to be settled before any table row can be implemented. Each is a decision, not a discovery.

### 2.1 Paths: UTF-8 in, UTF-16 out — use WTF-8, not strict UTF-8

The contract passes paths as `(const char *path, size_t path_size)` byte spans with no trailing NUL
(`host_services.h:346` and 12 other entry points). Windows filesystem APIs are UTF-16.

Neither side is well-formed Unicode:

- A Linux filename is an arbitrary byte string that is not `/` and not NUL. It need not be valid UTF-8.
- A Windows filename is an arbitrary `WCHAR` string, which may contain unpaired surrogates.

A strict `MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, ...)` loses both directions. **Policy: WTF-8.**
Encode unpaired UTF-16 surrogates as three-byte WTF-8 sequences on the way out, decode them back on the way in,
and pass through valid UTF-8 unchanged. Bytes that are neither valid UTF-8 nor WTF-8 surrogate escapes get an
overlong-free fallback mapping into the `U+DC80..U+DCFF` low-surrogate range (the "surrogateescape" convention).
This is lossless in both directions and is what every serious cross-platform runtime converges on.

The conversion helper is ~120 lines and is used by roughly 14 file callbacks. It belongs in
`src/host/windows/path.c`, not inlined per call site.

### 2.2 Long paths

Three separate mechanisms, and it matters which one is used where:

| Mechanism | Applies to | Effect |
|---|---|---|
| `NtCreateFile` with `OBJECT_ATTRIBUTES.RootDirectory` | every relative open | **No `MAX_PATH` limit at all.** The relative name is a leaf or short component list; the prefix never appears. |
| `\??\` NT prefix | the few absolute-path entry points | No limit. Disables all normalization — the backend must normalize itself. |
| `longPathAware` manifest + `\\?\` Win32 prefix | any `CreateFileW` fallback | Removes `MAX_PATH` for Win32 APIs on Win10 1607+ with the registry opt-in. |

**Policy: use `NtCreateFile` with `RootDirectory` for everything reachable from a directory handle, which is
everything except the initial root open.** This is not a workaround — it is required anyway, because `CreateFileW`
has no relative-open form and the entire file group is `*at`-shaped. `MAX_PATH` then simply never arises. Ship the
`longPathAware` manifest as belt-and-braces for the `GetFinalPathNameByHandleW` and `CreateSymbolicLinkW` paths
that have no NT-level alternative.

Note that `\\?\` (and `\??\`) suppress `.`/`..` collapsing and `/`→`\` translation. Since `resolve.c`'s algorithm
already walks components itself and never hands a multi-component path to the OS, that suppression is the desired
behaviour, not a hazard.

### 2.3 The handle table

The `(generation << 32) | (index + 1)` encoding in `hl_linux_encode_handle` (`src/host/linux/host.c:240`) is
platform-neutral and should be copied verbatim. The entry payload changes from `int descriptor` to a union of
`HANDLE` / `SOCKET`, plus the per-kind fields already present.

What does **not** carry over is `src/host/private.c`'s descriptor-band scheme. `HL_HOST_PRIVATE_DESCRIPTOR_MINIMUM`
(4096) and `HL_HOST_GUEST_DESCRIPTOR_MINIMUM` (20480) in `src/host/system.h` rest on four POSIX premises that are
all false on Windows: descriptors are small dense integers; `RLIMIT_NOFILE` bounds that space;
`fcntl(F_DUPFD_CLOEXEC, floor)` can relocate a descriptor above a chosen number; and `fork()` inherits both the
descriptors and a `MAP_SHARED` page holding the table. Windows `HANDLE`s are opaque, sparse, and not relocatable.
The guest-visible fd namespace has to be entirely synthetic, allocated by the Linux front. In practice it largely
already is; the private registry exists to keep engine-owned fds out of a namespace the guest can otherwise *see*
through raw syscalls, and on Windows the guest can never see a `HANDLE` at all. `src/host/windows/private.c` should
be a ~40-line no-op shim in the shape `src/host/fake/host.c:1351-1379` already uses.

### 2.4 Error mapping

Two mapping tables are needed, mirroring `hl_linux_status_from_errno` (`src/host/linux/host.c:199`):

- `hl_windows_status_from_error(DWORD)` for Win32 `GetLastError()` values.
- `hl_windows_status_from_ntstatus(NTSTATUS)` for the `Nt*` calls. Either map directly or funnel through
  `RtlNtStatusToDosError` into the first table; direct is more precise (`STATUS_DELETE_PENDING` and
  `STATUS_SHARING_VIOLATION` both collapse to `ERROR_ACCESS_DENIED`, which is a real loss).

`hl_host_result.detail_domain` is declared at `include/hl/host_services.h:156` and **read by nothing in the entire
tree** — no consumer in `src/`, `include/`, or `tests/`. Linux uses `1` for errno (`hl_linux_result`,
`host.c:195`); `src/host/sync.c:28` uses `0`. A Windows backend can freely take `2` for Win32 and `3` for NTSTATUS.

~180 lines for both tables.

### 2.5 Granularity

| Quantity | Linux (x86-64) | Windows (x86-64) | Consequence |
|---|---|---|---|
| Page size | 4 KiB | 4 KiB | Same. Guest `PAGE_SIZE` is unaffected. |
| Allocation/reservation granularity | 4 KiB | **64 KiB** (`SYSTEM_INFO.dwAllocationGranularity`) | Reservation base addresses and view base addresses are 64 KiB-aligned. |
| Mapping file offset | 4 KiB | **64 KiB** normally; **4 KiB with `MEM_REPLACE_PLACEHOLDER`** | See §4, `map_file`. |

The 64 KiB reservation granularity is why `memory.reserve`'s `alignment > page` rejection
(`src/host/linux/host.c:340`) is harmless — Windows over-delivers. It is *not* harmless for `map_file`, where the
guest supplies a 4 KiB-aligned offset.

---

## 3. Group-by-group verdict summary

| Group | Cap bit | Callbacks | Difficulty | Phase 1 |
|---|---|---|---|---|
| memory | `MEMORY` (mandatory) | 14 | **hard** | **yes** — required by `engine.c:470` |
| memory / code | `CODE_MAPPING` | (4 of the 14) | moderate | **yes** — required by `translator/cache.c:97` |
| clock | `CLOCK` (mandatory) | 9 | trivial–moderate | **yes** — required by `engine.c:470` |
| log | `LOG` | 1 | trivial | yes |
| sync | `SYNC` (mandatory) | 7 | trivial | **yes** — required by `engine.c:470` |
| file | `FILE` | 40 | **hard** | yes (with named omissions) |
| stream | `STREAM` | 8 | moderate–hard | yes |
| event | `EVENT` | 5 | **hard** | yes |
| event / timer | `EVENT_TIMER` | 2 | moderate | yes |
| counter | `COUNTER` | 10 | moderate | yes |
| transfer | `TRANSFER` | 5 | moderate | yes |
| network | `NETWORK` | 6 | moderate | yes (IPv4/IPv6 only) |
| shared_memory | `SHARED_MEMORY` | 4 | moderate | yes (`resize` grow-only) |
| directory | `DIRECTORY` | 7 | moderate–hard | **no** |
| watch | `WATCH` | 4 | moderate | **no** |
| process | `PROCESS` | 5 | **blocked** | **no** |
| posix_attachment | `POSIX_ATTACHMENT` | 3 | **blocked** | **no** |

Recommended phase 1 capability mask:

```c
HL_HOST_CAP_MEMORY | HL_HOST_CAP_CLOCK | HL_HOST_CAP_LOG | HL_HOST_CAP_SYNC |
HL_HOST_CAP_CODE_MAPPING | HL_HOST_CAP_FILE | HL_HOST_CAP_STREAM |
HL_HOST_CAP_EVENT | HL_HOST_CAP_EVENT_TIMER | HL_HOST_CAP_COUNTER |
HL_HOST_CAP_TRANSFER | HL_HOST_CAP_NETWORK | HL_HOST_CAP_SHARED_MEMORY
```

Omitting a bit is safe by construction: `hl_host_services_validate` (`src/core/host_services.c:18-147`) checks a
group's pointers **only when the corresponding bit is set**, and `src/host/fake/host.c:1326` already ships a
backend that omits five bits including `POSIX_ATTACHMENT` and `CODE_MAPPING`. What omission costs is documented
per group below.

---

## 4. memory (`HL_HOST_MEMORY_ABI 6`)

The hardest group, and the one where the modern Win32 API genuinely changes the answer. Everything below assumes
**placeholder reservations** — `VirtualAlloc2` / `MapViewOfFile3` / `UnmapViewOfFile2`, Win10 1803+, `Kernel32.dll`,
`onecore.lib`.

Without placeholders, `unmap_range` and `map_file`-at-a-fixed-address are not implementable at all: `VirtualFree`
with `MEM_RELEASE` must free an *entire* original reservation and `UnmapViewOfFile` must unmap an *entire* view,
whereas `munmap` freely carves a hole out of the middle. With placeholders, a reservation is explicitly splittable
(`VirtualFree(base, size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)`) and a view can be retired back into a
placeholder (`UnmapViewOfFile2(..., MEM_PRESERVE_PLACEHOLDER)`), which is exactly the `mmap`/`munmap` address-space
model.

**Linkage caveat, unresolved.** mingw-w64 declares `VirtualAlloc2`/`MapViewOfFile3` in `memoryapi.h` behind
`_WIN32_WINNT >= 0x0A00` / `NTDDI_VERSION >= 0x0A000005`, but
[mingw-w64 issue #27](https://github.com/mingw-w64/mingw-w64/issues/27) reports `undefined reference to
__imp_VirtualAlloc2` at link time, with only the `…FromApp` variants resolvable, and an
[October 2023 patch](https://sourceforge.net/p/mingw-w64/mailman/mingw-w64-public/thread/20231025193048.28648-2-mark@harmstone.com/)
moved the `FromApp` spellings into an `api-ms-win-core-memory-l1-1-6_windowsapp` import library to match the
WinSDK layout. **Action: resolve this in the first hour of implementation by test-linking against the pinned
toolchain.** If the import symbols are absent, the fallback is a one-time `GetProcAddress` on `kernel32.dll` for
five entry points (`VirtualAlloc2`, `MapViewOfFile3`, `UnmapViewOfFile2`, `VirtualFree` is always present) into a
function-pointer table — ~40 lines, no behavioural difference. Do **not** use the `FromApp` variants as a
substitute without checking: they apply UWP-appcontainer restrictions and reject `PAGE_EXECUTE*` protections,
which would break `reserve_code`.

| Callback | Linux approach | Win32/NT | Semantic gaps | Diff. | Ph.1 |
|---|---|---|---|---|---|
| `reserve(size, alignment, flags)` | `mmap(NULL, PRIVATE\|ANON, prot)`; rejects `alignment > page` (`host.c:340`) | `VirtualAlloc2(NULL, NULL, size, MEM_RESERVE\|MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS)` then commit, or plain `VirtualAlloc(MEM_RESERVE\|MEM_COMMIT)` | Base is 64 KiB-aligned rather than 4 KiB — strictly stronger than the contract asks. Commit charge is taken eagerly against the pagefile, unlike Linux overcommit; a large reservation that Linux would happily hand out can fail here. | trivial | yes |
| `protect(mapping, off, size, flags)` | `mprotect` | `VirtualProtect` | (a) `VirtualProtect` cannot span two distinct reserved regions; placeholder splitting makes each subrange its own region, so the backend must iterate. (b) Windows has no write-without-read: `HL_HOST_MEMORY_WRITE` alone must widen to `PAGE_READWRITE`. On x86-64 this is unobservable (the hardware has no W-only encoding either) but it is a widening and must be recorded. (c) `PAGE_EXECUTE_*` on a view requires the section's *maximum* protection to have allowed it at `CreateFileMapping` time. | moderate | yes |
| `release(mapping)` | `munmap` (+ second alias, + `close(fd)`) | `UnmapViewOfFile` per view, then `VirtualFree(base, 0, MEM_RELEASE)` for the placeholder, then `CloseHandle(section)` | The ordering is fixed and the reverse of Linux's: views must go before the placeholder. | moderate | yes |
| `unmap_range(mapping, off, size)` | `munmap` of a subrange; carves holes | `UnmapViewOfFile2(base+off, MEM_PRESERVE_PLACEHOLDER)` for a mapped subrange, or `VirtualFree(base+off, size, MEM_RELEASE\|MEM_PRESERVE_PLACEHOLDER)` for a committed one | **The reason placeholders are mandatory.** Also: Linux rounds a non-page-aligned *length* up internally and the backend deliberately permits that (`host.c:591-596`, a real bug fix); Windows requires the exact placeholder bounds, so the backend must round up itself and must first *split* the placeholder at both edges before it can retire the middle. Splitting a placeholder that is currently a mapped view is not permitted — unmap first, split second. | **hard** | yes |
| `map_file(file, addr, off, size, prot, flags)` | `mmap(fd, off, MAP_SHARED\|MAP_PRIVATE, MAP_FIXED\|MAP_FIXED_NOREPLACE)` | `CreateFileMappingW(hFile, maxProt, size)` once per file, then `MapViewOfFile3(section, NULL, placeholder, off, size, MEM_REPLACE_PLACEHOLDER, prot, NULL, 0)` | Five real gaps, listed below the table. | **hard** | yes |
| `map_anonymous(addr, size, prot, flags)` | `mmap(ANON, PRIVATE\|SHARED, FIXED…)`; relies on kernel zero-fill | private → `VirtualAlloc2(…, MEM_RESERVE\|MEM_COMMIT, prot)`; shared → `CreateFileMappingW(INVALID_HANDLE_VALUE, …)` + `MapViewOfFile3` | Zero-fill is guaranteed for both `MEM_COMMIT` and pagefile-backed sections, so the header's explicit reliance on zero pages (`host_services.h:207-211`) holds. `MAP_SHARED\|ANON` exists on Windows only to be shared between *threads*, since there is no fork — the fork-shared use case simply does not arise. | moderate | yes |
| `sync(mapping, off, size)` | `msync(MS_SYNC)` | `FlushViewOfFile(addr, size)` **then** `FlushFileBuffers(hFile)` | `FlushViewOfFile` alone only queues the writes to the filesystem; `MS_SYNC` promises durability. Both calls are required. For a pagefile-backed section there is no file handle and the second call is skipped. | trivial | yes |
| `publish_code(mapping, off, size)` | `__builtin___clear_cache` | `FlushInstructionCache(GetCurrentProcess(), addr, size)` | None. On x86-64 both are effectively no-ops (coherent I-cache), but the call is required for correctness on any future ARM64 Windows host and is documented as mandatory for `VirtualAlloc2`-created executable regions. | trivial | yes |
| `reserve_code(size, align, DUAL_ALIAS, out)` | `memfd_create` + `ftruncate` + two `MAP_SHARED` views (RW and RX) at `hl_linux_map_aligned` alignment | `CreateFileMappingW(INVALID_HANDLE_VALUE, PAGE_EXECUTE_READWRITE, size)`; two placeholders from `VirtualAlloc2` with `MEM_EXTENDED_PARAMETER` `MemExtendedParameterAddressRequirements{.Alignment = align}`; `MapViewOfFile3` each with `MEM_REPLACE_PLACEHOLDER`, `PAGE_READWRITE` and `PAGE_EXECUTE_READ` | **Cleaner than Linux.** `MEM_ADDRESS_REQUIREMENTS.Alignment` gives arbitrary power-of-2 alignment natively, retiring the over-reserve-and-trim dance in `hl_linux_map_aligned` (`host.c:633-653`). The section's max protection *must* be `PAGE_EXECUTE_READWRITE` or the RX view fails. **Blocker if present:** Arbitrary Code Guard (`ProcessDynamicCodePolicy`) forbids `PAGE_EXECUTE*` entirely — the process must not enable it. If CFG is enabled for the image, indirect calls into JIT code need `SetProcessValidCallTargets`; mingw-w64 does not enable CFG by default. | moderate | yes |
| `repair_code_after_fork(mapping, preserve)` | rebuilds the memfd and re-`MAP_FIXED`s both aliases in the fork child (`host.c:717`) | — | No fork, so no fork child, so the entry point is unreachable. `hl_host_services_validate` requires it non-NULL when `CODE_MAPPING` is advertised (`host_services.c:38-41`), so provide a stub returning `HL_STATUS_NOT_SUPPORTED`. | trivial | yes (stub) |
| `begin_code_write` / `end_code_write` | no-ops (dual-alias host) | no-ops | None. Same reasoning: the RW alias is always writable. | trivial | yes |
| `discard(mapping)` | pure handle-table bookkeeping | identical | None. | trivial | yes |
| `repair_signal_page(addr, size, prot)` | `mprotect`, else `mmap(MAP_FIXED_NOREPLACE)`, else `mprotect` again; async-signal-safe by construction | `VirtualProtect(page, 4096, prot)`, else `VirtualAlloc(page, 4096, MEM_RESERVE\|MEM_COMMIT, prot)`, else `VirtualProtect` again | `VirtualAlloc` with `MEM_RESERVE` on an already-reserved range fails — which *is* `MAP_FIXED_NOREPLACE` semantics, exactly. The contract's "no errno-dependent decisions" translates to "branch on the return value only, never on `GetLastError()`". Safety argument in §1.3 and **unverified**. | moderate | yes |

**`map_file`, the five gaps:**

1. **Offset alignment.** `MapViewOfFile` normally demands a 64 KiB-aligned offset; the guest supplies 4 KiB.
   Microsoft documents that with `MEM_REPLACE_PLACEHOLDER`, *"the 64k alignment requirements on Offset and
   BaseAddress do not apply"* and the offset need only be page-aligned
   ([MapViewOfFile3](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-mapviewoffile3)).
   This is the single most important reason to use placeholders. **Verify empirically in the first week** —
   it is load-bearing and the doc wording is recent.
2. **Maximum protection is fixed at section creation.** A later `protect()` to RX over a section created
   `PAGE_READWRITE` will fail. The backend must decide the section's max protection from the file handle's granted
   access at `CreateFileMappingW` time, and cache one section per (file, max-protection) pair.
3. **`MAP_FIXED` is not atomic.** `mmap(MAP_FIXED)` atomically replaces whatever is at the address; the Linux
   backend relies on exactly that and then retires the stale ownership handles it displaced
   (`host.c:479-494`, "MAP_FIXED replaced these VMAs atomically"). Windows cannot: the sequence is
   `UnmapViewOfFile2(MEM_PRESERVE_PLACEHOLDER)` then `MapViewOfFile3(MEM_REPLACE_PLACEHOLDER)`, with a window in
   which the address range is a bare placeholder. Any concurrent guest thread touching that range takes an access
   violation. **This is a genuine, unrecoverable semantic loss** and it should be written into the Windows
   backend's own comments rather than discovered later. Mitigation is to hold the address-space lock across the
   pair and treat a fault inside the window as retryable — ugly, and it does not help a *guest* thread.
4. **Mapping past EOF.** `mmap` permits a mapping whose last page extends past EOF (reads give zeros, beyond that
   SIGBUS). `CreateFileMapping` with a size larger than the file **extends the file**. The backend must clamp the
   section size to the current file size and let the tail come from a separate anonymous placeholder, or accept
   the file-extension side effect. Clamping is correct; it is also fiddly.
5. **`MAP_PRIVATE`.** Maps to `PAGE_WRITECOPY` / `SEC_COMMIT`. Windows copy-on-write over a file section behaves
   as expected; no gap found, but untested.

**Omitting `CODE_MAPPING`** costs the JIT entirely: `src/translator/cache.c:97` and `src/core/target/native.c:39`
both require `MEMORY | CLOCK | CODE_MAPPING`. Since the x86-64 interpreter path exists
(`docs/amd64-host.md` §4), a Windows host could in principle run interpreted-only without it. Not recommended —
`reserve_code` is one of the *easier* rows here.

---

## 5. clock (`HL_HOST_CLOCK_ABI 4`)

The cheapest group after `log`. Nine callbacks, all short.

| Callback | Linux | Win32 | Gaps | Diff. | Ph.1 |
|---|---|---|---|---|---|
| `monotonic_ns` | `clock_gettime(CLOCK_MONOTONIC)` | `QueryUnbiasedInterruptTimePrecise` (100 ns units) | `CLOCK_MONOTONIC` excludes suspended time; `QueryUnbiasedInterruptTime*` is the only Windows clock that also excludes it. Using `QueryPerformanceCounter` here would be wrong across sleep on some HALs. | trivial | yes |
| `realtime_ns` | `clock_gettime(CLOCK_REALTIME)` | `GetSystemTimePreciseAsFileTime`, minus `116444736000000000` (100 ns ticks 1601→1970), ×100 | 100 ns resolution, not 1 ns. Observable by a guest comparing consecutive `clock_gettime` results. | trivial | yes |
| `raw_monotonic_ns` | `clock_gettime(CLOCK_MONOTONIC_RAW)` | `QueryPerformanceCounter` scaled by `QueryPerformanceFrequency` (cached once) | QPC is NTP-disciplined-free, which is the point of `_RAW`. Frequency is typically 10 MHz. | trivial | yes |
| `process_cpu_ns` | `clock_gettime(CLOCK_PROCESS_CPUTIME_ID)` | `GetProcessTimes`, user + kernel, ×100 | — | trivial | yes |
| `thread_cpu_ns` | `clock_gettime(CLOCK_THREAD_CPUTIME_ID)` | `GetThreadTimes`, user + kernel, ×100 | **Real accuracy loss.** `GetThreadTimes` is accumulated at scheduler-tick granularity (~15.6 ms default), against Linux's nanosecond accounting. `QueryThreadCycleTime` is precise but returns cycles, and converting cycles→ns needs a TSC frequency the host does not have (§`architectural_counter_hz`). Report the coarse value and document it. | trivial | yes |
| `sleep_until(kind, deadline_ns)` | `clock_nanosleep(TIMER_ABSTIME)` | `CreateWaitableTimerExW(..., CREATE_WAITABLE_TIMER_HIGH_RESOLUTION)` + `SetWaitableTimer` + `WaitForSingleObject` | `REALTIME` maps directly — `SetWaitableTimer`'s absolute `FILETIME` *is* system time and correctly tracks clock steps, matching `clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME)`. `MONOTONIC` has no absolute form: compute the remaining interval and pass a negative (relative) due time, which loses the "immune to clock changes" property only across a step during the sleep. `PROCESS_CPU` → `HL_STATUS_NOT_SUPPORTED` (Linux supports it; Windows has no CPU-time timer). No `EINTR` on Windows, so `HL_STATUS_INTERRUPTED` never occurs — a non-alertable wait cannot be interrupted. | moderate | yes |
| `architectural_counter_hz` | `mrs cntfrq_el0` on AArch64, else `NOT_SUPPORTED` | `HL_STATUS_NOT_SUPPORTED` | None — identical to the Linux x86-64 answer and for the identical reason (TSC frequency is not architecturally readable). `s1_calibrate` then clears its fast-clock flag. | trivial | yes |
| `backoff_ns(interval)` | `nanosleep`, retried over `EINTR` | interval < 1 ms: spin on `QueryPerformanceCounter` with `_mm_pause`. Otherwise `Sleep(ms)` in a loop until the QPC deadline passes. | The contract forbids allocation, locking, and early return, and demands immutable state. A high-resolution waitable timer needs a `HANDLE`, which means either per-thread TLS (allocation) or a process-wide pre-created timer (not thread-safe for concurrent waiters). `Sleep()` has ~15.6 ms granularity unless `timeBeginPeriod(1)` is in effect process-wide — which has global power-consumption consequences and should be an explicit engine decision, not a side effect. The spin-below-1 ms hybrid avoids all of it. | moderate | yes |

---

## 6. log (`HL_HOST_LOG_ABI 1`)

| Callback | Linux | Win32 | Gaps | Diff. | Ph.1 |
|---|---|---|---|---|---|
| `emit(event, msg, size)` | `write(STDERR_FILENO)` loop, retried over `EINTR` | `WriteFile(GetStdHandle(STD_ERROR_HANDLE), …)` loop | None. No `EINTR`. If stderr is a console, the bytes are interpreted in the console code page unless `SetConsoleOutputCP(CP_UTF8)` is called once at init — do that, or non-ASCII diagnostics are mojibake. | trivial | yes |

~25 lines.

---

## 7. sync (`HL_HOST_SYNC_ABI 2`)

Mandatory (`src/core/engine.c:470` requires `MEMORY | CLOCK | SYNC`). Almost free.

`src/host/sync.c` is 207 lines of which ~170 are portable registry bookkeeping — chunked storage, generation
encoding, refcounting, free-slot scan. Only the ~30-line primitive layer is POSIX.

| Callback | Linux | Win32 | Gaps | Diff. | Ph.1 |
|---|---|---|---|---|---|
| `mutex_create` | `pthread_mutex_init` with `PTHREAD_MUTEX_ERRORCHECK` | `InitializeSRWLock` + an owner-thread-id field | `SRWLOCK` is not error-checking and `CRITICAL_SECTION` is *recursive*, which the contract explicitly forbids ("Opaque, non-recursive host mutexes", `host_services.h:587`). Store the owner TID beside the lock and synthesize the `EDEADLK`/`EPERM` verdicts that `hl_sync_status` maps. | trivial | yes |
| `mutex_lock` / `mutex_unlock` | `pthread_mutex_lock/unlock` | `AcquireSRWLockExclusive` / `ReleaseSRWLockExclusive` + owner check | Unlock-by-non-owner must return `HL_STATUS_INVALID_ARGUMENT` as the errorcheck mutex does; `SRWLOCK` would corrupt silently. | trivial | yes |
| `mutex_close` | `trylock` + `destroy`, refuses when `users != 0` | `TryAcquireSRWLockExclusive`; no destroy needed | `SRWLOCK` has no destructor, which is simpler. | trivial | yes |
| `fork_prepare` / `fork_parent` / `fork_child` | hold/release the registry lock across `fork` | return `HL_STATUS_OK` unconditionally | No fork. Validation requires all three non-NULL (`host_services.c:110-116`). Returning OK is honest: there is nothing to prepare and nothing to repair. | trivial | yes |

**Shortcut worth considering:** mingw-w64 bundles winpthreads, so `src/host/sync.c` very likely compiles verbatim.
That is the fastest path to a booting backend, and the SRWLOCK rewrite can follow. It should not be the shipping
answer — winpthreads' errorcheck mutex is a userspace emulation and adds a dependency the rest of the backend does
not need.

---

## 8. file (`HL_HOST_FILE_ABI 23`)

Forty callbacks. The largest group and, after `event`, the most work.

**Architecture.** Every file handle is a Win32 `HANDLE` opened through `NtCreateFile`, because
`OBJECT_ATTRIBUTES.RootDirectory` is the only relative-open mechanism Windows has and the entire group is `*at`-shaped.
Once the handle exists, prefer documented Win32 (`GetFileInformationByHandleEx`, `SetFileInformationByHandle`,
`DeviceIoControl`) over further `Nt*` calls; drop to `Nt*` only where Win32 has no equivalent
(positional I/O, directory enumeration, relative rename/link).

**The Unix permission-bit problem, stated once.** `open_relative`, `make_directory`, `make_fifo`,
`store_private_atomic` all take a `mode_t`-shaped `permissions`, `metadata` returns one, and `set_permissions`
sets one. Windows has ACLs, and the only bit with a native analogue is "not writable"
(`FILE_ATTRIBUTE_READONLY`). DOCS.md §2 item 7 forbids silently ignoring a requested option, and §3.5 assigns
guest ownership virtualization to the Linux front — which is the right place for this too. **Decision needed
before implementation:** either (a) the Windows backend maps only the write bit and returns
`HL_STATUS_NOT_SUPPORTED` for any other requested change, or (b) the mode is virtualized entirely above the host
seam and the backend takes `permissions` as advisory. (b) is correct and (a) is what an un-discussed
implementation will drift into. This is the single largest fidelity question in the group.

### 8.1 Open, close, identity

| Callback | Linux | Win32/NT | Gaps | Diff. | Ph.1 |
|---|---|---|---|---|---|
| `open_relative` | `openat(dirfd, path, flags, mode)` | `NtCreateFile` with `RootDirectory = dirHANDLE` | `READ`/`WRITE` → `FILE_GENERIC_READ`/`_WRITE`. `APPEND` → `FILE_APPEND_DATA` — **natively atomic on Windows**, retiring the `/proc/self/fd` re-open hack at `host.c:958-985` entirely. `DIRECTORY` → `FILE_DIRECTORY_FILE`. `NOFOLLOW` → `FILE_OPEN_REPARSE_POINT`. `PATH_ONLY` (`O_PATH`) → open with `FILE_READ_ATTRIBUTES\|SYNCHRONIZE` only, which is the closest analogue and, like macOS's model, is a *real* handle rather than Linux's restricted one — that removes the class of `EBADF` bugs recorded at `host.c:1464-1471`. `NONBLOCK` on a file has no analogue and must be ignored or rejected. `CREATE`/`EXCLUSIVE`/`TRUNCATE` → `FILE_CREATE` / `FILE_OPEN_IF` / `FILE_OVERWRITE_IF`. Sharing mode must be `FILE_SHARE_READ\|WRITE\|DELETE` throughout or ordinary POSIX patterns deadlock. | moderate | yes |
| `close` | `close` | `CloseHandle` | None. | trivial | yes |
| `metadata` | `fstat` + `statx(STATX_BTIME)` | `GetFileInformationByHandleEx`: `FileBasicInfo`, `FileStandardInfo`, `FileIdInfo` | `stable_device` ← `FILE_ID_INFO.VolumeSerialNumber`. `stable_object` ← low 64 bits of the 128-bit `FileId`; on NTFS that equals the FRN and is stable, but **on ReFS the full 128 bits are meaningful and truncation collides** ([FILE_ID_INFO](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_id_info)). Record it. `created_ns` is *native* here and better than Linux's `statx` dance. `changed_ns` ← `ChangeTime`. `user`/`group`/`permissions` synthesized. `link_count` ← `NumberOfLinks`. Type from `FILE_ATTRIBUTE_DIRECTORY` / `FILE_ATTRIBUTE_REPARSE_POINT` + `IO_REPARSE_TAG_SYMLINK`; named pipes → `FIFO`, console → `CHARACTER`; no `BLOCK`, no `SOCKET`. | moderate | yes |
| `path` | `readlink("/proc/self/fd/N")` | `GetFinalPathNameByHandleW(FILE_NAME_NORMALIZED \| VOLUME_NAME_DOS)` | Returns a `\\?\`-prefixed Windows path. The prefix must be stripped; the path is still `C:\...`, not POSIX, and the guest can see it. Whatever `/proc/self/fd` emulation the Linux front does will show Windows paths. | moderate | yes |
| `standard_stream(n)` | `fcntl(F_GETFL)` + `F_DUPFD_CLOEXEC` | `GetStdHandle` + `DuplicateHandle` | No `F_GETFL`. Recover the access mode with `NtQueryInformationFile(FileAccessInformation)`; recover append-ness from `FILE_APPEND_DATA` in the granted access. `NONBLOCK` is not recoverable and should be reported clear. | moderate | yes |
| `clone_for_fork` | `dup` (shares the OFD offset) | `DuplicateHandle` (shares the file object, hence the pointer) | Semantics match exactly. Purpose is moot without fork, but it costs nothing and validation requires it. | trivial | yes |
| `validate_private_regular` | `fstat`: regular, `st_uid == geteuid()`, `(mode & 022) == 0` | `GetSecurityInfo(OWNER_SECURITY_INFORMATION\|DACL_SECURITY_INFORMATION)`; compare owner SID to the token user SID; walk the DACL rejecting any write grant to a non-owner, non-SYSTEM trustee | The Unix check is two comparisons; the ACL check is a DACL walk. Approximating it (owner SID only) **weakens confinement**, which DOCS.md §4 forbids. Do it properly or return `HL_STATUS_NOT_SUPPORTED` and let the caller refuse the cache. | moderate | yes |
| `validate_private_directory` | as above, for a directory | as above | Same. | moderate | yes |

### 8.2 I/O

| Callback | Linux | Win32/NT | Gaps | Diff. | Ph.1 |
|---|---|---|---|---|---|
| `read_at` / `write_at` | `pread` / `pwrite` | `NtReadFile` / `NtWriteFile` with an explicit `ByteOffset` | `ReadFile` with an `OVERLAPPED` offset on a *synchronous* handle **updates the shared file pointer**, which `pread` must not. `Nt*File` with `ByteOffset` is the only clean `pread`/`pwrite`. | moderate | yes |
| `read` / `write` | `read` / `write` | `ReadFile` / `WriteFile` | None. | trivial | yes |
| `append` | second `O_APPEND` descriptor, `write` | `WriteFile` on a `FILE_APPEND_DATA` handle with `OVERLAPPED.Offset = 0xFFFFFFFFFFFFFFFF` | **Simpler than Linux.** Windows documents that offset as "append to end of file" and it is atomic against other appenders. The whole `wake_descriptor` second-handle mechanism (`host.c:927,958-985,1582-1604`) disappears. | trivial | yes |
| `readv` / `writev` / `readv_at` / `writev_at` / `appendv` | `readv`/`writev`/`preadv`/`pwritev` | a loop of `NtReadFile`/`NtWriteFile` | `ReadFileScatter`/`WriteFileGather` require page-aligned, page-sized, unbuffered buffers and are unusable here. A loop **loses atomicity**: POSIX `writev` on a regular file is atomic with respect to the offset, and on a pipe is atomic up to `PIPE_BUF`. A guest relying on either sees interleaving. Document; do not paper over. | moderate | yes |
| `seek` | `lseek` incl. `SEEK_DATA`/`SEEK_HOLE` | `SetFilePointerEx` for SET/CUR/END | `SEEK_DATA`/`SEEK_HOLE` need `FSCTL_QUERY_ALLOCATED_RANGES` on a sparse file plus range arithmetic. Return `HL_STATUS_NOT_SUPPORTED` in phase 1. | trivial (+moderate) | yes (partial) |
| `truncate` | `ftruncate` | `SetFileInformationByHandle(FileEndOfFileInfo)` | Requires `FILE_WRITE_DATA` in the granted access. | trivial | yes |
| `sync` / `data_sync` | `fsync` / `fdatasync` | `FlushFileBuffers` for both | Windows has no metadata/data distinction; `data_sync` over-syncs. Strictly safe, measurably slower. | trivial | yes |
| `sync_range(off, size, flags)` | `sync_file_range` | `FlushFileBuffers` (ignoring the range) or `HL_STATUS_NOT_SUPPORTED` | No ranged flush. `FlushFileBuffers` satisfies every flag combination by over-delivering, but "ignores a requested limit" brushes against DOCS §2 item 7. `NOT_SUPPORTED` is the honest answer; `sync_file_range` is advisory on Linux too. | trivial | yes |
| `sync_filesystem` | `syncfs` | `HL_STATUS_NOT_SUPPORTED` | No unprivileged whole-volume flush. `FlushFileBuffers` on a `\\.\C:` volume handle requires admin. | trivial | yes |
| `allocate_range(mode, off, size)` | `fallocate` | `KEEP_SIZE` → `SetFileInformationByHandle(FileAllocationInfo)`. `PUNCH_HOLE`/`ZERO_RANGE` → `FSCTL_SET_SPARSE` then `FSCTL_SET_ZERO_DATA`. | `COLLAPSE_RANGE`, `INSERT_RANGE`, `UNSHARE_RANGE` have no analogue → `HL_STATUS_NOT_SUPPORTED`. Sparse files are NTFS-only. Plain `fallocate(0)` (allocate-and-extend) maps to `FileAllocationInfo` + `FileEndOfFileInfo`. | moderate | yes (partial) |
| `filesystem_metadata` | `fstatfs` | `GetDiskFreeSpaceExW` + `GetVolumeInformationByHandleW` | `files` / `files_free` have no analogue (NTFS has no fixed inode count) → 0, which is what a guest sees from many Linux filesystems too. `filesystem_id` ← volume serial. `name_max` ← `GetVolumeInformationByHandleW`'s `lpMaximumComponentLength` (255 on NTFS). `flags` needs a hand-built mapping from `FILE_READ_ONLY_VOLUME` etc. | moderate | yes |

### 8.3 Namespace

| Callback | Linux | Win32/NT | Gaps | Diff. | Ph.1 |
|---|---|---|---|---|---|
| `rename_relative` | `renameat` (atomic replace) | `SetFileInformationByHandle(FileRenameInfoEx)` with `FILE_RENAME_FLAG_REPLACE_IF_EXISTS \| FILE_RENAME_FLAG_POSIX_SEMANTICS` and `RootDirectory = newDirHANDLE` | **Without POSIX semantics, renaming over a file that anyone has open fails** with `ERROR_SHARING_VIOLATION` — which breaks the atomic-replace contract `store_private_atomic` is built on. `FileRenameInfoEx` needs Win10 1709 / build 17763 and **NTFS**; it fails on FAT, exFAT, and most network redirectors. Detect once and record the fallback (non-POSIX rename) as a known fidelity gap on those volumes. | moderate | yes |
| `unlink_relative` | `unlinkat` | open with `DELETE \| FILE_OPEN_REPARSE_POINT`, then `SetFileInformationByHandle(FileDispositionInfoEx, FILE_DISPOSITION_DELETE \| FILE_DISPOSITION_POSIX_SEMANTICS)` | **The most important single flag in the group.** Legacy Windows delete is *deferred* until the last handle closes, and the name stays visible and un-recreatable in the meantime (`STATUS_DELETE_PENDING`). POSIX semantics removes the name immediately, which is what unlink-then-recreate, unlink-while-open, and every `mkstemp`-shaped idiom require. Win10 1709+ and NTFS. Without it, a large fraction of guest filesystem behaviour is simply wrong. | moderate | yes |
| `remove_directory` | `unlinkat(AT_REMOVEDIR)` | as `unlink_relative` on a `FILE_DIRECTORY_FILE` handle | Non-empty → `STATUS_DIRECTORY_NOT_EMPTY` → `HL_STATUS_NOT_EMPTY`. | trivial | yes |
| `make_directory` | `mkdirat` | `NtCreateFile` `FILE_DIRECTORY_FILE \| FILE_CREATE` | Mode bits ignored (see §8 preamble). | trivial | yes |
| `make_link` | `linkat` | `NtSetInformationFile(FileLinkInformationEx)` with `RootDirectory` | NTFS only; no hardlinks to directories, ever; cross-volume → `HL_STATUS_CROSS_DEVICE`. `AT_SYMLINK_FOLLOW` maps by choosing `FILE_OPEN_REPARSE_POINT` or not on the source open. | moderate | yes |
| `make_symlink` | `symlinkat` | `CreateSymbolicLinkW(..., SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)` | Three hard gaps. (a) **Requires Developer Mode** or `SeCreateSymbolicLinkPrivilege`; the unprivileged flag is Win10 1703+ and still gated on Developer Mode. On a normal user machine this simply fails. (b) Windows symlinks are **typed** at creation (file vs directory, `SYMBOLIC_LINK_FLAG_DIRECTORY`); POSIX symlinks are typeless and may dangle, so the type is unknowable at creation time. (c) No relative-to-`dirfd` form — the backend must materialize an absolute path from the directory handle via `GetFinalPathNameByHandleW`, reintroducing a TOCTOU window the rest of the group avoids. | **hard** | yes (best effort) |
| `make_fifo` | `mkfifoat` | — | Windows named pipes live in a flat `\\.\pipe\` namespace and are not filesystem entries under an arbitrary directory. There is no analogue. `HL_STATUS_NOT_SUPPORTED`. | **blocked** | yes (stub) |
| `store_private_atomic` | temp name + `openat(O_EXCL)` + write + `fsync` + `renameat` | same shape: `NtCreateFile FILE_CREATE` + `WriteFile` + `FlushFileBuffers` + `FileRenameInfoEx` POSIX | Inherits `rename_relative`'s NTFS/1709 dependency. | moderate | yes |
| `set_owner(uid, gid)` | `fchownat(AT_EMPTY_PATH)` | — | No analogue worth building. Recommend: succeed as a no-op when `uid`/`gid` match the values `metadata` synthesizes, `HL_STATUS_NOT_SUPPORTED` otherwise. Guest `chown` to a different uid fails, which is also what an unprivileged Linux process gets. | trivial | yes |
| `set_permissions` | `fchmodat2(AT_EMPTY_PATH)` | `SetFileInformationByHandle(FileBasicInfo)` toggling `FILE_ATTRIBUTE_READONLY` | See the §8 preamble. Only the write bit is expressible. | trivial | yes |
| `set_times` | `futimens` (`UTIME_NOW`/`UTIME_OMIT`) | `SetFileInformationByHandle(FileBasicInfo)` | Clean mapping: `0` = leave unchanged (`UTIME_OMIT`), an explicit value for `EXPLICIT`, and `GetSystemTimeAsFileTime` for `UTIME_NOW`. `(LONGLONG)-1` additionally suppresses future automatic updates, which POSIX has no equivalent for and which should not be used. **Nanoseconds truncate to 100 ns.** No `ctime` setter, matching POSIX. | trivial | yes |
| `readlink` | `readlinkat(fd, "")` on an `O_PATH\|O_NOFOLLOW` handle | `DeviceIoControl(FSCTL_GET_REPARSE_POINT)` → parse `REPARSE_DATA_BUFFER` | Decode `SymbolicLinkReparseBuffer` (and, for POSIX-shaped behaviour, `MountPointReparseBuffer` for junctions). Convert UTF-16→WTF-8, translate `\`→`/`, strip the `\??\` prefix from absolute targets. Non-reparse-point → `HL_STATUS_INVALID_ARGUMENT`, matching the careful `EINVAL`-vs-`ENOENT` distinction at `host.c:1354-1365`. WSL-created symlinks use `IO_REPARSE_TAG_LX_SYMLINK` with a different payload — handle both. | moderate | yes |
| `read_directory` | `getdents64` on the shared OFD cursor | `NtQueryDirectoryFileEx(FileIdBothDirectoryInformation)` | The handle carries the enumeration cursor, which matches "shared OFD cursor" (`host_services.h:404`). One call yields name + `FileId` + attributes, so `object` and `type` are free. **`next_offset` has no analogue**: `getdents64`'s `d_off` is a resumable cookie and Windows enumeration is purely stateful, so `seekdir`/`telldir` semantics cannot be reproduced. Report a synthetic monotonic counter and document that seeking to it fails. `.` and `..` *are* returned on NTFS, matching Linux. `FILE_ID_BOTH_DIR_INFORMATION` carries a 64-bit `FileId` which is 0 on some filesystems. | **hard** | yes |
| `resolve_beneath` | `hl_host_resolve_beneath` (`src/host/resolve.c`) over `openat`/`fstatat`/`readlinkat` | a `HANDLE`-based sibling of the same algorithm | The *algorithm* — component split, `..` clamped to the pinned root, 40-link budget, absolute link targets re-rooted by stripping leading `/` — is fully portable and should be transcribed. Every leaf operation changes: `openat(O_RDONLY\|O_DIRECTORY\|O_NOFOLLOW)` → `NtCreateFile(FILE_DIRECTORY_FILE\|FILE_OPEN_REPARSE_POINT)`, `fstatat(AT_SYMLINK_NOFOLLOW)` → `NtQueryInformationFile(FileAttributeTagInformation)`, `readlinkat` → `FSCTL_GET_REPARSE_POINT`. `OBJ_DONT_REPARSE` in `OBJECT_ATTRIBUTES.Attributes` gives `HL_HOST_RESOLVE_NO_SYMLINKS` in one flag. The public struct `hl_host_resolved_path` in `src/host/resolve.h` exposes `int parent_fd/target_fd` and must gain a Windows shape. | **hard** | yes |
| `open_beneath` | `resolve_beneath` then `open_relative` with `NOFOLLOW` | identical composition | Free once `resolve_beneath` works. | trivial | yes |

---

## 9. event (`HL_HOST_EVENT_ABI 2`) — the readiness/completion problem

This is the classic hard one and it deserves being stated precisely rather than waved at.

**The mismatch.** `epoll`/`kqueue` are *readiness* interfaces: "tell me when I could read without blocking", and
the caller then performs the read itself. IOCP is a *completion* interface: "I have submitted this read; tell me
when it finished", and the kernel owns the buffer in the meantime. These are not trivially interconvertible.
Converting completion→readiness requires issuing an operation the caller did not ask for; converting
readiness→completion requires a buffer the caller has not supplied.

There is no single Windows mechanism that covers every object the contract feeds to `event.control`. The Linux
backend accepts six handle kinds there (`host.c:2650-2658`): `FILE`, `SOCKET`, `STREAM`, `COUNTER`, `DIRECTORY`,
`TRANSFER`. Each needs a different Windows answer.

**Proposed architecture — one IOCP, four feeders.**

```
pollset = HANDLE hIocp (CreateIoCompletionPort)
        + registration table {token -> {kind, object, interests, state}}

feeder 1  sockets            NtDeviceIoControlFile(\Device\Afd, IOCTL_AFD_POLL) -> completes on hIocp
feeder 2  waitable kernel    RegisterWaitForSingleObject -> callback -> PostQueuedCompletionStatus(hIocp, token)
          objects            (counters, timers, transfer queues)
feeder 3  overlapped pipes   zero-byte ReadFile with OVERLAPPED -> completes on hIocp when data arrives
feeder 4  directory changes  ReadDirectoryChangesW with OVERLAPPED -> completes on hIocp
wake                         PostQueuedCompletionStatus(hIocp, token=0)
wait                         GetQueuedCompletionStatusEx(hIocp, entries, n, ms, FALSE)
```

**Feeder 1 is undocumented and it is the right choice anyway.** `\Device\Afd` with `IOCTL_AFD_POLL` is the *actual*
readiness interface underneath Winsock's `select`/`WSAPoll`. It is what
[wepoll](https://github.com/piscisaureus/wepoll), libuv, and Rust's mio all use, and libuv has shipped it in
production for over a decade — so "undocumented" here means "unsupported by Microsoft", not "fragile". The
documented alternative is a dedicated thread running `WSAPoll` and posting to the IOCP: correct, O(n) per wake,
and capped at `FD_SETSIZE` for `select`. Phase 1 can ship the `WSAPoll` thread and swap in AFD later behind the
same internal interface; the registration table does not change.

**Feeder 3 is the trick that makes pipes work.** A zero-byte overlapped `ReadFile` completes when data becomes
available *without consuming it* — that is a readiness signal synthesized out of a completion API. It is the
standard technique and it is why `stream.pipe_pair` must create **overlapped named pipes**, not `CreatePipe`
anonymous pipes (§12). There is no symmetric trick for *write* readiness on a byte-mode pipe;
`NtQueryInformationFile(FilePipeLocalInformation)` gives `WriteQuotaAvailable`, which the backend can poll to
synthesize `HL_HOST_READY_WRITE`. Reporting always-writable is the lazy answer and it will hang a guest that
relies on `EPOLLOUT` for flow control.

**Level-triggered is the hard part, not edge-triggered.** AFD_POLL and zero-byte reads are inherently one-shot:
each request yields exactly one notification. So `HL_HOST_READY_ONESHOT` is free and `HL_HOST_READY_EDGE` is
nearly free, while *level-triggered* — the default — must be emulated: after delivering an event, immediately
re-arm, and if the object is still ready, deliver again. This is precisely wepoll's design and precisely where
its complexity lives.

| Callback | Linux | Win32/NT | Gaps | Diff. | Ph.1 |
|---|---|---|---|---|---|
| `create` | `epoll_create1` + an `eventfd` registered for wake | `CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0)` | No separate wake object needed — `PostQueuedCompletionStatus` is the wake. Simpler than Linux. | trivial | yes |
| `control(op, object, token, interests)` | `epoll_ctl` ADD/MOD/DEL | dispatch on the handle kind to one of the four feeders; maintain the registration table | The whole architecture above. MOD on an armed one-shot request means cancelling it (`CancelIoEx`) and re-issuing, which races with a completion already in flight — the table must tolerate a stale completion arriving for a cancelled registration. This is the single most bug-prone piece of the backend. | **hard** | yes |
| `wait(events, capacity, deadline_ns)` | `epoll_wait`, ms timeout rounded up | `GetQueuedCompletionStatusEx(..., dwMilliseconds, FALSE)` | Millisecond timeout granularity against a nanosecond deadline — but the Linux backend already rounds up to ms (`host.c:2701`), so this is parity, not a regression. `HL_HOST_DEADLINE_INFINITE` → `INFINITE`. The re-arm-and-recheck loop for level-triggered registrations lives here. | **hard** | yes |
| `wake` | `write(eventfd, 1)` | `PostQueuedCompletionStatus(hIocp, 0, 0, NULL)` with token 0 | None. Cleaner. | trivial | yes |
| `close` | close the timers, then the epoll fd | `CancelIoEx` every armed request, `UnregisterWaitEx(..., INVALID_HANDLE_VALUE)` every wait (synchronous quiesce), drain the IOCP, `CloseHandle` | Draining is mandatory: a completion in flight when the port closes is a use-after-free of the registration entry. | moderate | yes |
| `arm_timer(token, deadline_ns, interval_ns)` | `timerfd_create`/`timerfd_settime(TFD_TIMER_ABSTIME)` + `epoll_ctl` | `CreateWaitableTimerExW(..., CREATE_WAITABLE_TIMER_HIGH_RESOLUTION)`; `SetWaitableTimer` with a **negative relative** due time computed from the host-monotonic deadline; `RegisterWaitForSingleObject` → `PostQueuedCompletionStatus` | Deadlines are host-monotonic, and `SetWaitableTimer`'s absolute form is *system* time — so absolute cannot be used and the monotonic→relative conversion reintroduces a small window where a clock step during arming shifts the deadline. `lPeriod` is `LONG` **milliseconds**, so a sub-millisecond `interval_ns` rounds up to 1 ms; Linux's `timerfd` is an exact hrtimer. Real, guest-visible. | moderate | yes |
| `disarm_timer(token)` | `epoll_ctl(DEL)` + `close(timerfd)` | `CancelWaitableTimer` + `UnregisterWaitEx(..., INVALID_HANDLE_VALUE)` + `CloseHandle` | `UnregisterWaitEx` with `INVALID_HANDLE_VALUE` blocks until any in-flight callback finishes, which is exactly the quiesce the contract wants. | trivial | yes |

---

## 10. counter (`HL_HOST_COUNTER_ABI 2`)

Linux backs this with `eventfd`. Windows has no single object with `eventfd`'s semantics, but the composition is
straightforward and the result is *cheaper* than Linux's, which spawns a thread per subscription
(`host.c:3021-3107`).

Representation: `{ SRWLOCK lock; CONDITION_VARIABLE cv; uint64_t value; uint32_t flags; HANDLE ready; }` where
`ready` is a manual-reset `Event` held signalled exactly when `value != 0`, so `event.control` can wait on it
through feeder 2.

| Callback | Linux | Win32 | Gaps | Diff. | Ph.1 |
|---|---|---|---|---|---|
| `create(initial, flags)` | `eventfd(0, EFD_SEMAPHORE\|EFD_NONBLOCK)` + a write | allocate the struct; `CreateEventW(NULL, TRUE, initial != 0, NULL)` | None. `UINT64_MAX` reserved, as documented. | trivial | yes |
| `read` | `read(8)`, consumes all or (semaphore) one | lock; if `value == 0` → block on the CV or `HL_STATUS_WOULD_BLOCK`; consume; `ResetEvent` when it hits 0 | None. | trivial | yes |
| `write(value)` | `write(8)`, saturating add, blocks at `UINT64_MAX-1` | lock; saturating add with the same ceiling; `SetEvent`; `WakeAllConditionVariable` | None. | trivial | yes |
| `get_flags` / `set_flags` | stored in the handle entry + `fcntl(F_SETFL)` for `O_NONBLOCK` | stored in the struct | Simpler — no fd flags to keep in sync. `SEMAPHORE` cannot change after create, as on Linux. | trivial | yes |
| `duplicate` | `dup` | refcount the struct, new handle-table entry | `dup` aliases the same eventfd; a refcount aliases the same struct. Equivalent. | trivial | yes |
| `readiness(interests)` | `poll(POLLIN, 0)` | `WaitForSingleObject(ready, 0)` | None. | trivial | yes |
| `subscribe(notify, observer, token)` | `dup` + `pipe2` + a dedicated `pthread` polling both | `RegisterWaitForSingleObject(ready, cb, ctx, INFINITE, WT_EXECUTEDEFAULT)` | **Strictly better than Linux**: one thread-pool wait instead of one dedicated thread per subscription. | moderate | yes |
| `unsubscribe` | wake the thread, `pthread_join`, close | `UnregisterWaitEx(wait, INVALID_HANDLE_VALUE)` | The contract demands "Synchronously quiesces the callback before returning" — that is exactly `UnregisterWaitEx`'s `INVALID_HANDLE_VALUE` contract. A perfect fit. | trivial | yes |
| `close` | close the fd after unsubscribing all | unsubscribe all, decref, `CloseHandle(ready)` | None. | trivial | yes |

Transfer rights (`HL_HOST_TRANSFER_READ/WRITE/WAIT/CONTROL`, stored in `entry->reserved` on Linux) carry over
unchanged — they are pure bookkeeping.

---

## 11. transfer (`HL_HOST_TRANSFER_ABI 2`)

Linux uses `socketpair(AF_UNIX, SOCK_SEQPACKET)` + `SCM_RIGHTS`. Windows has **neither**: AF_UNIX support (Win10
1803+) is `SOCK_STREAM` only, there is no `socketpair`, and there is **no ancillary-data support at all**
([AF_UNIX comes to Windows](https://devblogs.microsoft.com/commandline/af_unix-comes-to-windows/)).

That sounds fatal and is not. The contract is explicit that these channels transfer *object identity*, never
native descriptor numbers (`host_services.h:537-539`), and without `fork` both endpoints are always in the same
process. So the right Windows implementation is **entirely in-process**: a refcounted, mutex-guarded queue of
fixed-size `hl_linux_transfer_wire`-shaped records, with attachments retained as extra references on the
counter objects in the host handle table until `receive` mints receiver-local handles. Back the queue with a
manual-reset `Event` so `event.control` can poll it through feeder 2.

| Callback | Linux | Win32 | Gaps | Diff. | Ph.1 |
|---|---|---|---|---|---|
| `channel_pair` | `socketpair(SOCK_SEQPACKET)` | allocate a shared queue object; two handle-table entries | None. | moderate | yes |
| `send(data, attachments, n)` | `sendmsg` + `SCM_RIGHTS` | enqueue the record; addref each attached counter; `SetEvent` | Rights validation (`attachments[i].rights & entry->reserved`) carries over verbatim. | moderate | yes |
| `receive(data, attachments, cap)` | `recv(MSG_PEEK)` to size, then `recvmsg(MSG_CMSG_CLOEXEC)` | peek the head record under the lock, return `HL_STATUS_RESOURCE_LIMIT` with the sizes if it does not fit, else dequeue and mint handles | The peek-then-consume protocol maps directly and is *easier* in-process. | moderate | yes |
| `duplicate` | `dup` | refcount the endpoint | None. | trivial | yes |
| `close` | `close` | decref; free the queue and drop retained attachments at zero | None. | trivial | yes |

**What is lost:** cross-process capability transfer. On Windows that is unreachable regardless — without `fork`
there is no second engine process to transfer to.

---

## 12. stream (`HL_HOST_STREAM_ABI 1`)

| Callback | Linux | Win32 | Gaps | Diff. | Ph.1 |
|---|---|---|---|---|---|
| `pipe_pair(flags)` | `pipe2(O_CLOEXEC\|O_NONBLOCK)` | `CreateNamedPipeW` with a GUID-unique name, `PIPE_ACCESS_INBOUND \| FILE_FLAG_OVERLAPPED`, `PIPE_TYPE_BYTE \| PIPE_WAIT`, then `CreateFileW` on that name for the write end | **`CreatePipe` is unusable**: it returns non-overlapped handles, and a non-overlapped handle cannot be associated with an IOCP, cannot be polled, and cannot be cancelled cleanly. The named-pipe route is the standard replacement. Name collisions need a retry loop. Both handles must be `FILE_FLAG_OVERLAPPED` for the event group to work. | moderate | yes |
| `read` / `write` | `read`/`write`, with a `SIGPIPE`-blocking dance around the write (`host.c:1207-1226`) | `ReadFile`/`WriteFile` on the overlapped handle, waiting on the `OVERLAPPED` event for the synchronous case | **The entire `SIGPIPE` mechanism disappears** — Windows has no `SIGPIPE`, so the `pthread_sigmask`/`sigtimedwait` bracket is deleted, not ported. `ERROR_BROKEN_PIPE` → `HL_STATUS_DISCONNECTED`. Overlapped handles have no implicit file pointer, so every read/write must carry its own `OVERLAPPED`. | moderate | yes |
| `duplicate` | `fcntl(F_DUPFD_CLOEXEC)` | `DuplicateHandle` | None. | trivial | yes |
| `close` | `close` | `CancelIoEx` any in-flight overlapped op, then `CloseHandle` | Closing a handle with an outstanding overlapped operation whose `OVERLAPPED` lives on a freed heap block is a classic use-after-free. Cancel first, wait for the cancellation to complete, then close. | moderate | yes |
| `set_status_flags(NONBLOCK)` | `fcntl(F_SETFL, O_NONBLOCK)` | a per-stream flag in the handle entry; the read/write paths branch on it | Windows has no per-handle non-blocking mode for pipes. Non-blocking is synthesized: issue the overlapped op, and if it does not complete immediately, `CancelIoEx` and return `HL_STATUS_WOULD_BLOCK`. (`PIPE_NOWAIT` exists and Microsoft documents it as legacy/not-recommended; it also does not compose with overlapped I/O.) The cancel-on-would-block path can lose bytes if the cancellation races a completion — it must check `GetOverlappedResult` after cancelling and honour a late success. | **hard** | yes |
| `readiness(interests)` | `poll(..., 0)` on a pinned dup | READ: zero-byte overlapped `ReadFile` probe, or `PeekNamedPipe`. WRITE: `NtQueryInformationFile(FilePipeLocalInformation).WriteQuotaAvailable` | `PeekNamedPipe` is the cheap synchronous READ answer and does not consume. Note the Linux backend deliberately accepts `FILE`-kind handles here too (`host.c:1245-1257`) — a Windows version must accept console handles and disk files, both of which are always ready. | moderate | yes |
| `move(src, soff, dst, doff, size, flags)` | `splice(2)` | a bounded read-into-a-stack-buffer + write loop | **No general zero-copy.** `TransmitFile` requires a socket destination and `mswsock`. `NtCopyFileChunk` (Win11) is file→file, which the contract explicitly rejects. So it is a loop — and the loop is the hard part, because the contract says `move` "consumes exactly the bytes reported in value and never consumes bytes which the destination did not accept" (`host_services.h:632-634`). A naive read-then-write violates that the moment the write short-writes: the bytes are already gone from the pipe. Correct implementation queries the destination's accept capacity **first** (`WriteQuotaAvailable` for a pipe, unbounded for a file), reads at most that much, then writes. Getting this exactly right is the second-most-subtle thing in the backend. | **hard** | yes |

---

## 13. network (`HL_HOST_NETWORK_ABI 1`)

The most mechanical group. Winsock2 is a near-clone of BSD sockets.

| Callback | Linux | Win32 | Gaps | Diff. | Ph.1 |
|---|---|---|---|---|---|
| `socket(family, type, protocol)` | `socket(..., SOCK_CLOEXEC)` | `WSASocketW(..., WSA_FLAG_OVERLAPPED)` after a one-time `WSAStartup(MAKEWORD(2,2))` | `SOCKET` is `UINT_PTR`, not `int`; the invalid value is `INVALID_SOCKET`, not `-1`; errors come from `WSAGetLastError`, not `errno`. **`HL_HOST_NETWORK_LOCAL` (AF_UNIX) is `SOCK_STREAM` only** on Windows — `SOCK_DGRAM` and `SOCK_SEQPACKET` are unsupported, so `LOCAL`+`DATAGRAM` must return `HL_STATUS_NOT_SUPPORTED`. `WSA_FLAG_OVERLAPPED` is required for the event group. | moderate | yes |
| `bind` | `bind` | `bind` | For `AF_UNIX`, `sun_path` is a **Windows** path, and the abstract namespace (leading NUL) is unsupported. The 108-byte `local_path` field in `hl_host_network_address` is large enough but the contents need path translation. | moderate | yes |
| `connect` | `connect` | `connect` | Same `AF_UNIX` caveats. Non-blocking connect reports `WSAEWOULDBLOCK`, mapping to `HL_STATUS_WOULD_BLOCK`. | trivial | yes |
| `send` | `send` (with `MSG_NOSIGNAL` on Linux) | `send` | No `SIGPIPE`, so `MSG_NOSIGNAL` has nothing to translate to. The `flags` argument's Linux values (`MSG_DONTWAIT`, `MSG_NOSIGNAL`, …) do **not** match Winsock's and need an explicit mapping table; passing them through numerically would be a silent misinterpretation. | moderate | yes |
| `receive` | `recv` | `recv` | Same flag-mapping issue. | moderate | yes |
| `close` | `close` | `closesocket` | Not `CloseHandle`. | trivial | yes |

The one non-obvious cost is that `SOCKET` and `HANDLE` are different types with different closers, so the handle
table's entry union must distinguish them and every close path must dispatch on kind — a class of bug the Linux
backend does not have, since everything is an `int`.

---

## 14. shared_memory (`HL_HOST_SHARED_MEMORY_ABI 1`)

| Callback | Linux | Win32 | Gaps | Diff. | Ph.1 |
|---|---|---|---|---|---|
| `create(size, flags)` | `memfd_create` + `ftruncate` | `CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_EXECUTE_READWRITE, hi, lo, NULL)` | Max protection must be chosen at creation (see §4); pick the widest the caller could later need. | trivial | yes |
| `open(identity, flags)` | `fcntl(F_DUPFD_CLOEXEC)` on the live source | `DuplicateHandle` of the live section | The "reopen identity valid while the source handle is live" contract maps exactly. | trivial | yes |
| `resize(object, size)` | `ftruncate` — grows **and shrinks** | `NtExtendSection` — **grows only** | **A real gap.** A Win32 section's size is fixed at `CreateFileMapping` time. `NtExtendSection` (`ntdll`, undocumented but long-standing and used by SQL Server and the .NET runtime) can enlarge a pagefile-backed section; nothing can shrink one. Shrink → `HL_STATUS_NOT_SUPPORTED`. A guest `ftruncate`-down on a memfd will fail where Linux succeeds. Alternative: back the section with a real temp file so `SetFileInformationByHandle(FileEndOfFileInfo)` works — but the existing views do not follow the new size, which is the same problem in a different place. | **hard** | yes (grow-only) |
| `close` | `close` | `CloseHandle` | None. | trivial | yes |

---

## 15. directory (`HL_HOST_DIRECTORY_ABI 1`) — omit in phase 1

Linux uses `inotify`. Windows uses `ReadDirectoryChangesW` on an overlapped directory handle, completing on the
IOCP (feeder 4).

| Callback | Linux | Win32 | Gaps | Diff. |
|---|---|---|---|---|
| `create` | `inotify_init1` | allocate the instance object; no kernel object yet | Structural difference: one `inotify` fd carries *many* watches; Windows needs one open `HANDLE` and one outstanding `ReadDirectoryChangesW` **per watched directory**. | moderate |
| `add(file, token, interests)` | `inotify_add_watch` via `/proc/self/fd` | `DuplicateHandle` the directory, associate with the IOCP, issue `ReadDirectoryChangesW` with the mapped filter | Filter mapping: `CREATE`/`DELETE`/`RENAME` → `FILE_NOTIFY_CHANGE_FILE_NAME \| _DIR_NAME`; `MODIFY` → `_LAST_WRITE \| _SIZE`; `ATTRIB` → `_ATTRIBUTES \| _SECURITY`. **`HL_HOST_DIRECTORY_ACCESS` has no analogue at all** — Windows never reports opens, reads, or closes. That interest must return `HL_STATUS_NOT_SUPPORTED` or be silently dropped, and DOCS §2 item 7 forbids the latter. | moderate |
| `modify` | update the stored interests | cancel + re-issue with a new filter | Racy in the same way as `event.control` MOD. | moderate |
| `remove` | `inotify_rm_watch` | `CancelIoEx` + `CloseHandle` + drain | | moderate |
| `read(records, cap)` | `read` the inotify fd, coalesce into pending records | drain completed `FILE_NOTIFY_INFORMATION` buffers into the same pending-record queue | `IN_DELETE_SELF` / `IN_MOVE_SELF` for the watched directory itself are **not reported** by `ReadDirectoryChangesW` — Windows reports children only. Reproducing them requires also watching the parent, doubling the handle count. Buffer overflow → `ERROR_NOTIFY_ENUM_DIR`, which maps cleanly to `HL_HOST_DIRECTORY_IGNORED` / `IN_Q_OVERFLOW`. `HL_HOST_DIRECTORY_ONESHOT` must be emulated by not re-issuing. | **hard** |
| `duplicate` | `fcntl(F_DUPFD_CLOEXEC)` + refcount | refcount the instance | | trivial |
| `close` | close + free at refcount zero | cancel all, drain, close, free | | moderate |

**Recommend omitting.** `src/linux_abi/syscall/binding.c:290-291` needs `WATCH | EVENT` together and degrades to
`NULL` when either is missing, so guest `inotify` on bound fds simply reports unavailable. That is a defensible
phase-1 hole; the `_ACCESS` gap means the group can never be *complete* anyway, and DOCS §3.5 says "a backend
advertises only complete groups".

---

## 16. watch (`HL_HOST_WATCH_ABI 1`) — omit in phase 1

The Linux implementation is `inotify` **plus** an `fstat` diff (`hl_linux_watch_refresh`, `host.c:2455-2481`);
the notification only wakes the dispatcher, and every reported field comes from the stat comparison.

That structure makes a cheap Windows version possible: implement `query` and `drain` purely by re-querying
`GetFileInformationByHandleEx` and diffing, exactly as `hl_linux_watch_refresh` does, with no notification source
at all. The header's contract — *"query returns current state even when the host coalesced notifications"*
(`host_services.h:615-617`) — is satisfied. The only thing lost is that the watch handle never becomes readable
in `event.control`, so a dispatcher that waits instead of polling never wakes.

| Callback | Linux | Win32 | Gaps | Diff. |
|---|---|---|---|---|
| `open(file)` | `dup` + `inotify_init1` + `inotify_add_watch` + `fstat` baseline | `DuplicateHandle` + `GetFileInformationByHandleEx` baseline; optionally `ReadDirectoryChangesW` on the parent filtered to the leaf name | The optional half is what makes it pollable and is the expensive part. | moderate |
| `query(record)` | refresh from `fstat`, return the record | refresh from `GetFileInformationByHandleEx`, identical diff logic | `HL_HOST_WATCH_DELETED` is derived from `st_nlink == 0`; Windows `NumberOfLinks` behaves the same on NTFS. | trivial |
| `drain(records, cap)` | drain inotify, fold into the record, refresh | drain the directory-change buffer if present, then refresh | Without the notification source, `drain` degenerates to `query` plus a generation check — correct, never spontaneously ready. | moderate |
| `close` | close both fds, free | `CloseHandle`, free | | trivial |

**Recommend omitting** for the same reason as `directory`: it is only meaningfully complete alongside it.

---

## 17. process (`HL_HOST_PROCESS_ABI 3`) — omit in phase 1

| Callback | Linux | Win32 | Verdict |
|---|---|---|---|
| `spawn_cloned(entry, ctx)` | `fork()`, child calls `entry` and `_exit`s | — | **blocked**, §1.1 |
| `spawn_prepared(entry, ctx)` | `fork()` inside a `sync.fork_prepare` bracket | — | **blocked** |
| `wait(process, deadline)` | `waitpid` + a condvar for repeated/concurrent waiters | `WaitForSingleObject(hProcess, ms)` + `GetExitCodeProcess`; the retained-completion condvar logic ports directly | moderate, but unreachable without a spawn |
| `terminate(process, reason)` | `kill(SIGINT/SIGKILL/n)` | `TerminateProcess` for FORCE; `GenerateConsoleCtrlEvent(CTRL_C_EVENT)` for INTERRUPT; arbitrary Linux signal numbers have no analogue | moderate, partial |
| `close(process)` | drop the handle-table entry | `CloseHandle` | trivial |

Omission cost: `src/core/lifecycle.c:109` is the only caller and it converts a missing capability to
`HL_STATUS_NOT_SUPPORTED`. Guest `fork`/`vfork` fail. Guest threads are unaffected.

---

## 18. posix_attachment (`HL_HOST_POSIX_ATTACHMENT_ABI 2`) — do not advertise

Recommended, and the evidence is unambiguous.

**Legal.** `hl_host_services_validate`'s POSIX-attachment clause (`src/core/host_services.c:50-55`) is entirely
inert when the bit is clear. No caller anywhere passes `HL_HOST_CAP_POSIX_ATTACHMENT` in a
`required_capabilities` mask. `src/host/fake/host.c` already ships a backend that omits it. `tests/unit/test_host_services.c`
never mentions it.

**And unimplementable anyway.** The group's whole purpose is to hand out a *native POSIX file descriptor number*
so that the Linux ABI layer can use it with `openat`, `ioctl`, `fcntl`, and `sendmsg`/`SCM_RIGHTS`. Windows has
no such number to hand out — the CRT's `_open_osfhandle` produces an integer, but it is a CRT-internal index,
not a kernel descriptor, and it means nothing to `NtCreateFile`.

**What breaks, precisely:**

| Consumer | Behaviour without the group |
|---|---|
| `src/linux_abi/container/vfs.c:1455-1498` `root_handle_bind` | **`return -1`** → `src/core/target/x86_64.c:801,812` and `aarch64.c:822,834` `return -1` → **engine init fails, for containers and bare guests alike.** No fallback exists. |
| `src/linux_abi/syscall/binding.c:3410` (termios/TTY `ioctl` block) | `-EOPNOTSUPP` where Linux returns terminal settings |
| `src/linux_abi/syscall/binding.c:3720` (`fcntl` `F_SETOWN`/`F_OFD_*`/`F_*PIPE_SZ`/`F_*SEALS`) | `-EOPNOTSUPP`; the code comment notes the prior behaviour was `-EINVAL`, so this is a superset of a working degraded mode |
| `src/linux_abi/container/netns.c:1099` (`SCM_RIGHTS` over unix sockets) | `sendmsg` fails with `EOPNOTSUPP` for a bound fd |

The last three degrade gracefully and are fine. The first is the blocker, and it is **not a host-backend problem**
— it is §1.2's problem. `root_handle_bind` must gain a path that pins the namespace root through
`file.open_relative` / `file.resolve_beneath` and keeps an opaque `hl_host_handle` rather than an `int`, and the
61 `*at` call sites downstream must follow. Budget that separately.

---

## 19. `COMMON_HOST_SOURCES`

Defined at `CMakeLists.txt:167-169` as six files. Backend selection (`CMakeLists.txt:171-200`) has **no `else()`
branch** — a Windows configure today builds core + translator + ABI + fake, with no host archive at all.
`cmake/Phase2Production.cmake:65-66` early-`return()`s on non-Linux.

| File | Lines | POSIX call sites | Verdict | Note |
|---|---|---|---|---|
| `src/host/range.c` | 26 | 0 | **REUSE** | Pure page arithmetic. Needs `src/host/windows/range.c` supplying `hl_host_page_size` (`GetSystemInfo`) and `hl_host_address_mapped` (`VirtualQuery`) — ~50 lines. |
| `src/host/file.c` | 135 | 0 | **REUSE** | Pure `hl_host_services` vtable wrapper. Not in `COMMON_HOST_SOURCES`; appended per-OS. Compiles verbatim. |
| `src/host/sync.c` | 207 | ~29, all pthread | **REUSE-WITH-SHIM** | ~170 lines portable; swap the ~30-line primitive layer for `SRWLOCK` + owner TID (§7). Likely compiles as-is against mingw-w64 winpthreads for a first boot. |
| `src/host/resolve.c` | 260 | ~14 (`openat`, `fstatat`, `readlinkat`, `fcntl`, `dup`, `close`) | **NEEDS-WINDOWS-SIBLING** | The algorithm transcribes; every leaf operation changes. `resolve.h` exposes `int parent_fd/target_fd` and needs a Windows shape. |
| `src/host/private.c` | 439 | ~20 (`mmap`, `getrlimit`, `fcntl`, `getpid`, pthread, `sysctlbyname`) | **NEEDS-WINDOWS-SIBLING** | Its *premises* are POSIX, not just its calls (§2.3). Replace with a ~40-line no-op shim in the shape of `src/host/fake/host.c:1351-1379`. `<sys/mman.h>` and `<sys/resource.h>` have no mingw-w64 equivalent at all. |
| `src/host/child.c` | 69 | `pipe`, `fcntl`, `sigaction(SIGCHLD)` | **NEEDS-WINDOWS-SIBLING** | `SIGCHLD` has no analogue. Only needed alongside `process`, which phase 1 omits. |
| `src/host/fork_wire.c` | 188 | `<sys/socket.h>`, `<sys/un.h>`, `SCM_RIGHTS` | **NEEDS-WINDOWS-SIBLING** | Only needed alongside `process`. |

`src/host/fake/host.c` is the existence proof that the engine core links without any POSIX host: it includes only
`"hl/fake.h"`, `<string.h>`, and `<sched.h>`, and its sole POSIX dependency is `sched_yield()` at two spin sites
(`SwitchToThread()` on Windows). It does **not** link `COMMON_HOST_SOURCES` (`CMakeLists.txt:165,185`), which is
exactly why it stubs the three private-descriptor hooks itself.

---

## 20. Size estimate

Counting only `src/host/windows/` and the CMake to build it. Linux is 3961 lines and macOS 4839 for comparison,
neither of which carries a UTF-8/UTF-16 layer or a synthesized readiness engine.

| Component | Lines | Basis |
|---|---|---|
| `host.c` — handle table, group tables, lifecycle | 600 | Linux equivalent ≈ 550 |
| memory (14 callbacks, placeholder machinery) | 900 | Placeholder split/coalesce is new logic with no Linux analogue |
| clock (9) | 250 | Linux ≈ 120; the sleep/backoff rows are longer |
| log (1) | 25 | |
| sync — `src/host/windows/sync.c` sibling | 220 | mostly the existing 207 with a new primitive layer |
| file (40 callbacks) | 1900 | Linux ≈ 1400; `NtCreateFile` plumbing and `FileXxxInfo` structs are wordier |
| `path.c` — WTF-8 ↔ UTF-16, NT path building | 250 | |
| `resolve.c` sibling | 350 | 260 + `HANDLE` verbosity |
| event — IOCP core, registration table, level-trigger emulation | 900 | wepoll is ~1800 for a *complete* epoll; this is a subset with a narrower contract |
| event — AFD feeder | 250 | can be deferred behind a `WSAPoll` thread (~120) in phase 1 |
| stream (8) | 500 | the overlapped state machine and `move`'s capacity-first loop dominate |
| counter (10) | 350 | |
| transfer (5) | 300 | in-process queue |
| network (6) | 300 | + flag-mapping tables |
| shared_memory (4) | 150 | |
| `range.c` sibling | 60 | |
| `private.c` shim | 40 | |
| error mapping — Win32 + NTSTATUS | 180 | |
| CMake, manifest, toolchain file | 150 | |
| **Phase 1 subtotal** | **≈ 7 700** | |
| directory (7) — phase 2 | 500 | |
| watch (4) — phase 2 | 250 | polling-only version ≈ 120 |
| process (5) — blocked | 400 | only if a `fork` substitute is ever built |
| **With phase 2** | **≈ 8 500** | |

**Not counted, and larger than any of the above:** routing `root_handle_bind` and the 61 `*at` call sites in
`src/linux_abi` through the host-services contract (§1.2, §18). That is the real critical path to a Windows host
that boots a guest.

---

## 21. Open questions to resolve before writing code

Ranked by how much of the design they can invalidate.

1. **Does `MapViewOfFile3` with `MEM_REPLACE_PLACEHOLDER` really accept a 4 KiB-aligned offset?** Microsoft
   documents that it does. If it does not, `map_file` cannot honour guest `mmap` offsets and the whole file-mapping
   story needs a bounce-buffer redesign. **One 30-line test program.**
2. **Can `VirtualProtect` / `VirtualAlloc` be called from a vectored exception handler on the faulting thread?**
   §1.3 argues yes on structural grounds. `repair_signal_page` is unimplementable if not. **One test program.**
3. **Do `VirtualAlloc2` / `MapViewOfFile3` / `UnmapViewOfFile2` link against the pinned mingw-w64?**
   See §4. If not, a `GetProcAddress` table costs 40 lines. **One test link.**
4. **Which of `QueryUnbiasedInterruptTimePrecise` and `QueryPerformanceCounter` matches `CLOCK_MONOTONIC` across
   suspend?** Affects every guest timeout. **One test across a sleep/resume cycle.**
5. **Is Developer Mode an acceptable requirement?** `make_symlink` cannot work without it (or admin). If not,
   guest `symlink(2)` fails permanently and that must be documented as a platform limitation, not a bug.
6. **`FileRenameInfoEx` / `FileDispositionInfoEx` POSIX semantics on the CI volume.** NTFS-only. If CI runs on a
   network share or a container overlay, `rename` and `unlink` silently fall back to legacy Windows semantics and
   a large class of guest behaviour breaks in ways that look like engine bugs.
7. **Permission-bit policy** (§8 preamble) — a contract-level decision, not an implementation detail.
8. **`stable_object` truncation on ReFS.** 128→64 bits. Probably irrelevant for CI; note it before someone hits it.

---

## Sources

- [VirtualAlloc2 (memoryapi.h)](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualalloc2) — placeholder reservation, `MEM_ADDRESS_REQUIREMENTS.Alignment`, `onecore.lib` / `Kernel32.dll`, Win10+
- [MapViewOfFile3 (memoryapi.h)](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-mapviewoffile3) — `MEM_REPLACE_PLACEHOLDER`, the 64 KiB-offset relaxation, Win10 1803+
- [FILE_ID_INFO (winbase.h)](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_id_info) — 128-bit file id + volume serial
- [GetFileInformationByHandleEx](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-getfileinformationbyhandleex)
- [SetFileInformationByHandle](https://learn.microsoft.com/en-us/windows/desktop/api/FileAPI/nf-fileapi-setfileinformationbyhandle) — `FileRenameInfoEx`, `FileDispositionInfoEx`
- [OBJECT_ATTRIBUTES (ntdef.h)](https://learn.microsoft.com/en-us/windows/win32/api/ntdef/ns-ntdef-_object_attributes) — `RootDirectory`, `OBJ_DONT_REPARSE`
- [NtCreateFile](https://learn.microsoft.com/en-us/windows/win32/api/winternl/nf-winternl-ntcreatefile)
- [CreateSymbolicLinkW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-createsymboliclinkw) — `SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE`, Developer Mode
- [AF_UNIX comes to Windows](https://devblogs.microsoft.com/commandline/af_unix-comes-to-windows/) — `SOCK_STREAM` only, no ancillary data, no `socketpair`
- [Windows/WSL Interop with AF_UNIX](https://devblogs.microsoft.com/commandline/windowswsl-interop-with-af_unix/) — no `SCM_RIGHTS`
- [wepoll: fast epoll for windows](https://github.com/piscisaureus/wepoll) — the AFD/IOCP readiness design
- [\Device\Afd, or, the Deal with the Devil that makes async Rust work on Windows](https://notgull.net/device-afd/)
- [Adventures with \Device\Afd — Len Holgate](https://lenholgate.com/blog/2023/04/adventures-with-afd.html)
- [mio issue #281 — use IOCTL_AFD_POLL](https://github.com/tokio-rs/mio/issues/281)
- [mingw-w64 issue #27 — VirtualAlloc2/MapViewOfFile3 missing from mingw libs](https://github.com/mingw-w64/mingw-w64/issues/27)
- [mingw-w64-public patch, Oct 2023 — MapViewOfFile3/VirtualAlloc2 in api-ms-win-core-memory](https://sourceforge.net/p/mingw-w64/mailman/mingw-w64-public/thread/20231025193048.28648-2-mark@harmstone.com/)
