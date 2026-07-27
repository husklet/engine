# The borrowed-descriptor lane in `src/linux_abi`

Scope and design for removing `hl_host_posix_attachment_services` from the guest-boot path, and for the
larger refactor behind it. **Nothing here has been implemented.** Every count and line reference was read
out of the tree at `feat/windows-amd64`; where a claim is inference rather than reading, it says so.

`DOCS.md` is normative. `docs/windows/host-services-map.md` §1.2 raised this; this file is the answer to it,
and it corrects that section in two places (§2.1 and §5 below).

---

## 1. The problem in one paragraph

`root_handle_bind` (`src/linux_abi/container/vfs.c:1455-1498`) pins the namespace root twice: once as an
opaque `hl_host_handle` via `file->open_relative` (`vfs.c:1468`), and once as a raw native descriptor via
`posix_attachment->borrow_file_at_least` (`vfs.c:1475`, retried at `:1477`). The second pin is stored in
`g_root_fd` (`vfs.c:1445`). A failure of either is `return -1`, and both target roots turn that into
unconditional init failure — `src/core/target/x86_64.c:801` (container) and `:812` (bare),
`src/core/target/aarch64.c:822` and `:834`. `hl_host_posix_attachment_services` is an explicitly optional
POSIX-host adapter (`include/hl/host_services.h:652-659`) that hands out a native file descriptor. Windows
cannot supply one. So on Windows the engine fails during `container_init`, before a single guest instruction
executes, for a bare guest as well as a container.

---

## 2. The measured extent

### 2.1 Correcting the site count

The host-services map cites "**61 direct call sites** of the POSIX `*at` family inside `src/linux_abi` and
`src/core`, 62 of them in `src/linux_abi/syscall/fs.c` alone". That table counted identifier *occurrences*,
which in this tree are dominated by comments and `switch` labels: `fs.c` mentions `openat` 86 times and calls
it 3 times; `src/linux_abi/sentry.c` scores 10 occurrences and **zero** calls; all 21 hits in
`syscall/nonpie_args.h` are `case 56: // openat(dfd, PATH, flags, mode)`-shaped labels.

Stripping `//` comments and requiring the identifier be immediately followed by `(`:

| Scope | `*at`-family call sites |
|---|---|
| `src/linux_abi` | **64** |
| `src/core` | **2** (`target/aarch64.c:294`, `activation.c:603`) |
| `src/host` (legitimate — this *is* the backend) | 36 (`linux/host.c` 19, `macos/host.c` 13, `resolve.c` 4) |

So **66 sites outside `src/host/`**, not 61, and the distribution across files is different enough from the
map's table that the map's per-file numbers should not be used for planning.

That count is also the *narrowest* of three concentric surfaces, and conflating them is the main way this
work gets mis-sized:

- **Surface 1 — the borrowed root.** Sites that read `g_root_fd` or a volume's borrowed fd: the eight in
  `container/vfs/resolve.c` plus their six consumers listed in §2.3 Group N/B. This is what
  `posix_attachment` actually feeds.
- **Surface 2 — the `*at` family.** 66 sites. Most take a parent fd that the *walk* produced, not the
  borrowed root.
- **Surface 3 — ambient POSIX on descriptors generally.** `read`/`write`/`close`/`fcntl`/`dup`/`ioctl`/
  `getdents64`/`fstat`/`lseek`/`mmap` on numbers that are simultaneously guest descriptors and host
  descriptors. A crude count (comment-stripped, member accesses excluded) puts this near **900** in
  `src/linux_abi`, led by `checkpoint.c` (~249), `container/netns.c` (~169), `syscall/fs.c` (~95),
  `syscall/io.c` (~91). This number is indicative only — the regex has false positives — but the order of
  magnitude is right and it is the real Windows cost. **Surface 3 is out of scope for this document** and
  is called out in §7 so it is not discovered late.

### 2.2 The descriptor model these sites assume

`ATFD(value)` is `#define ATFD(value) (((int)(value) == -100) ? AT_FDCWD : (int)(value))`
(`src/linux_abi/syscall/dispatch.c:32`). A guest descriptor number is passed to the host kernel unchanged.
`close(2)` at `fs.c:3588-3595` closes the raw guest number. **In this engine a guest fd number *is* a host
fd number**, except where the typed lane intervenes (§4.2). Every plan in this document has to say what it
does about that; the ones that do not are wrong.

### 2.3 Grouping by what each site needs

Every site below was read. Groups R, L and N consume a descriptor produced by the confined walk or by the
borrowed root; group A consumes `AT_FDCWD` or a guest descriptor directly.

**Group R — the confined per-component walk** (8 sites, all in `src/linux_abi/container/vfs/resolve.c`)

`resolve_at` (`resolve.c:89-310`). Needs: open-directory-beneath, lstat-a-name, read-a-link.

| Site | Call | Role |
|---|---|---|
| `resolve.c:117` | `readlinkat(root_fd, …)` | single-file bind that is a symlink |
| `resolve.c:141` | `openat(root_fd, ".", O_DIRECTORY)` | hand back a single-file bind's parent |
| `resolve.c:180` | `fstatat(d, fcomp, AT_SYMLINK_NOFOLLOW)` | dentry-cache fast path: is the leaf a link? |
| `resolve.c:192` | `openat(root_fd, ".", O_DIRECTORY)` | seed the walk stack |
| `resolve.c:249` | `fstatat(fds[nf-1], comp, AT_SYMLINK_NOFOLLOW)` | per-component symlink test |
| `resolve.c:256` | `readlinkat(fds[nf-1], comp, …)` | per-component link splice |
| `resolve.c:289` | `openat(fds[nf-1], comp, O_DIRECTORY\|O_NOFOLLOW)` | descend |
| `resolve.c:304` | `openat(fds[nf-1], ".", O_DIRECTORY)` | return the confined parent |

**Group L — leaf operation on the parent the walk returned** (18 sites, `syscall/fs.c`)

These run inside `if (jail_routed_at(...))` branches on the `pfd` / `dfd` / `opfd` / `npfd` that
`jail_at`/`jail_open_plan` produced.

`fs.c:1532` `mknodat` · `:1587` `mkdirat` · `:1738` `fstatat` · `:1758` `unlinkat` · `:1834` `symlinkat` ·
`:1896` `linkat(…,"",…,AT_EMPTY_PATH)` · `:1903` `openat` · `:1930` `unlinkat` · `:1987` `linkat` ·
`:1991` `fstatat` · `:2085` `renameatx_np` · `:2715` `fstatat` · `:2777` `fstatat` · `:2945` `openat` ·
`:2948` `unlinkat` · `:3474` `faccessat` · `:3526` `openat` · `:4384` `utimensat`

**Group A — ambient, on `AT_FDCWD` or a guest descriptor** (24 sites)

The "no jail" fall-through. `atpath` (`container/route.c:36-66`) returns the guest path **untouched** when
`g_rootfs` is NULL, so these operate on the real host root.

`fs.c:1548` `mknodat` · `:1604` `mkdirat` · `:1781` `fstatat` · `:1788` `unlinkat` · `:1841` `symlinkat` ·
`:2002` `linkat` · `:2005` `fstatat` · `:2054` `renameatx_np(AT_FDCWD,…)` · `:2106` `renameatx_np(ATFD,…)` ·
`:2735` `fstatat` · `:2803` `fstatat` · `:3568` `faccessat` · `:3572` `openat` · `:4066` `readlinkat` ·
`:4268` `fstatat` · `:4290` `fstatat` · `:4397` `utimensat` · `:4531` `fstatat` · `:4540` `fstatat` ·
`:4635` `fstatat` · `:4741` `fstatat` · `:4776` `faccessat` · `:4783` `faccessat` ·
`syscall/proc.c:1940` `fstatat`

**Group D — directory enumeration** (2): `fs.c:3683` `fdopendir(dup(fd))`, `vfs.c:3211` `fdopendir(scan)`.

**Group F — whole-descriptor filesystem geometry** (1): `fs.c:2212` `fstatfs`.

**Group N — engine-side namespace maintenance, not guest syscalls** (10):
`container/state.c:462,464,467,470,479` (the permission-bit transaction, `mode_transaction_path`) ·
`vfs.c:2000` (`xresolve_exec` for a bind volume under a bare launch) · `vfs.c:3220,3224,3228` (recursive
teardown of a synthesized `/proc/<pid>/fd` directory) · `vfs/overlay.c:457` (`utimensat` after copy-up).

**Group B — boot and loader** (3): `src/linux_abi/x86.c:78` and `src/core/target/aarch64.c:294` (the guest
ELF open, one per target root) · `src/core/activation.c:603` (`unlinkat(dirfd(entries), …)`).

8 + 18 + 24 + 2 + 1 + 10 + 3 = **66**.

---

## 3. What the typed contract already covers

`hl_host_file_services` is at `include/hl/host_services.h:344-426`, ABI 23. The policy set is
`HL_HOST_RESOLVE_NOFOLLOW_FINAL | NO_SYMLINKS | ALLOW_MISSING` (`:86-88`); the resolution result is
`hl_host_file_resolution { parent, target, target_type, final[256] }` (`:296-303`).

| Need | Sites | Verdict |
|---|---|---|
| Resolve a path beneath a pinned root, returning parent + leaf | R (8) | **Covered** — `resolve_beneath` (`:387`). Semantics differ on `..`; see §6.1. |
| Open beneath, atomically, no final symlink | L: 3526, 2945, 1903 | **Covered** — `open_beneath` (`:396`). |
| Metadata of a leaf | L: 1738, 1991, 2715, 2777; A: 1781, 2005, 2735, 2803, 4268, 4290, 4531, 4540, 4635, 4741; proc.c:1940 | **Covered** by `resolve_beneath` + `metadata` (`:356`) — two calls where POSIX needs one. |
| Read a symlink | R: 117, 256; A: 4066 | **Covered** — `open_beneath(PATH_ONLY\|NOFOLLOW)` + `readlink` (`:383`). Three calls where POSIX needs one. |
| Create directory / symlink / hard link / FIFO relative to a handle | L: 1587, 1834, 1987; A: 1604, 1841, 2002 | **Covered** — `make_directory`/`make_symlink`/`make_link`/`make_fifo` (`:408-416`). |
| Unlink / rmdir relative to a handle | L: 1758, 1930, 2948; A: 1788 | **Covered** — `unlink_relative` (`:377`), `remove_directory` (`:425`). |
| Rename relative, atomic replace | L: 2085; A: 2054, 2106 | **Partly.** `rename_relative` (`:374`) has no flags word. `RENAME_NOREPLACE` / `RENAME_EXCHANGE` / `RENAME_WHITEOUT` (`fs.c:2012-2110`) cannot be expressed. **Appended callback needed.** |
| Directory enumeration | D (2) | **Covered** — `read_directory` (`:405`). Cursor model differs; see §6.3. |
| Filesystem geometry | F (1) | **Covered** — `filesystem_metadata` (`:399`). |
| Times / permissions / owner on an **open handle** | — | Covered — `set_times`/`set_permissions`/`set_owner` (`:401-403`, `:385`). |
| Times / permissions / owner on a **name, not following the final symlink** | L: 4384; A: 4397; N: state.c:464,467,479, overlay.c:457 | **Genuine gap.** You cannot open a symlink for these operations. Linux itself needs `AT_SYMLINK_NOFOLLOW` on the *at form. **Appended callbacks needed** (`set_times_relative`, `set_permissions_relative`), or accept that `lutimes`-shaped guest calls fail. |
| Device node creation (`mknodat`) | L: 1532; A: 1548 | **Not expressible.** No `make_device` in the group; `make_fifo` covers only S_IFIFO. **Appended callback needed** for container work. |
| Access check (`faccessat`) | L: 3474; A: 3568, 4776, 4783 | **Should not be a host callback.** DOCS.md §3.4 assigns virtual ownership to the Linux front; the check is derivable from `metadata` + the `owner.h` table. Migrate as Linux-front logic, not a new callback. |
| `linkat(..., "", ..., AT_EMPTY_PATH)` — name an `O_TMPFILE` inode | L: 1896 | **Not expressible.** `make_link` takes two paths. Either an appended `link_handle` or keep the existing fallback at `fs.c:1903-1930` (open+copy+rename), which is already there for hosts without the empty-path form. |
| Extended attributes | N: state.c (via `mode_xattr_*`) | **Not in the contract at all.** `src/linux_abi/xattr.c` exists but there is no host xattr group. Container permission virtualization depends on it. Out of scope here; record it. |

So: of the 66 sites, **50 need nothing new**, **13 need one of four appended callbacks**
(`rename_relative_flags`, `set_times_relative`, `set_permissions_relative`, `make_device`), **3 are
Linux-front logic misplaced as host calls** (`faccessat`), and **1 has no good answer**
(`linkat`/`AT_EMPTY_PATH`) but already has a working fallback beside it.

Appending is legal and is the documented procedure — DOCS.md §11 "Add a host-service operation" step 2,
plus §3.5 "Optional appended callbacks are detected using the group size." `hl_valid_file_group` in
`src/core/host_services.c:57` checks `size >= sizeof(...)`, so the ABI number and the prefix are preserved.

---

## 4. This is already half-done, twice

### 4.1 The precedent: `hl_persist_directory`

DOCS.md §12's checked item ("Persistent-cache storage is fully routed through a pinned typed File-service
directory") is `src/translator/persist.c`, 127 lines, and it is the template:

- `hl_persist_directory_open` (`persist.c:21-42`) pins one handle with
  `open_relative(HL_HOST_HANDLE_CWD, path, READ|DIRECTORY|NOFOLLOW)`, then gates it on
  `validate_private_directory`. The pinned handle is the *only* authority afterwards.
- Every subsequent operation is relative to that handle and names a **single leaf** —
  `hl_persist_leaf` (`:11-19`) rejects any name containing `/`, `.` or `..`.
- Reads go `open_relative` → `validate_private_regular` → `metadata` → `read_at` loop → `close`
  (`:52-96`). Writes go through one call, `store_private_atomic` (`:104-107`).
- There is no descriptor anywhere in the file.

`src/linux_abi/image.c:40-56` is the same shape for the guest ELF image, and `fdcache.c:709-745`
(`fsgen_bind`) is the same shape for the shared filesystem-generation page — `open_relative` +
`memory->map_file`, no fd. This is why a **bare** guest's ELF load already works without any native
descriptor (§5).

### 4.2 The half-built version: `jail_open_plan`

`src/linux_abi/container/vfs/resolve.c:363-509` already runs the typed resolver. It is worth reading in
full before designing anything, because it contains the answer to "can these two lanes coexist" — and the
answer it gives is *no*, in the specific way it fails to:

- `:412` calls `jail_at` unconditionally and keeps `native_parent` as the function's return value. Every
  caller's `pfd` is still a native fd.
- `:418-507` then *additionally* runs `resolve_beneath` and, under a narrow set of conditions
  (`:454-457`: regular file, or directory with `O_DIRECTORY`, or missing-with-`O_CREAT`), also
  `open_beneath` — and the results are discarded when those conditions do not hold (`:486-488` closes
  both handles).
- `:414-417` documents the one real semantic divergence in a comment: the typed resolver is clamped to a
  single jail, so a `..` that crosses a bind-mount boundary would give the wrong answer, and the branch is
  therefore guarded on `!path_has_dotdot(absolute)`.

So today Linux and macOS execute **both** lanes on every jailed open, and the typed one is decoration
except where it wins. That is the cost of the dual-lane design, already paid, already measurable.

The consumer side is likewise built: `bound_handle_reserve` (`syscall/binding.c:1157-1183`) reserves a
typed slot in `g_linux_box` (the real `hl_linux_abi` descriptor/OFD table, `src/linux_abi/linux_abi.c`)
*paired with* a shadow native descriptor from `bound_shadow_reserve` (`binding.c:986-1005`,
`fcntl(F_DUPFD_CLOEXEC)`), and `bound_adopt_handle` (`:1193-1215`) commits the opaque handle into it.
The shadow exists purely to make the guest's fd *number* un-claimable by ambient host code — i.e. it is a
tax paid by Surface 3.

`src/host/resolve.c:106-260` (`hl_host_resolve_beneath`) is the POSIX implementation of `resolve_beneath`
shared by both backends (`linux/host.c:1908`, `macos/host.c:2396`). Note that `hl_host_resolved_path` with
its `int parent_fd/target_fd` lives in `src/host/resolve.h` — it is **host-internal**, not a public contract
type. The host-services map's remark that it "must gain a Windows shape" is unnecessary: a Windows backend
simply does not link it.

---

## 5. The boot-critical subset

This is the milestone-versus-mountain question, so here is the trace rather than a summary.

### 5.1 A bare guest never touches the confined walk

`jail_routed_at` (`src/linux_abi/syscall/fs.c:90-97`):

```c
static int jail_routed_at(int dirfd, const char *path) {
    (void)dirfd;
    if (g_rootfs) return 1;
    if (!path || path[0] != '/') return 0;
    char normalized[4200];
    confine(path, normalized, sizeof normalized);
    return jail_match(normalized) >= 0;
}
```

For a bare launch `container_init` never assigns `g_rootfs` (`x86_64.c:800-812`: the assignment is inside
`if (rootfs && rootfs[0])`), and with no `HL_VOLUMES` entries `jail_match` (`vfs.c:1704-1720`) iterates zero
volumes and returns −1. So `jail_routed_at` is **identically 0**, every `if (jail_routed_at(...))` branch in
`fs.c` is dead, and with it all of Group L and all of Group R.

`g_root_fd`'s remaining readers — `resolve.c:67`, `vfs.c:1755` (both inside `jail_pick*`),
`syscall/dispatch.c:280`, `vfs/overlay.c:355,391`, `syscall/proc.c:145`, `syscall/io.c:309,322` — are either
unreachable without jail routing or are equality tests and relocation guards that already handle a negative
value. Checked: `engine_fd_reloc` (`io.c:181-191`) opens with `if (!slot || *slot != newfd || newfd < 0)
return;`, so a `-1` slot is inert; `engine_fd_vacate_range` (`io.c:322-324`) filters on `fds[i] >= 0`;
`proc.c:145` is `fd == g_root_fd` against a non-negative guest fd; and `resolve.c:107` has an explicit
`root_fd < 0 && !g_rootfs` fallback that opens `/` directly.

### 5.2 What a bare guest's init actually requires

1. `root_handle_bind("/")` (`x86_64.c:812`, `aarch64.c:834`) — `file->open_relative`, `file->path`, and
   `posix_attachment->borrow_file_at_least`. Only the last is unimplementable on Windows.
2. `hl_owner_seed("/", NULL, NULL, 0)` — `container/owner.h:185-224`. With `spec == NULL` it calls
   `hl_owner_reset` and returns at `:190`. No I/O.
3. The ELF load — `src/linux_abi/x86.c:70-84`. The jailed branch is guarded on
   `g_rootfs != NULL || jail_match(request) >= 0`, false for a bare launch, so control reaches
   `hl_linux_image_read(effective_host_services(), request, image)` → `image.c:43`, typed `open_relative`.
   **Already portable.**

So the boot gate is exactly one call: `vfs.c:1475`.

### 5.3 The milestone ladder

| | What runs | What it needs | Size |
|---|---|---|---|
| **M0** | `container_init` completes | Make `posix_attachment` optional in `root_handle_bind`: pin `g_root_handle` always, set `g_root_fd` only when the group is advertised. | ~15 lines, `vfs.c:1455-1498` |
| **M1** | A **statically linked** bare guest (`/bin/true`-shaped) runs to `exit_group` | M0, plus `write`/`brk`/`arch_prctl`/`set_tid_address`/`exit_group`. **No Group A site is executed** — a static binary opens no path. | 0 additional in this lane |
| **M2** | A **dynamically linked** bare guest runs | M1, plus the path syscalls `ld.so` issues: `openat`, `fstatat`, `readlinkat`, `faccessat`, `access`. That is `fs.c:3572, 3568, 4066, 4268, 4290, 4531, 4540, 4776, 4783` — **9 of the 24 Group A sites**. All take an absolute guest path and can be routed through the already-pinned `g_root_handle` with `open_beneath`/`resolve_beneath`, which is what container mode does. | ~350 lines |
| **M3** | A container / rootfs guest | Groups R (8), L (18), N (10), D (2), F (1), the remaining 15 of A, the four appended callbacks, and the xattr gap. | ~2500 lines + backend work |

**M1 is the honest first Windows milestone and it is one fifteen-line change away in this lane.** M2 is a
week. M3 is the mountain, and it is container work, not boot work.

The corrective to `host-services-map.md` §1.2 is therefore: `root_handle_bind` is a **gate**, not a
functional dependency, for bare guests. The claim "a perfect Windows `hl_host_services` still will not boot
a guest until `g_root_fd` and the `openat`-from-`dirfd` resolver lane are routed through
`file.resolve_beneath`" is true of a *container*; for a bare guest the resolver lane is never entered and
what blocks the boot is the gate plus Group A, which is a different set of sites with a different fix.

---

## 6. Design

### 6.1 One lane, not two

The tempting shape is `#if HL_HOST_POSIX` keeping `openat` and Windows taking the typed path. **That is a
trap, and `jail_open_plan` is the evidence.** A dual lane means the typed path is exercised in production
only on the host that has no reference implementation to differ against, so every divergence is discovered
by a Windows user rather than by the compat corpus. It also doubles the resolution work on the hosts that
*are* gated (§4.2), which is a live cost today.

The correct place for the fast native path is **inside the backend**: `hl_linux_file_resolve_beneath`
(`linux/host.c:1908`) already implements `resolve_beneath` in terms of `openat`/`fstatat`/`readlinkat` via
`src/host/resolve.c`. A Linux host loses nothing by having `src/linux_abi` call `resolve_beneath` instead of
open-coding the same walk — the same syscalls are issued, one function call deeper. What it gains is that
the corpus runs the same `src/linux_abi` code on all three hosts.

Recommendation: **single typed lane.** Delete `jail_at`'s native return value rather than adding an arm to it.

### 6.2 The one semantic that does not transfer, and what to do about it

`resolve_at` deliberately crosses bind-mount boundaries on `..` (`resolve.c:218-243`, the `xings` counter
and the `goto restart`): a `..` at a volume's own root re-resolves against the *parent* namespace, which is
Linux mount semantics. `hl_host_resolve_beneath` clamps instead (`src/host/resolve.c:158-165`:
`if (stack.count > 1) close(...)`, otherwise the component is dropped), which is `RESOLVE_IN_ROOT`
semantics. These are different answers for `/data/..`, and `jail_open_plan:414-417` already refuses to use
the typed resolver on any path containing `..` for exactly this reason.

Two options, and the choice should be made explicitly rather than drifted into:

- **(a) Keep the routing above the host seam.** `src/linux_abi` decomposes a `..`-crossing path into a
  sequence of single-jail `resolve_beneath` calls, one per boundary crossing, replicating the `goto restart`
  loop with handles instead of fds. The clamp stays a host-side guarantee; the crossing stays a Linux-ABI
  decision. This is correct by construction and matches DOCS.md §3.4 ("The Linux front is the sole owner of
  Linux behavior") and §4 (confinement must not be weakened).
- **(b) Add `HL_HOST_RESOLVE_ALLOW_ESCAPE`.** Cheaper, and wrong: it makes a confinement primitive
  configurable in the escaping direction, which is the one property `resolve_beneath` exists to guarantee.

Take (a).

### 6.3 The appended callbacks

Signatures, following the group's existing conventions (byte spans without a trailing NUL, `hl_host_result`,
appended at the end of the struct, `HL_HOST_FILE_ABI` bumped to 24):

```c
/* renameat2 flags. Hosts without the flag return HL_STATUS_NOT_SUPPORTED without side effects. */
enum {
    HL_HOST_RENAME_NO_REPLACE = 1u << 0,
    HL_HOST_RENAME_EXCHANGE   = 1u << 1,
    HL_HOST_RENAME_WHITEOUT   = 1u << 2
};
hl_host_result (*rename_relative_flags)(void *context,
                                        hl_host_handle old_directory, const char *old_path, size_t old_path_size,
                                        hl_host_handle new_directory, const char *new_path, size_t new_path_size,
                                        uint32_t flags);

/* Times/permissions on a name beneath a directory handle, never following the final symlink.
   These exist because a symlink cannot be opened for modification on any host. */
hl_host_result (*set_times_relative)(void *context, hl_host_handle directory, const char *path, size_t path_size,
                                     const hl_host_file_time times[2]);
hl_host_result (*set_permissions_relative)(void *context, hl_host_handle directory, const char *path,
                                           size_t path_size, uint32_t permissions);

/* Device nodes. type is HL_HOST_FILE_TYPE_CHARACTER or _BLOCK; device is the packed native dev_t
   equivalent as reported by metadata.device. Hosts without device nodes return NOT_SUPPORTED. */
hl_host_result (*make_device)(void *context, hl_host_handle directory, const char *path, size_t path_size,
                              uint32_t type, uint32_t permissions, uint64_t device);
```

`rename_relative_flags` maps to `renameat2` on Linux, `renameatx_np` on macOS (both already reached through
`src/host/native_compat.h:583`), and `FileRenameInfoEx` on Windows. `make_device` returns
`HL_STATUS_NOT_SUPPORTED` on Windows and on macOS-without-privilege, which is what a guest `mknod` gets
today anyway. Per DOCS.md §11 step 4, all four need Linux, macOS **and** fake implementations, or an
explicit omission — `src/host/fake/host.c:1297-1300` already carries `resolve_beneath`/`open_beneath` stubs
and is the right place to model the `NOT_SUPPORTED` responses.

`faccessat` gets no callback. `fs.c:3474` in particular is only asking "did this name exist before I
created it?", which `resolve_beneath(ALLOW_MISSING)` answers from `resolution.target ==
HL_HOST_HANDLE_INVALID` at no cost.

### 6.4 How sites migrate

The mechanical transformation, once per site, in the shape `jail_open_plan` half-implements:

```
    parent_fd = jail_at(dirfd, raw, final, sizeof final, nofollow);   /* native */
    r = <op>at(parent_fd, final, ...);
    close(parent_fd);
->
    hl_host_file_resolution res;
    rc = jail_resolve(dirfd, raw, policy, &res);                      /* typed; §6.2(a) inside */
    r  = file-><typed op>(ctx, res.parent, res.final, res.final_size, ...);
    jail_resolution_close(&res);
```

`jail_resolve` is the new `jail_at` — same name resolution, same overlay `overlay_mkparents` call
(`resolve.c:334`), same chroot re-rooting, returning `hl_host_file_resolution` instead of an `int`. It is the
one new function; everything else is call-site rewriting.

The two caches must move with it, or the perf gates fail:

- The dentry fast path (`resolve.c:158-188`) keys on `g_rootfs_canon + normalized`, calls
  `hl_fdcache_dentry_lookup` (`fdcache.c:508`), and then `open(dcanon)` by absolute path. Under the typed
  lane that becomes `open_relative(g_root_handle, dcanon_relative, …)`. The key is a host path string and
  stays valid.
- The openat memo (`fs.c:3428-3446`, `hl_fdcache_open_lookup` at `fdcache.c:635`) stores the canonical host
  path from `hl_native_fd_path` (`F_GETPATH` on macOS, `/proc/self/fd` on Linux —
  `src/host/native_compat.h:34,498`) and replays it with a single `open()`. The typed equivalent is
  `file->path` (`host_services.h:379`) plus `open_beneath(g_root_handle, …)`. **`file->path` on Windows
  returns a `C:\…` string** (host-services-map §8.1), so anything that compares a memo key against
  `g_rootfs_canon` needs to be path-syntax-neutral. This is the most likely place for a silent Windows-only
  defect and it should get its own test.

`read_directory` replaces `fdopendir` at `fs.c:3683` and `vfs.c:3211`. The `g_dirs[64]` `DIR*` table
(`fs.c:3676-3695`) becomes a per-OFD cursor, which is what DOCS.md §3.4 already specifies ("Directory
enumeration uses a shared logical cursor. Host cookies are local implementation details"). The map's note
that Windows has no `d_off` cookie (§8.3, `read_directory`) applies here: `getdents64`'s `d_off` is
currently passed through, and any guest using `seekdir` to a previously returned offset will regress on
Windows only.

### 6.5 Ordering that keeps the tree green

Each step compiles and passes on Linux and macOS on its own.

1. **Make `posix_attachment` optional in `root_handle_bind`.** `g_root_handle` is pinned unconditionally;
   `g_root_fd` becomes `-1` when the group is absent. Verify `engine_fd_reloc`, `engine_fd_vacate_range`
   (`io.c:306-334`) and `proc.c:145` are `-1`-safe. Zero behaviour change on Linux/macOS, which always
   advertise the bit (`linux/host.c:3876`, `macos/host.c:4722`). **This is M0.**
2. **Append the four callbacks** with Linux, macOS and fake implementations plus provider tests. No caller
   yet. `HL_HOST_FILE_ABI` 23 → 24; `hl_valid_file_group` prefix check unchanged.
3. **Introduce `jail_resolve`** returning `hl_host_file_resolution`, implemented as §6.2(a) over
   `resolve_beneath`, with `jail_at` retained and *both* asserted equal under a debug build. Run the compat
   corpus with the assertion on. This is where the `..` divergence either shows up or is proved absent.
4. **Migrate Group L** (18 sites) to `jail_resolve` + typed leaf ops. Delete `jail_at`'s native return once
   the last caller is gone. Retire the dual lane in `jail_open_plan` (`resolve.c:412`).
5. **Migrate Group A** (24 sites) to `g_root_handle`-rooted `open_beneath`. This unifies bare and container
   routing and makes `jail_routed_at` unconditional — a real behaviour change on Linux, because absolute
   paths in bare mode start going through the confining walk. **Expect fallout**; this is the step that
   needs the corpus, not review.
6. **Groups D, F, N, B.** `read_directory`, `filesystem_metadata`, and the engine-side maintenance paths.
   `state.c`'s permission transaction is blocked on the missing xattr group and should be sequenced last or
   split out.
7. Only then: `posix_attachment` becomes unreferenced outside `binding.c:1879-1901`
   (`bound_attachment_borrow`, SCM_RIGHTS), which already degrades to `-EOPNOTSUPP` when the group is
   absent (`binding.c:1891-1892`).

---

## 7. Risk and cost

### Size

| Step | Files | Estimate |
|---|---|---|
| 1 (M0) | `container/vfs.c` | ~15 lines |
| 2 | `host_services.h`, `core/host_services.c`, `host/{linux,macos,fake}/host.c`, `tests/unit` | ~450 lines |
| 3 | `container/vfs/resolve.c` | ~250 lines |
| 4 | `syscall/fs.c`, `container/vfs/resolve.c` | ~700 lines touched |
| 5 | `syscall/fs.c`, `container/route.c`, `syscall/proc.c` | ~600 lines touched |
| 6 | `syscall/fs.c`, `container/vfs.c`, `container/state.c`, `container/vfs/overlay.c`, `linux_abi/x86.c`, `core/target/*.c`, `core/activation.c` | ~500 lines touched |

Roughly **2.5 kLOC touched** for the whole lane, of which **~15 lines** unblock init and **~350** get a
dynamically linked bare guest to the point where Surface 3 becomes the blocker instead.

This does **not** include Surface 3 (§2.1) — the ~900 ambient descriptor operations and the guest-fd-equals-
host-fd identity. A Windows host needs a synthetic descriptor-number allocator to replace
`bound_shadow_reserve`'s `fcntl(F_DUPFD_CLOEXEC)` (`binding.c:997`), and every ambient `read`/`write`/
`close`/`fcntl` in `src/linux_abi` has to go through `g_linux_box`. That is a separate, larger work item and
it is the true schedule driver for Windows. **It is not estimated here.**

Note also that `syscall/fs.c`, `container/vfs.c`, `syscall/binding.c` and `container/vfs/resolve.c` are not
separately compiled: they are `#include`d into `src/core/target/{x86_64,aarch64}.c`
(`x86_64.c:118,690,721,751`). Only the portable half of `src/linux_abi` is a real library
(`CMakeLists.txt:183-194` — `open_plan.c`, `linux_abi.c`, `image.c`, `fdcache.c`, …). A Windows build cannot
port these files incrementally; the whole unity TU has to compile at once. That argues for doing steps 1–6
on Linux first and only then attempting a Windows compile.

### What could regress on Linux/macOS, and how it is caught

1. **`..` across a bind-mount boundary** (§6.2). `resolve.c:224-239` crosses; `host/resolve.c:158-165`
   clamps. A wholesale swap silently redirects `/data/..` to stay inside `/data`. Caught by: the
   differential assertion in step 3, and by the compat corpus's volume cases. **This is the single most
   likely correctness regression.**
2. **Metadata-storm performance.** The dentry cache (`resolve.c:144-188`) and the openat memo
   (`fs.c:3417-3446`) are measured wins — `fdcache.c:704-707` explicitly calls the generation poll a
   per-syscall hot path. Routing through `resolve_beneath` adds an allocation per component
   (`host/resolve.c:35,62,121` all `malloc`/`strdup`) where `resolve_at` is entirely stack-local
   (`resolve.c:84`, "Fully stack-local (fds[] + buffers) -> thread-safe"). Caught by: `perf-linux` /
   `perf-macos` p99 gates. **Not** caught by `ctest -L unit` or the compat corpus. The fix, if needed, is to
   make `hl_host_resolve_beneath` stack-local too — it is host-internal and nothing depends on its
   allocation behaviour.
3. **TOCTOU window.** `resolve.c:79-83` states the guarantee: the caller's `openat(pfd, final, O_NOFOLLOW)`
   is atomic against a concurrent symlink swap because `pfd` is a held descriptor. `resolve_beneath` +
   `open_relative` preserves this *only* while the returned `parent` handle stays pinned. Any migration
   that round-trips through a path string (e.g. via `file->path`) reintroduces the window. Caught by: not
   reliably by anything in the tree. This needs a targeted test, and it should be written before step 4, not
   after.
4. **Step 5 changes bare-mode behaviour on Linux.** Absolute paths currently reach the host root untouched
   (`route.c:59` — `if (g_rootfs)` guards all rewriting). Routing them through `open_beneath(g_root_handle,
   …)` re-roots them at `/`, which should be identity, but `open_beneath` refuses to follow a final symlink
   out of the root and `resolve_beneath` caps symlinks at 40. A bare guest reading a host path through 41
   symlinks changes answer. Caught by: the compat corpus, if it has such a case; unknown whether it does.
5. **`HL_HOST_FILE_ABI` 23 → 24.** Any out-of-tree backend pinned to 23 stops validating.
   `src/core/host_services.c:57` compares `size >= sizeof(*services->file)`, so a *smaller* struct is
   rejected — appending is safe for the engine but not for a third-party backend compiled against the old
   header. `pkgs/rust` archives embed the header; check `cmake/ArchiveSources.cmake` before bumping.

### Test surface that exists today

`ctest -L unit` (114 cases, `cmake/Phase3Units.cmake:235-257`), with `tests/unit/test_resolve.c` (97 lines)
and `tests/unit/test_resolve_services.c` (162 lines) already covering `hl_host_resolve_beneath` and the
typed `resolve_beneath`/`open_beneath` callbacks directly. `tests/unit/linux.c:252-258` and
`tests/unit/macos.c:167-173` are the only tests that exercise `posix_attachment`; both would need a
capability-conditional arm. The compat corpus is `ctest -L compat` (`cmake/Phase3Compat.cmake`), 1148 active
exact-golden cases across 16 manifests per DOCS.md §12.

---

## 8. Unknowns, stated

- **Whether the compat corpus contains a `..`-across-a-bind-mount case.** If it does not, risk 1 is
  uncaught and step 3's differential assertion is the only guard.
- **Whether `resolve_beneath`'s per-component `malloc` is measurable.** Risk 2 is reasoned from the code,
  not measured. `perf-linux` would answer it in one run and that run should happen before step 4, not after.
- **Whether the 900-ish Surface 3 figure is close.** The regex has false positives (struct members named
  `read`/`write` survive the `[^_A-Za-z0-9.>]` guard in some spellings). The order of magnitude is
  defensible; the number is not.
- **What `container_populate_dev` (`vfs.c:6505-6540`) needs.** It uses ambient `mkdir`/`open`/`symlink` on
  absolute host paths and is not in any of the seven groups because it makes no `*at` call. It runs only for
  containers (`x86_64.c:802`), so it does not affect the boot-critical set, but it is unported and
  unaccounted for.
- **Whether `linkat(AT_EMPTY_PATH)` needs a callback at all.** `fs.c:1903-1930` is already a working
  fallback for hosts without it; whether the fallback is reached on Linux today, or only on macOS, was not
  determined.
