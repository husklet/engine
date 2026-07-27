# Prior art: flinux's memory manager

What `wishstudio/flinux` ("Foreign Linux") does to present a 4-KiB Linux `mmap`/`mprotect`/`munmap` interface on
top of NT's 64-KiB allocation granularity, what that costs, and which of it survives contact with
`VirtualAlloc2` placeholders — an API flinux predates.

---

## 0. Licensing — read this before using anything below

**flinux is GPLv3. HL Engine is MIT. They are incompatible.**

No flinux source may be copied into `src/`, and the Windows backend must not be a derivative work of flinux.
This document therefore records **techniques, algorithms, constraints and measurements** — none of which are
copyrightable — with `file:line` citations so a reader can check the claim against the original. The few code
fragments quoted are one- or two-line excerpts used to make a specific technical point, not a transliteration.

Anyone implementing `src/host/windows/` must write it independently from an understanding of the *approach*,
never from flinux's text. Where this document says "adopt", it means "adopt the idea", never "copy the code".

---

## 1. Scope and method

**Read:** `src/syscall/mm.c` (1711 lines) and `src/syscall/mm.h` in full, plus the call sites that constrain
them: `src/syscall/syscall.c` (the vectored handler), `src/syscall/fork.c`, `src/syscall/exec.c`,
`src/syscall/stubs64.asm`, `src/heap.c`, `src/shared.c`, `src/common/mman.h`, `src/lib/rbtree.h`,
`flinux.vcxproj`. Tree at commit `a041253` (shallow clone; pre-`a041253` history was not available, so
"this was added when" questions are unanswerable here).

**Measured:** every load-bearing NT behaviour below was measured rather than reasoned about, on

- Windows 11 Pro, build 10.0.26200.8246, x86-64
- clang 22.1.8, target `x86_64-w64-windows-gnu` (`C:\msys64\clang64\bin\clang.exe`), `-lmincore`

Four scratch programs (`memprobe.c`, `memprobe2.c`, `memprobe3.c`, `commit.c`) produced the numbers. Raw output
is summarised in §9; the programs live in the session scratchpad and are not part of the repo. Where a claim was
**not** measured, it says so.

**Out of scope by assignment:** process creation and the `fork()` bracket, the fd/VFS layer, `src/dbt` and
signal delivery. §7 touches fork only where the *memory* mechanism is the subject.

---

## 2. The 64-KiB block architecture

### 2.1 The problem, stated in their own header

flinux opens `mm.c` with a comment that frames the whole design (`src/syscall/mm.c:38-53`): Linux maps at 4-KiB
boundaries, Windows reserves at 64-KiB boundaries, and this "causes two main issues" — a file cannot be mapped
at a non-64-KiB-aligned offset using Windows file-mapping calls at all, and `MAP_FIXED` at a non-64-KiB address
requires allocating whole 64-KiB blocks and sub-allocating inside them. Their conclusion, written in 2014, is
blunt: *"it seems impossible to implement MAP_FIXED with MAP_SHARED or MAP_PRIVATE on non 64kB aligned
address."* Under the API available to them, that was true. §5 shows it is no longer true for us.

### 2.2 The unit: one 64-KiB block, one pagefile-backed section object

The address space is cut into fixed 64-KiB **blocks** at fixed addresses — block *i* *is* the range
`[i·64 KiB, (i+1)·64 KiB)`. There is no allocator choosing block addresses; the block index is a pure function
of the address (`mm.h:31`, `mm.c:96-103`).

Each block that has ever been materialised owns exactly one kernel **section object**, created and mapped by
`allocate_block()` (`mm.c:668-703`):

- `NtCreateSection(..., PAGE_EXECUTE_READWRITE, SEC_COMMIT, NULL)` — pagefile-backed, 64 KiB,
  `OBJ_INHERIT` so the handle survives into a fork child
- `NtMapViewOfSection(..., base = block address, PAGE_EXECUTE_READWRITE)`

Two properties are deliberate. **`PAGE_EXECUTE_READWRITE` as the section's maximum protection** exists so that
per-page protection can later be *promoted* as well as demoted; §4 shows why that is mandatory. **`OBJ_INHERIT`**
is what makes the fork scheme in §7 possible at all.

### 2.3 Two indices, not one

There are two independent data structures, and conflating them is the main way to misread this code.

**(a) The guest VM map — a red-black tree of `map_entry`** (`mm.c:109-124`, tree ops `mm.c:126-136`,
`mm.c:251-287`). One entry per contiguous guest mapping, keyed by start page, carrying
`{start_page, end_page, prot, flags, struct file *f, offset_pages}`. This is the authority for what the *guest*
believes; `/proc/self/maps` is generated straight from it (`mm.c:565-599`). Entries are page-granular, and
`split_map_entry()` (`mm.c:273-287`) divides one at any 4-KiB boundary.

The entries come from a **fixed-size static array**, not a heap: `MAX_MMAP_COUNT` is 1024 (`mm.c:57`), the array
is embedded in the global `mm_data` (`mm.c:165`), and exhaustion is a logged hard failure with no fallback
(`mm.c:234-244`). 1024 VMAs is a low ceiling — a moderately complex glibc process can approach it.

**(b) The block → section-handle table — a flat array indexed by block number** (`mm.c:171`,
accessors `mm.c:173-211`). This is where the address-space cost lives, and the arithmetic is worth doing
explicitly for x86-64:

| Quantity | Value | Source |
|---|---|---|
| `ADDRESS_SPACE_HIGH` | `0x0001_0000_0000_0000` = 2⁴⁸ | `mm.c:64` |
| `BLOCK_SIZE` | 64 KiB = 2¹⁶ | `mm.h:31` |
| `BLOCK_COUNT` | 2³² | `mm.c:84` |
| `mm_section_handle` reservation | 2³² × 8 B = **32 GiB** of `MEM_RESERVE` | `mm.c:351` |
| `SECTION_HANDLE_PER_TABLE` | 64 KiB / 8 = 8192 handles per commit unit | `mm.c:86` |
| `SECTION_TABLE_COUNT` | 2¹⁹ = 524 288 | `mm.c:88` |
| `section_table_handle_count[]` | 524 288 × `uint16_t` = **1 MiB, always resident** | `mm.c:168` |
| `entries[1024]` | 1024 × 64 B = 64 KiB (`rb_node` is 24 B, `rbtree.h:5-9`) | `mm.c:165` |

So the fixed overhead is **32 GiB of reserved address space plus ~1.1 MiB of always-resident static data**,
before a single guest page exists. The 32 GiB reservation is cheaper than it looks — measured, it succeeds in
**0.001 ms and takes zero commit charge** (§9, probe 1 test 6), and 32 GiB is 0.025 % of the 128 TiB of user
address space Windows actually offers. It is a two-level sparse table: a 64-KiB table page is committed on the
first handle stored in it and decommitted when its refcount returns to zero (`mm.c:182-211`), so the *committed*
cost is 64 KiB of table per 512 MiB of guest address space touched.

Two flaws in this table are worth recording because they show the design straining against 64-bit:

- `ADDRESS_SPACE_HIGH` is 2⁴⁸, but the measured `lpMaximumApplicationAddress` on this machine is
  `0x00007FFF_FFFEFFFF` — about 2⁴⁷. The upper half of the block table can never be populated. Harmless only
  because `find_free_pages()` starts low and works up (`mm.c:441`).
- `mm_shutdown()` (`mm.c:398-412`) iterates all 2³² blocks. It is **never called** — `grep` finds only the
  definition, while `mm_reset()` is called from `exec.c:406` — so it is dead code rather than a shutdown stall.
  It is still a fair signal that a flat block-indexed array was designed for the 32-bit case (2¹⁶ blocks) and
  scaled up without revisiting.

### 2.4 What a guest `mmap` at an arbitrary 4-KiB address actually does

`mmap_internal()` (`mm.c:1105-1302`) is the whole story. For an anonymous private mapping at an arbitrary
4-KiB-aligned address:

1. Validate, round length up to a page, reject `INTERNAL_MAP_VIRTUALALLOC` requests that are not block-aligned
   (`mm.c:1124-1129`).
2. If not `MAP_FIXED`, pick an address from flinux's **own** free-space scan over the `map_entry` tree
   (`find_free_pages`, `mm.c:439-461`, or the top-down variant `mm.c:464-489`) — a linear walk of the tree, not
   an NT query. Windows is never asked to choose.
3. If `MAP_FIXED`, either reject on overlap (`INTERNAL_MAP_NOOVERWRITE`) or **call `munmap_internal()` on the
   range first** (`mm.c:1199-1220`) — so `MAP_FIXED` is unmap-then-map, not atomic replace.
4. Insert one `map_entry` into the rb-tree (`mm.c:1222-1238`).
5. **Materialise only the first and last blocks, and only if they already exist.** If the first block already
   has a section (because a neighbouring mapping shares it), take ownership of it (§7), load it if detached,
   temporarily force it writable, fill the sub-range, and restore the requested protection
   (`mm.c:1256-1274`); same for the last block (`mm.c:1275-1292`). Interior blocks are left **unmapped**.
6. Only `MAP_POPULATE` (and, via `mm.c:1130-1136`, `MAP_SHARED`) eagerly allocates the interior blocks
   (`mm.c:1293-1299`).

So the steady-state answer is: **a guest `mmap` is usually just a red-black-tree insertion.** No NT call is
made for the interior. The address range becomes real on first touch, in the vectored handler.

### 2.5 First touch: `mm_handle_page_fault`

The vectored exception handler (`syscall.c:38-129`, installed `syscall.c:131`) routes every access violation to
`mm_handle_page_fault()` (`mm.c:931-963`), which dispatches on two bits:

| Block has a section handle? | Write? | Action |
|---|---|---|
| no | — | `handle_on_demand_page_fault()` — create the section, map it, fill every `map_entry` range inside the block from file or zeros, then apply per-entry protection (`mm.c:894-929`) |
| yes | no | `load_detached_block()` — the section exists but its view is not mapped (a fork child); map it read-only-ish (`mm.c:837-855`) |
| yes | yes | `handle_cow_page_fault()` — the copy-on-write path (§7, `mm.c:857-892`) |

The unit of materialisation is **the whole 64-KiB block**, always. A guest that touches one page of a sparse
mapping gets 64 KiB committed. Against Linux's 4 KiB that is a **16× commit amplification** on sparse
workloads, and it compounds with §2.6.

### 2.6 `SEC_COMMIT` is a measured scaling wall

`allocate_block()` creates every section with `SEC_COMMIT` (`mm.c:683`). Measured (§9, `commit.c`):

- Creating 8000 × 64-KiB `SEC_COMMIT` sections — **without mapping any view** — raised system commit charge by
  **503 MiB** for 500 MiB nominal.
- Mapping all 8000 views added ~1 MiB more. The charge is taken at *section creation*, not at view mapping or
  first touch.
- A plain `VirtualAlloc(500 MiB, MEM_RESERVE)` took **0 MiB** of commit charge.

Linux overcommits by default; flinux cannot. Every 64-KiB block the guest has ever touched holds pagefile
commit until its last `map_entry` is unmapped. `SEC_RESERVE` would have avoided this; they did not use it.

Handle pressure is the second half: measured, 20 000 section objects (1.25 GiB of guest address space) put the
process handle count at 20 076, created in 20.6 ms (§9, probe 3 test N). One kernel handle per 64 KiB of guest
address space is a real, if survivable, cost.

---

## 3. The `VirtualAlloc` silent-round-down trap, and how flinux avoids it

### 3.1 The trap is real — measured

`VirtualAlloc(lpAddress, ...)` rounds `lpAddress` **down** to the 64-KiB allocation granularity and reports
success at the *wrong* address. Measured (§9, probe 1 test 1):

```
  VirtualAlloc(want=0x…345031000, 64K, RESERVE|COMMIT) -> 0x…345030000
    ROUNDED DOWN BY 0x1000  (silent success at wrong address: YES)
```

The return value is non-NULL, `GetLastError()` is untouched stale garbage, and a caller that checks only
`p != NULL` has just placed a guest mapping 4 KiB below where the guest asked. This confirms the prior-art
survey's day-one flag: **`VirtualAlloc` fixed-address requests must be treated as advisory and the returned
address compared against the request.**

### 3.2 flinux's answer: never call `VirtualAlloc` for guest memory

flinux does not defend against the round-down. It structurally cannot hit it, because guest mappings never go
through `VirtualAlloc` at all — they go through `NtMapViewOfSection` at block-aligned addresses. The only
`VirtualAlloc` calls in the memory path are:

- the 32-GiB shadow-table reservation (`mm.c:351`) — `NULL` address, no fixed request;
- the `INTERNAL_MAP_VIRTUALALLOC` escape hatch (`mm.c:1243`), which is **validated block-aligned on entry**
  and rejected with `EINVAL` otherwise (`mm.c:1124-1129`).

That second check is the actual defence and it is a good pattern: *refuse* misaligned fixed requests at the API
boundary rather than letting the kernel round them. Note also that `NtMapViewOfSection` does **not** silently
round — measured, `MapViewOfFileEx` at a 4-KiB-aligned base fails cleanly with `ERROR_MAPPED_ALIGNMENT` (1132),
as does a 4-KiB view offset (§9, probe 1 test 3). Section mapping is fail-loud where `VirtualAlloc` is
fail-silent, which is a good reason to prefer it when both would work.

### 3.3 Does flinux reserve address space up front?

**No — not for guest memory.** There is no large up-front reservation of the guest address range. What exists is:

- the 32-GiB shadow **table** reservation (`mm.c:351`), which is bookkeeping, not guest memory;
- a **fixed allocation window** enforced purely in software: `ADDRESS_ALLOCATION_LOW = 0x2_0000_0000` (8 GiB) to
  `ADDRESS_ALLOCATION_HIGH = 0x1_0000_0000_0000` (`mm.c:66-68`). `find_free_pages()` only ever returns addresses
  in that window (`mm.c:441`, `mm.c:454`), but nothing *reserves* it, so a Windows DLL or the CRT heap may land
  inside it and flinux will only find out when `NtMapViewOfSection` fails.
- one genuine up-front reservation, and it is not in `mm.c`: the ET_EXEC load address (§8.1).

---

## 4. `mprotect` at 4 KiB: what is free, what forces a split

This is the part of the design that works cleanly, and the measurements confirm flinux's premises.

**NT page protection is genuinely per-4-KiB-page, including inside a mapped section view.** Measured (§9,
probe 1 test 3, probe 2 test C): inside a single 64-KiB section view, `VirtualProtect` of one 4-KiB page to
`PAGE_READONLY`, `PAGE_NOACCESS` and `PAGE_EXECUTE_READ` all succeed, and `VirtualQuery` afterwards reports
separate 4-KiB regions with distinct `Protect` values sharing one `AllocationBase`.

**What is 64-KiB-granular is the *view*, not the protection.** Two consequences, both measured:

1. **`VirtualProtect` cannot span two allocation regions.** Measured (probe 2 test C): protecting 8 KiB that
   straddles the boundary between two adjacent 64-KiB section views fails with `ERROR_INVALID_ADDRESS` (487),
   while the same call wholly inside one view succeeds. This is exactly why `mm_change_protection()`
   (`mm.c:618-646`) **iterates block by block** and clips each call to the block, instead of issuing one call
   for the whole range.
2. **A block with no section handle is skipped, not failed.** `mm_change_protection()` only calls
   `NtProtectVirtualMemory` for blocks that have a handle (`mm.c:625-626`) and explicitly tolerates
   `STATUS_CONFLICTING_ADDRESSES` as "block not yet mapped, silently ignore" (`mm.c:635-636`). Measured, the
   Win32 spelling of that condition is `ERROR_INVALID_ADDRESS` (487) on both reserved-not-committed and free
   address space (probe 1 test 9). Protection for a not-yet-materialised block is *stored in the `map_entry`*
   and applied later by `load_block_protection()` (`mm.c:805-834`) when the fault handler materialises it.

**So the precise answer to "what forces a split":**

| Operation | Granularity | Splits anything? |
|---|---|---|
| Change protection of pages inside one block | 4 KiB | No NT-level split. Splits a `map_entry` if the range partially covers it (`mm.c:1526-1538`) |
| Change protection spanning blocks | must be issued per block | No re-map; N calls instead of 1 |
| Protection on a not-yet-materialised block | deferred | Recorded in the `map_entry`, applied at fault time |
| Promote protection beyond the section's max | **impossible** | Would require a new section — see below |

The last row is the one constraint that genuinely forces a re-map, and it is why every flinux section is created
`PAGE_EXECUTE_READWRITE`. Measured (probe 2 test D):

- section created `PAGE_READWRITE` → view `VirtualProtect` to `PAGE_EXECUTE_READ` **fails** (`ERROR_INVALID_PARAMETER`, 87)
- section created `PAGE_EXECUTE_READWRITE` → RX, back to RW, to `PAGE_NOACCESS`, and back to RW again all succeed
- a view mapped with `FILE_MAP_READ` only → `VirtualProtect` to `PAGE_READWRITE` **fails** (87)

That third result confirms the claim in flinux's own comment at `mm.c:723-727`: a view mapped without write
access cannot have write protection promoted afterwards. It is why `duplicate_section()` re-maps the source
elsewhere as `PAGE_READWRITE` rather than reusing the read-only view.

`sys_mprotect` itself (`mm.c:1464-1551`) is otherwise conventional: validate the whole range is mapped
(`mm.c:1482-1503`, returns `ENOMEM` on a hole, which is correct Linux behaviour), split `map_entry`s at the
edges, then apply. Two quirks worth flagging:

- **`INTERNAL_MAP_VIRTUALALLOC` regions are skipped entirely** (`mm.c:1517-1519`) — their protection is not
  tracked in `e->prot`, so `mprotect` on a guest stack silently does nothing to the bookkeeping.
- **The applied protection is `prot & ~PROT_WRITE`** (`mm.c:1541`), deliberately, "in case the pages are already
  shared". Write access is then restored lazily by the COW fault path. A guest that `mprotect`s to RW and
  immediately writes therefore takes an extra fault every time.

---

## 5. `munmap` carving holes: what they did instead of placeholders

### 5.1 The constraint, measured

`VirtualFree(MEM_RELEASE)` cannot punch a hole. Measured (probe 1 test 2): releasing one 4-KiB page from the
middle of a 64-KiB reservation fails with `ERROR_INVALID_PARAMETER` (87). `MEM_DECOMMIT` of that page succeeds,
but the address stays reserved.

Section views are worse. Measured (probe 2 test B): calling `UnmapViewOfFile` on an address 64 KiB *inside* a
256-KiB view **returns success and destroys the entire view** — `VirtualQuery` afterwards reports the whole
256 KiB as `MEM_FREE`. There is no partial-unmap error to catch; the API silently does far more than asked.

### 5.2 flinux's answer: two mechanisms, chosen by whether the block is shared

`free_map_entry_blocks()` (`mm.c:289-333`) is the whole of it:

1. If the entry is an `INTERNAL_MAP_VIRTUALALLOC` region, `VirtualFree(base, 0, MEM_RELEASE)` — whole region,
   trivially correct because those are always block-aligned (`mm.c:291-295`).
2. Otherwise, look at the neighbouring rb-tree entries. **If the first block is shared with the previous
   mapping**, the block cannot be destroyed; instead mark just this entry's pages
   `PAGE_NOACCESS` and exclude the block from the destroy loop (`mm.c:305-313`). Same for the last block against
   the next mapping (`mm.c:314-320`).
3. Every **fully-owned interior block** is destroyed properly: `NtUnmapViewOfSection` + `NtClose` +
   remove from the handle table (`mm.c:322-332`).

So the answer to "what did they do instead of placeholders" is: **`PAGE_NOACCESS` as a stand-in for
`MEM_FREE`, applied only to the ragged 64-KiB edges, with real destruction reserved for whole blocks.** The
guest's view of a hole is provided entirely by the `map_entry` tree (the hole is simply absent from it); NT's
view of a hole is faked with an inaccessible-but-still-committed page range.

`munmap_internal_unsafe()` (`mm.c:1319-1378`) does the tree side: for each overlapping entry, either free it
whole, or `split_map_entry()` at the boundary and free the piece — which is how a 4-KiB hole appears in the
*guest's* map even though NT knows nothing about it.

### 5.3 Two hazards they hit that we will hit too

**Re-entrancy.** `munmap` can be re-entered — freeing a file-backed mapping drops a `struct file` reference,
whose release path can itself `munmap`. flinux handles this with a thread-id guard and a deferred work list:
`mm_munmap()` checks whether the current thread is already inside the unmap loop and, if so, appends to
`munmap_list` instead of recursing (`mm.c:1398-1416`, list at `mm.c:213-232`, drain at `mm.c:1375-1376`). Any
backend whose `unmap_range` can trigger a handle release needs the same shape.

**Their own TODOs admit the model is incomplete.** `mm.c:1306` — *"We should mark NOACCESS for munmap()-ed but
not VirtualFree()-ed pages"*; `mm.c:889` and `mm.c:923` — *"TODO: Mark unmapped pages as PAGE_NOACCESS"*;
`mm.c:1420` — the same for `VirtualAlloc`-ed but unused pages. In the shipped code a freshly materialised block
is fully accessible even in the sub-ranges the guest never mapped, so **a guest wild pointer into a hole inside
a partly-mapped 64-KiB block reads zeros instead of faulting.** That is a correctness gap, not a performance
one.

### 5.4 Is their approach better or worse than placeholders — for us?

Worse on every axis I measured, with exactly one exception. Placeholder facts, all measured (§9, probes 2 and 3):

| Question | Measured answer |
|---|---|
| Can a placeholder be split at **4 KiB**? | **Yes.** `VirtualFree(base+0x1000, 0x1000, MEM_RELEASE\|MEM_PRESERVE_PLACEHOLDER)` succeeds; `VirtualQuery` then shows three regions of 0x1000 / 0x1000 / 0xfe000, each its own `AllocationBase`. |
| Can a **file view** land on a 4-KiB base at a 4-KiB offset? | **Yes.** `MapViewOfFile3(section, …, base+0x1000, offset=0x1000, size=0x1000, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE)` returns the exact requested address, and the page is writable. |
| Can a hole be punched in a **committed private** region? | **Yes.** After committing 1 MiB, `VirtualFree(mid, 0x1000, MEM_RELEASE\|MEM_PRESERVE_PLACEHOLDER)` leaves a 4-KiB `MEM_RESERVE` placeholder hole between two `MEM_COMMIT` regions. This is `munmap`'s hole-carving, natively. |
| Cost | 4096 × 4-KiB splits in **6.1 ms (1.49 µs each)**; 4096 private 4-KiB commits-into-placeholder in **4.2 ms (1.02 µs each)**; zero handles; zero commit charge for the reserved parts. |
| Where do the entry points live? | **`KernelBase.dll`, not `kernel32.dll`.** See §8.4 — this resolves an open question in `host-services-map.md`. |

Against that, flinux's scheme:

- **never returns address space.** A sub-block `munmap` leaves the range committed and `PAGE_NOACCESS`. Long-running
  `mmap`/`munmap` churn leaks address space and commit charge in 64-KiB units.
- costs **one kernel handle and 64 KiB of commit charge per 64 KiB of guest address space** (§2.6, measured).
- forces the *whole* VM through section objects, which is why every `mprotect` becomes a per-block loop (§4).
- leaves the correctness gap in §5.3.
- creating + mapping a 64-KiB block measured **2.33 µs**, worse than a 1.49 µs placeholder split, and it also
  consumes a handle.

**The one thing flinux's approach has that ours does not: it needs no API newer than NT 3.1.** flinux targeted
Windows 7 (`src/win7compat.c` exists for exactly that reason). We have declared a Windows 10 1803+ floor, so we
are paying nothing for placeholders.

**Verdict: placeholders win outright.** But three cautions carry over unchanged, and two are *new* constraints
placeholders impose that flinux never had:

1. *(new)* **`MEM_REPLACE_PLACEHOLDER` must cover exactly one whole placeholder.** Measured (probe 3 test I): a
   4-KiB private commit into an *exactly*-4-KiB placeholder succeeds; the same 4-KiB commit into a 64-KiB
   placeholder fails with `ERROR_INVALID_ADDRESS` (487), and so does a 4-KiB `MapViewOfFile3` into a 64-KiB
   placeholder. **The rule is "exact whole placeholder", not "64-KiB minimum".** So `unmap_range` and
   `map_file` must *split first, replace second*, and the backend must know the current split boundaries.
2. *(new)* **A placeholder under a live view cannot be split.** Measured (probe 3 test G): splitting a
   placeholder that currently holds a mapped view fails with 87; after `UnmapViewOfFile2(MEM_PRESERVE_PLACEHOLDER)`
   the same split succeeds. Unmap first, split second — as `host-services-map.md` already states.
   `MEM_COALESCE_PLACEHOLDERS` likewise requires exact placeholder boundaries: coalescing two adjacent
   placeholders exactly succeeds, coalescing a range that ends part-way into a third fails with 487 (probe 3 test J).
3. *(carried over)* **`MAP_FIXED` is not atomic.** flinux does unmap-then-map (`mm.c:1217-1219`); we do
   `UnmapViewOfFile2` then `MapViewOfFile3`. Both leave a window where the address is unbacked. flinux has no
   better answer than we do, so this is confirmed as an unavoidable semantic loss rather than a gap in our plan.

One genuinely useful nuance: because each split placeholder becomes its own `VirtualQuery` region with its own
`AllocationBase` (measured, probe 2 test F), **the split boundaries are recoverable from the OS**. A shadow map
is a performance optimisation, not a correctness requirement — unlike flinux, whose `PAGE_NOACCESS` holes are
indistinguishable from live mappings under `VirtualQuery` and *must* be shadowed.

---

## 6. `MAP_SHARED`, `MAP_FIXED`, `MAP_NORESERVE`, `mremap`, `brk`

### `MAP_SHARED` — accepted, then quietly downgraded

`mmap_internal()` translates `MAP_SHARED` into `INTERNAL_MAP_SHARED` and forces `MAP_POPULATE` so the blocks are
allocated eagerly (`mm.c:1130-1136`), and `BLOCK_ALIGNED()` (`mm.h:59`) makes shared regions block-aligned so a
whole section can be shared. `MAP_FIXED` at a non-block-aligned address that would collide with a shared region
is rejected with `ENOMEM` (`mm.c:1158-1172`).

**But the flag is never stored.** At `mm.c:1231-1235` the new entry's flags are built from scratch and only
`INTERNAL_MAP_NORESET` and `INTERNAL_MAP_VIRTUALALLOC` are copied across; `INTERNAL_MAP_SHARED` is dropped.
Three separate places then read `e->flags & INTERNAL_MAP_SHARED` and always see zero:

- `find_free_pages_topdown()` (`mm.c:472`) — shared regions are not rounded up to whole blocks when placing
- `mm_get_maps()` (`mm.c:580`) — `/proc/self/maps` labels every mapping `p` (private)
- `mm_fork()` (`mm.c:1083`) — the "not shared, so make it copy-on-write" branch is taken for shared mappings too

The last one is the substantive consequence: **anonymous `MAP_SHARED` memory is copy-on-written across `fork()`
rather than shared.** I did not run flinux to confirm the runtime symptom, so this is a read of the code, not an
observation — but the read is unambiguous.

Separately, `MAP_SHARED` on a **file** is not shared with anything in any case: see the next item.

### File-backed mappings are *copies*, not mappings

`map_entry_range()` (`mm.c:601-616`) materialises a file-backed range by calling the VFS `pread` into the
memory and zero-filling the tail. There is no `NtCreateSection` on a file handle anywhere in `mm.c`. The
consequences are stated in flinux's own header comment (`mm.c:44-48`) and are all real:

- a shared file mapping is not coherent with any other process, or with `read`/`write` on the same file;
- `msync` is **`ENOSYS`** (`mm.c:1553-1558`), so dirty pages are never written back — a file mapping is
  effectively `MAP_PRIVATE` regardless of what the guest asked;
- `munmap` does not flush either (`free_map_entry_blocks` just releases).

This was forced on them: with only `MapViewOfFile`, a 4-KiB file offset is impossible (measured:
`ERROR_MAPPED_ALIGNMENT`). We are not forced, because `MapViewOfFile3` accepts a 4-KiB offset (measured).

### `MAP_FIXED` — supported, with two refusals

Supported at 4-KiB granularity for ordinary private/anonymous mappings, which is the whole point of the block
architecture. Refused in two cases (`mm.c:1146-1172`): a non-block-aligned address when the mapping needs a
whole block (`MAP_SHARED` or `INTERNAL_MAP_VIRTUALALLOC`, i.e. also `MAP_STACK`) → `ENOMEM`; and a
non-page-aligned address → `EINVAL`. `INTERNAL_MAP_NOOVERWRITE` (`mm.h:54`) provides `MAP_FIXED_NOREPLACE`
semantics internally but is not wired to a guest flag.

Worth recording as measured prior art for `repair_signal_page`: `VirtualAlloc(addr, …, MEM_RESERVE, …)` over an
already-committed range fails with `ERROR_INVALID_ADDRESS` (487) while a re-`MEM_COMMIT` of the same range
succeeds idempotently (probe 1 test 10). That is `MAP_FIXED_NOREPLACE` semantics exactly, as
`host-services-map.md` predicted — though the error code is 487, not 87.

### `MAP_NORESERVE` — silently ignored

The constant is defined (`common/mman.h:21`) and **never referenced in `mm.c`**. Nor are `MAP_GROWSDOWN`,
`MAP_LOCKED`, `MAP_DENYWRITE`, `MAP_32BIT` or `MAP_HUGETLB`. Only `MAP_ANONYMOUS`, `MAP_FIXED`, `MAP_SHARED`,
`MAP_PRIVATE`, `MAP_POPULATE` and `MAP_STACK` are inspected. Given §2.6, ignoring `MAP_NORESERVE` is the
expensive one: a guest asking explicitly *not* to be charged is charged 64 KiB per touched block anyway.

`madvise` is a near-total no-op that returns 0 and logs an error only for `MADV_DONTFORK` (`mm.c:1671-1678`).

### `mremap` — `ENOSYS`

`mm.c:1664-1669`. Logged and refused, no partial support. This is a meaningful gap: glibc's `realloc` and
several allocators use `mremap` for large blocks. Under the block architecture a moving `mremap` would mean
re-mapping every section in the range, and a non-moving grow would collide with whatever owns the next block.

### `brk` — a fixed base on x64, implemented on top of `mmap`

`sys_brk` (`mm.c:1680-1711`) grows by calling `mmap_internal(MAP_FIXED | MAP_ANONYMOUS | MAP_PRIVATE,
INTERNAL_MAP_NOOVERWRITE)` at the current break and shrinks by calling `munmap_internal()`. It is a thin shim
over the same machinery, which is the right structure.

The initial break is the interesting part. On 32-bit it follows the ELF image
(`mm_update_brk`, `mm.c:428-436`, called from `exec.c:238`). On **x64 it is unconditionally overwritten** with
`MM_BRK_BASE = 0x0000_0003_0000_0000` (12 GiB) (`mm.h:48`, `mm.c:431-433`), discarding the computed value. The
stated reason is a comment at `mm.c:430`: *"Seems glibc does not like unaligned initial brk"*. That is a
diagnosis-by-symptom, and it is worth knowing that a fixed, high, block-aligned initial break was what made
glibc work for them — it is the kind of detail that costs a week to rediscover.

---

## 7. Memory and fork: copy-on-write without kernel copy-on-write

*(Process creation is the fork agent's lane. This covers only the memory mechanism.)*

Windows has no `fork` and no kernel COW for private anonymous memory. flinux synthesises both from three
ingredients, and the whole thing turns on the fact that **each 64-KiB block is a separately-shareable kernel
object with an inheritable handle**.

**Ingredient 1 — inheritance.** Sections are created `OBJ_INHERIT` (`mm.c:673`), so the child process inherits a
handle to every block's section object. `mm_fork()` (`mm.c:965-1092`) copies the `mm_data` struct into the child
with `NtWriteVirtualMemory`, reserves a fresh 32-GiB shadow table in the child with `VirtualAllocEx`, and copies
only the *committed* table pages across (`mm.c:970-999`). The child ends up with the parent's map — tree and
handle table — and the same section objects.

**Ingredient 2 — write-protect the parent.** For each private mapping, `mm_fork()` drops `PROT_WRITE` in the
parent via `mm_change_protection()` (`mm.c:1083-1088`). Both processes now reference the same section objects
read-only. This is COW's write-fault trigger, built by hand.

**Ingredient 3 — detached views in the child, and lazy re-mapping.** This is the clever part, and flinux
explains its own reasoning at `mm.c:1000-1015`: mapping the views plus fixing protections measured *"about 8
msec for 50-60 sections (3-4M)"* on the author's machine, which is slower than `NtWriteVirtualMemory` of the
same data, and in the common `fork`-then-`execve` case that work is thrown away immediately. So `mm_fork()`
**maps nothing**. The child's section handles are present but **detached** — in the table, no view. The first
access to a detached block faults, and `load_detached_block()` (`mm.c:837-855`) maps the view then and there.

This is the "lazily-VEH-faulted unmapped views" the prior-art survey flagged, and the 8 ms figure is flinux's
own measurement, not mine.

**The COW fault itself** — `handle_cow_page_fault()` (`mm.c:857-892`):

1. Find the `map_entry`; if the guest protection has no `PROT_WRITE`, this is a genuine guest fault → fail.
2. `take_block_ownership(block)` (`mm.c:764-796`): ask the kernel whether anyone else holds this section, using
   **`NtQueryObject(ObjectBasicInformation)` and testing `HandleCount == 1`**. If we are the only holder,
   nothing to do.
3. Otherwise `duplicate_section(block)` (`mm.c:706-762`): unmap the shared view from the block address; re-map
   the *old* section somewhere else as `PAGE_READWRITE`; `allocate_block()` a brand-new private section at the
   block address; `CopyMemory` 64 KiB; unmap and close the old one. The comment at `mm.c:723-727` explains that
   the re-map-elsewhere step is not just to free the address but to get a *writable* view of the source — which
   §4 confirms is necessary.
4. Re-apply per-page protections from the `map_entry` tree (`load_block_protection`, `mm.c:805-834`).

**I measured the `HandleCount` oracle** (probe 2 test E), because it is the linchpin and it is undocumented for
this use:

```
  fresh section          HandleCount=1
  after DuplicateHandle  HandleCount=2
  after MapViewOfFile    HandleCount=2   (mapping a view does not add a handle)
  after CloseHandle(dup) HandleCount=1
```

It works, and it behaves exactly as flinux needs: `HandleCount` tracks *handles*, not views, so a parent and
child each holding one inherited handle read as 2, and the survivor of a fork reads back as 1 once the other
side exits. `PointerCount` is not usable — it moved unpredictably (32769 → 32767 → 32765) across the same
operations.

**The COW granularity is 64 KiB.** A one-byte write in a forked child copies a whole block. Against Linux's
4-KiB COW that is 16× write amplification, matching §2.5.

**One escape hatch that matters.** Guest stacks are *not* section-backed. `MAP_STACK` forces
`INTERNAL_MAP_VIRTUALALLOC` (`mm.c:1137-1143`) with the comment: *"Windows shows strange behaviour when the
stack is on a shared section object … it sometimes crashes when returning from a blocking system call."*
Those regions are eagerly copied into the child with `NtWriteVirtualMemory` (`mm.c:1023-1081`) instead of being
COW'd. I did **not** reproduce this, and the comment is a symptom report rather than a root cause — but if we
ever back guest thread stacks with section views, this is the first thing to suspect.

---

## 8. Address-space layout

### 8.1 Yes — and one region is pinned by relaunching the process

flinux does not have a `memory_layout.h` in Cygwin's style, but it pins by constant in three places:

| Region | Address | Source |
|---|---|---|
| Guest allocation window | `0x2_0000_0000` … `0x1_0000_0000_0000` | `mm.c:66-68` |
| Initial `brk` (x64) | `0x3_0000_0000` | `mm.h:48` |
| ET_EXEC image | `0x400000`, 256 MiB reserved | `fork.c:97-99` |
| Emulator statics (`mm_static_alloc`) | top-down, `MM_STATIC_ALLOC_SIZE` = 3 blocks | `mm.h:100`, `mm.c:353-355` |
| dbt code cache / block table | 8 MiB each, top-down `VirtualAlloc` | `dbt/x86.c:480-481`, `765-769` |

The ET_EXEC case is the interesting one and is directly relevant to us. A statically-linked Linux binary wants
`0x400000`, and on Win64 that address is inside the range where Windows may already have put a DLL or the CRT
heap by the time `main()` runs. flinux's solution (`fork.c:88-133`) is a three-way branch executed before
anything else:

- the range is `MEM_FREE` → `VirtualAlloc(MEM_RESERVE)` it immediately and continue;
- the range is already `MEM_RESERVE` of exactly 256 MiB → we are the relaunched child, continue;
- otherwise → **`CreateProcessW` a suspended copy of ourselves, `VirtualAllocEx` the range in the child before
  it runs a single instruction, `ResumeThread`, and exit.**

The reservation is later released just before the segments are mapped (`exec.c:200-204`). This is a clean,
API-independent answer to "a fixed guest load address may already be occupied", and it does not depend on any
Windows version. Note the ordering discipline: the reservation happens in `fork_init()`, which `main()` calls
**before `mm_init()`** (`main.c:112-117`) — the earliest point at which flinux's own code runs.

### 8.2 ASLR

flinux **disables ASLR for its own image**: `<RandomizedBaseAddress>false</RandomizedBaseAddress>` in all four
configurations of `flinux.vcxproj` (lines 241, 262, 289, 315). It does not disable system-wide ASLR and cannot —
`ntdll`, `kernel32` and friends still land at randomised addresses, which is precisely why the §8.1 relaunch
trick is needed rather than just a `/BASE` link option.

Note the interaction with the design in §3.3: because the guest allocation window is *not* reserved, ASLR of
system DLLs can in principle drop a module inside `[0x2_0000_0000, …)`. In practice Windows loads 64-bit system
DLLs high, so the low window is usually clear — but nothing enforces it and the failure mode would be an
`NtMapViewOfSection` failure at an arbitrary later time.

### 8.3 The guest-pointer validation trick (adjacent, worth recording)

`mm_check_read` / `mm_check_write` (`mm.h:73-75`) are **not** map lookups. They are hand-written assembly
(`stubs64.asm:143-232`) that simply *touches* one byte per page across the range, bracketed by exported labels
`mm_check_read_begin` / `_end` / `_fail`. The vectored handler (`syscall.c:66-84`) checks whether the faulting
IP lies between the begin/end labels and, if so, rewrites `Rip` to the `_fail` label and continues execution.

This makes pointer validation *and* on-demand materialisation the same operation — which is why `exec.c:235`
calls `mm_check_write` purely to force the pages in before a `pread` into them. It is a neat pattern, and the
engine already has a fault handler for `repair_signal_page`, but the ownership is the signals/dbt agent's, so it
is recorded here only as context for why flinux never needed an explicit "populate" call.

### 8.4 Resolved: where the placeholder entry points live

`host-services-map.md` §4 flags an unresolved linkage question (mingw-w64 issue #27) and asks for it to be
settled in the first hour of implementation. Measured (probe 2 test A) on this machine:

| Module | `VirtualAlloc2` | `MapViewOfFile3` | `UnmapViewOfFile2` |
|---|---|---|---|
| `kernel32.dll` | **absent** | **absent** | **absent** |
| `KernelBase.dll` | present | present | present |
| `api-ms-win-core-memory-l1-1-6.dll` | present (resolves to `KernelBase`) | present | present |

So the documented "Kernel32.dll" home is wrong in practice: these are `KernelBase.dll` exports, reached through
the `api-ms-win-core-memory-l1-1-*` API sets. `GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "VirtualAlloc2")`
returns NULL. **The `GetProcAddress` fallback that `host-services-map.md` describes as a contingency is in fact
the primary path, and it must target `KernelBase.dll` or the API-set name, not `kernel32.dll`.** Every
placeholder measurement in this document was taken through exactly that resolution and all of them worked, so
the mechanism is proven end-to-end on the pinned toolchain — no import library required.

---

## 9. Measurements

All on Windows 11 Pro 10.0.26200.8246, x86-64; clang 22.1.8, `x86_64-w64-windows-gnu`, `-lmincore`.
Single run each; timings are indicative, pass/fail results are deterministic and were stable across runs.

**Granularity and rounding**

| Probe | Result |
|---|---|
| `dwPageSize` / `dwAllocationGranularity` | 4096 / 65536 |
| `lpMaximumApplicationAddress` | `0x00007FFFFFFEFFFF` |
| `VirtualAlloc` at a 4-KiB-aligned fixed address | **succeeds, silently rounded down 0x1000** |
| `MapViewOfFileEx` at a 4-KiB-aligned base | fails, `ERROR_MAPPED_ALIGNMENT` (1132) |
| `MapViewOfFile` with a 4-KiB offset | fails, `ERROR_MAPPED_ALIGNMENT` (1132) |
| `VirtualProtect` of one 4-KiB page inside a 64-KiB view | ok, RO / NOACCESS / EXECUTE_READ all ok |
| `VirtualProtect` spanning two adjacent views | fails, `ERROR_INVALID_ADDRESS` (487) |
| `VirtualProtect` on reserved-not-committed, and on free | fails, 487 both |
| `VirtualFree(MEM_RELEASE)` of 4 KiB inside a 64-KiB reservation | fails, 87 |
| `VirtualFree(MEM_DECOMMIT)` of the same 4 KiB | ok |
| `UnmapViewOfFile` at base+64 KiB of a 256-KiB view | **"succeeds" — destroys the entire view** |
| `MEM_RESERVE` over a committed range | fails, 487 (= `MAP_FIXED_NOREPLACE`) |
| re-`MEM_COMMIT` over a committed range | ok, idempotent |

**Section maximum protection** (probe 2 test D)

| Section max prot | View re-protect to `PAGE_EXECUTE_READ` |
|---|---|
| `PAGE_READWRITE` | fails, 87 |
| `PAGE_EXECUTE_READWRITE` | ok; RW↔RX↔NOACCESS↔RW all ok |
| view mapped `FILE_MAP_READ` → `PAGE_READWRITE` | fails, 87 |

**Placeholders** (probes 2–3)

| Probe | Result |
|---|---|
| Split a placeholder at 4 KiB | **ok** — three regions 0x1000 / 0x1000 / 0xfe000, distinct `AllocationBase` |
| `MapViewOfFile3`, 4-KiB base + 4-KiB offset + 4-KiB size, `MEM_REPLACE_PLACEHOLDER` | **ok**, exact address, writable |
| `UnmapViewOfFile2(MEM_PRESERVE_PLACEHOLDER)` | ok, region returns to `MEM_RESERVE` placeholder |
| Private 4-KiB commit into an exactly-4-KiB placeholder | **ok** |
| Private 4-KiB commit into a 64-KiB placeholder (partial) | fails, 487 |
| `MapViewOfFile3` 4 KiB into a 64-KiB placeholder (partial) | fails, 487 |
| Split a placeholder holding a live view | fails, 87; ok after `UnmapViewOfFile2` |
| Punch a 4-KiB hole in a **committed private** region | **ok** |
| `MEM_COALESCE_PLACEHOLDERS`, exact boundaries / crossing a third | ok / fails 487 |
| `MEM_ADDRESS_REQUIREMENTS.Alignment = 2 MiB` | ok, returned 2-MiB-aligned |
| 4096 × 4-KiB splits | 6.118 ms (1.49 µs each); 8192 resulting VA regions |
| 4096 × private 4-KiB commits into placeholders | 4.179 ms (1.02 µs each) |

**flinux-shaped costs** (probes 1, 3, `commit.c`)

| Probe | Result |
|---|---|
| 32-GiB `MEM_RESERVE\|MEM_TOP_DOWN` (their shadow table) | ok, 0.001 ms, 0 MiB commit charge |
| `CreateFileMapping` 64 KiB × 512 | 0.633 ms (1.24 µs each) |
| `MapViewOfFileEx` × 512 | 0.481 ms (0.94 µs each) |
| create + map, per block | **2.33 µs** |
| One `VirtualAlloc` of the same 32 MiB | 0.003 ms |
| 20 000 section objects (1.25 GiB) | 20.6 ms; process handle count 20 076 |
| 8000 `SEC_COMMIT` sections, **no views mapped** | commit charge **+503 MiB** |
| the same after mapping all 8000 views | +504 MiB |
| plain `MEM_RESERVE` of 500 MiB | **+0 MiB** |
| 2048 × 4-KiB `VirtualProtect` inside views | 2.304 ms (1.12 µs each) |

---

## 10. What we should take

Against `hl_host_memory_services` (`include/hl/host_services.h:188-223`) and the memory section of
`docs/windows/host-services-map.md`. "Reject because we have placeholders" is the common verdict, and it is
earned by measurement rather than assumed.

| # | flinux technique | Verdict | Reasoning |
|---|---|---|---|
| T1 | 64-KiB block = one pagefile section; block-indexed section table (`mm.c:668-703`, `171-211`) | **Reject** | Placeholders split at 4 KiB and `MapViewOfFile3` takes a 4-KiB base *and* offset — both measured. The entire reason for the block architecture is gone. Adopting it would cost a handle and 64 KiB of commit per 64 KiB of guest AS for no benefit. |
| T2 | Two-level sparse shadow table over 2³² blocks (`mm.c:86-90`) | **Reject** | Falls with T1. Our per-mapping state is keyed by `hl_host_handle`, not by address. |
| T3 | Authoritative software VM map independent of NT's (`mm.c:109-136`) | **Adapt** | We need a host-side split-boundary map for `unmap_range`/`map_file`, because `MEM_REPLACE_PLACEHOLDER` demands an *exact* whole placeholder (measured). Unlike flinux this is a cache, not the source of truth — split placeholders are visible to `VirtualQuery` with distinct `AllocationBase`. Do **not** copy the fixed 1024-entry array (`mm.c:57`); that ceiling is a defect. |
| T4 | Own free-address scan over a pinned window (`mm.c:439-489`, `66-68`) | **Adapt, narrowly** | `MEM_ADDRESS_REQUIREMENTS` (`LowestStartingAddress` / `HighestEndingAddress` / `Alignment`) does this in the kernel — measured working for 2-MiB alignment, and `host-services-map.md` already plans it for `reserve_code`. Take the *idea* of a bounded guest window, not the linear scan. |
| T5 | `PAGE_NOACCESS` as a stand-in for a hole on shared 64-KiB edges (`mm.c:305-320`) | **Reject** | We can punch real holes at 4 KiB, in committed private memory as well as views (measured). flinux's version never returns address space or commit charge. |
| T6 | On-demand block materialisation via VEH (`mm.c:894-929`) | **Reject** for `map_anonymous` | `MEM_COMMIT` is already lazily backed by NT; the fault handler buys nothing and costs a 64-KiB granule. **Revisit only** if `MAP_NORESERVE`/overcommit-dependent guests fail on Windows' eager commit charge — that is the one scenario where a demand-fault scheme earns its keep, and it should be a measured decision, not a preemptive one. |
| T7 | Section-per-block COW with `NtQueryObject(HandleCount)` as the sharing oracle (`mm.c:764-796`, `706-762`) | **Reject now, record as proven** | `process` is "blocked" for phase 1 and `repair_code_after_fork` is a stub. But the oracle **is measured working** (`HandleCount` counts handles, not views), and 64-KiB-granular manual COW is the only known way to get `fork` memory semantics on NT. If fork is ever revived, start here. |
| T8 | Detached views in the fork child, mapped lazily on fault (`mm.c:1000-1015`) | **Reject now, record as proven** | Same reason. flinux's own measurement — ~8 ms to map and protect 3–4 MiB of sections, worse than `NtWriteVirtualMemory` of the same bytes — is the strongest argument in the file for laziness, and it is their number, not mine. |
| T9 | Read file contents into anonymous memory instead of mapping the file (`mm.c:601-616`) | **Adapt, narrowly** | Reject as the general `map_file` strategy: it is what forced their `msync` to be `ENOSYS` and made every shared file mapping incoherent. **Adopt for exactly one case** — `host-services-map.md` §4 gap 4, mapping past EOF. Clamp the section to the file size and satisfy the tail from a separate anonymous placeholder, rather than letting `CreateFileMapping` extend the file. |
| T10 | Create every section with `PAGE_EXECUTE_READWRITE` maximum protection (`mm.c:683`) | **Adopt** | Measured: max protection is fixed at creation and a later promote to `PAGE_EXECUTE_READ` fails with 87 if it was not allowed. This is a direct constraint on `map_file` (choose the section's max protection from the file's granted access, and cache one section per (file, max-prot) pair) and on `reserve_code` (must be `PAGE_EXECUTE_READWRITE`). Also measured: a view mapped `FILE_MAP_READ` cannot be promoted to writable — the *view's* access matters too, not just the section's. |
| T11 | Never put a thread stack on a section view; use `VirtualAlloc` (`mm.c:1137-1143`) | **Adopt as a caution, unverified** | Their comment reports crashes returning from blocking syscalls when a stack lived on a shared section. Not reproduced here. Cheap to honour: back guest/host stacks with private `MEM_COMMIT`, not views. |
| T12 | Probe-and-fixup guest pointer validation via VEH IP rewrite (`stubs64.asm:143-232`, `syscall.c:66-84`) | **Adapt — other lane** | A good pattern and compatible with the `repair_signal_page` handler we already need, but ownership sits with the signals/dbt agent. Flagged here only so it is not lost. |
| T13 | Deferred re-entrant `munmap` via a thread-id guard and work list (`mm.c:213-232`, `1398-1416`) | **Adopt** | Directly applicable: `unmap_range`/`release` drop `hl_host_handle` ownership, whose release path can re-enter the memory lock. Their shape — detect self-re-entry, queue, drain at the top — is the right one. |
| T14 | Pre-reserve the fixed ET_EXEC load address, relaunching a suspended copy of the process if it is already taken (`fork.c:88-133`, released `exec.c:200-204`) | **Adopt if the collision is observed** | We will have the same problem: a Linux guest wanting `0x400000` inside a process that has already loaded `ntdll`/`KernelBase`. The reservation-at-earliest-entry half is free and should be done unconditionally. The relaunch half is heavyweight and only pays if a real collision is seen — measure before building it. |
| T15 | `SEC_COMMIT` for anonymous sections (`mm.c:683`) | **Reject explicitly** | Measured: +503 MiB commit charge for 500 MiB of sections *never mapped*. If we create pagefile-backed sections for `map_anonymous(SHARED)`, prefer `SEC_RESERVE`. This is a mistake worth naming so it is not repeated. |
| T16 | Ignoring `MAP_NORESERVE`, `mremap` = `ENOSYS`, `msync` = `ENOSYS` (`mm.c:1664`, `1553`) | **Reject** | Recorded as evidence of what an incomplete memory backend can still boot with — flinux ran real glibc programs with all three missing. Useful for sequencing (they are not day-one blockers), not as a target. |

### 10.1 Per-callback consequences

- **`reserve`** — nothing to take from flinux. Note only that Windows returns a 64-KiB-aligned base, which
  over-delivers on the contract, and that commit charge is eager (measured, §9).
- **`protect`** — T10 is the real finding: the section's *maximum* protection and the *view's* access both cap
  what `protect` can ever do, and neither can be raised later. Also confirmed: `VirtualProtect` cannot span two
  regions, so after placeholder splitting the backend must iterate — exactly as `host-services-map.md` says,
  and for exactly the reason flinux iterates per block.
- **`release`** — T13 (re-entrancy) applies.
- **`map_file`** — T9 for the past-EOF tail; T10 for section max protection. The 4-KiB-offset relaxation is
  **verified** (§9), which closes the "verify empirically in the first week" item in `host-services-map.md` §4
  gap 1.
- **`map_anonymous`** — T5/T6 rejected; T15 rejected. Private anonymous memory should be
  `VirtualAlloc2(MEM_RESERVE|MEM_COMMIT|MEM_REPLACE_PLACEHOLDER)` into an exactly-sized placeholder (measured
  working at 4 KiB).
- **`unmap_range`** — the whole of §5.4. Split-then-replace, exact boundaries, unmap-before-split.
- **`sync`** — flinux has nothing here (`ENOSYS`). No prior art; `host-services-map.md`'s
  `FlushViewOfFile` + `FlushFileBuffers` plan stands unchallenged.
- **`discard`** — pure bookkeeping in both designs. Nothing to take.
- **`reserve_code`** — T10 (`PAGE_EXECUTE_READWRITE` max protection) is mandatory, and
  `MEM_ADDRESS_REQUIREMENTS.Alignment` is measured working. flinux's own code cache is a plain 8 MiB
  `VirtualAlloc(PAGE_EXECUTE_READWRITE)` (`dbt/x86.c:768`) with no W^X discipline at all — not a model for us.
- **`repair_signal_page`** — one measured contribution: `VirtualAlloc(MEM_RESERVE)` over an occupied range fails
  (487) while re-`MEM_COMMIT` is idempotent, so the planned `VirtualProtect` → `VirtualAlloc` → `VirtualProtect`
  ladder does have `MAP_FIXED_NOREPLACE` semantics. The error code is 487, not 87.

---

## 11. Open questions and unknowns

1. **Not reproduced:** flinux's claim that a thread stack on a section view causes crashes returning from
   blocking syscalls (`mm.c:1139-1141`). Recorded as a caution only.
2. **Not reproduced:** the `MAP_SHARED` flag-loss defect in §6 is a code reading, not an observed failure.
   flinux was not built or run.
3. **Not measured:** whether Windows imposes a practical VAD-count ceiling. 8192 regions from 4096 splits was
   fine; a guest with hundreds of thousands of distinct 4-KiB mappings was not tested. This is the one place
   flinux's coarse 64-KiB granularity could conceivably win, and it should be measured before it matters.
4. **Not measured:** `MAP_PRIVATE` over a *file* section (`PAGE_WRITECOPY`) — carried over unchanged as an open
   item from `host-services-map.md` §4 gap 5. flinux never used file sections, so it offers no evidence.
5. **Not measured:** placeholder behaviour under memory pressure or after very long churn (fragmentation of the
   placeholder set, cost of coalescing back).
6. **Single machine.** Every number in §9 comes from one Windows 11 26200 host. The pass/fail results are API
   contracts and should be stable; the timings are not portable.
