//! The integration suite's single host-platform boundary.
//!
//! `src/sys` is the crate's equivalent and this module deliberately mirrors its shape, but it cannot
//! *be* it: every file under `tests/` compiles as its own crate, so `pub(crate)` items in `hl-engine`
//! are invisible here. The alternative -- widening `sys::symlink` and friends to `pub` -- would put
//! host primitives into the crate's published API purely to serve its own tests, which is the exact
//! leak `sys/mod.rs` exists to prevent (it exports precisely one public item, and only because
//! `transport::Channel::from_stream` names it in a signature).
//!
//! So the fork is duplicated rather than shared, and kept to two functions. Both have the same rule
//! as `src/sys`: a Windows arm is never allowed to be a silent no-op. `is_socket` returning a
//! hard `false` there is the model -- refuse visibly rather than let a check quietly stop running.

// This file is pulled in with `#[path]` by several test binaries and each one compiles a private
// copy. A binary that uses only `symlink` would otherwise warn about `open_handles`, and vice versa.
#![allow(dead_code)]

use std::{io, path::Path};

/// Creates a symbolic link at `link` pointing at `target`.
///
/// Fallible on purpose, and callers must handle the failure rather than `unwrap` it: see the Windows
/// arm for the privilege that is not granted by default there.
#[cfg(unix)]
pub fn symlink(target: &Path, link: &Path) -> io::Result<()> {
    std::os::unix::fs::symlink(target, link)
}

/// Creates a symbolic link at `link` pointing at `target`.
///
/// Windows needs Developer Mode or `SeCreateSymbolicLinkPrivilege`, and the bare failure is
/// `ERROR_PRIVILEGE_NOT_HELD` (1314), which names nothing a reader can act on. The two calls are
/// distinct here because the reparse point records the kind, so the target is resolved the way the
/// guest would resolve it -- relative to the directory the link sits in -- to decide which to use.
#[cfg(windows)]
pub fn symlink(target: &Path, link: &Path) -> io::Result<()> {
    const ERROR_PRIVILEGE_NOT_HELD: i32 = 1314;
    let resolved = link
        .parent()
        .map_or_else(|| target.to_path_buf(), |parent| parent.join(target));
    let result = if resolved.is_dir() {
        std::os::windows::fs::symlink_dir(target, link)
    } else {
        std::os::windows::fs::symlink_file(target, link)
    };
    result.map_err(|error| {
        if error.raw_os_error() == Some(ERROR_PRIVILEGE_NOT_HELD) {
            return io::Error::new(
                io::ErrorKind::PermissionDenied,
                "creating a symbolic link needs Developer Mode (Settings > System > For \
                 developers) or SeCreateSymbolicLinkPrivilege",
            );
        }
        error
    })
}

/// How many kernel objects this process currently holds open.
///
/// Used as a before/after pair to catch a launch that leaks one. The number itself is meaningless
/// across hosts and is never compared to a constant -- only to another reading of itself.
#[cfg(target_os = "linux")]
#[must_use]
pub fn open_handles() -> usize {
    std::fs::read_dir("/proc/self/fd")
        .expect("read /proc/self/fd")
        .count()
}

/// As above. macOS has no `/proc`; `/dev/fd` is the same directory by another name.
#[cfg(target_os = "macos")]
#[must_use]
pub fn open_handles() -> usize {
    std::fs::read_dir("/dev/fd").expect("read /dev/fd").count()
}

/// As above, for Windows.
///
/// A descriptor count has no counterpart there, but the *defect* being watched for does: a launch
/// that forgets to close a handle leaks a kernel object exactly as it would leak an fd.
/// `GetProcessHandleCount` is the direct reading of that. Answering a constant instead would make
/// the leak assertion compare `0 == 0` and pass against a launch that leaks every handle it opens.
#[cfg(windows)]
#[must_use]
#[allow(unsafe_code)] // The suite's only FFI call; `std` exposes no handle count.
pub fn open_handles() -> usize {
    let mut count = 0_u32;
    // SAFETY: `GetCurrentProcess` returns a pseudo-handle that needs no release, and
    // `GetProcessHandleCount` writes one `u32` through the pointer it is given.
    let ok = unsafe { GetProcessHandleCount(GetCurrentProcess(), &raw mut count) };
    assert!(ok != 0, "GetProcessHandleCount failed");
    usize::try_from(count).unwrap_or(usize::MAX)
}

#[cfg(windows)]
#[allow(unsafe_code)] // As above.
#[link(name = "kernel32")]
unsafe extern "system" {
    fn GetCurrentProcess() -> isize;
    fn GetProcessHandleCount(process: isize, count: *mut u32) -> i32;
}
